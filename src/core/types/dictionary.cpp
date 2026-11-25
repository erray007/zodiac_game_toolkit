// Zodiac's dictionary (see dictionary.hpp for the contract): the value type, the insertion-ordered hash table, the text
// format of serialize() and deserialize(), and the registration.
#include <core/types/dictionary.hpp>
#include <core/text.hpp>
#include <core/types/array.hpp>
#include <core/types/heap.hpp>
#include <core/types/script_guard.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cfloat>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace zodiac {
// The array element types the text format knows, in the order of array_tag_names.
enum class ArrayTag:std::uint8_t {boolean,int8,int16,int32,int64,uint8,uint16,uint32,uint64,float32,float64,string,dictionary_handle,dictionary_object,count};

// Per engine (engine user data): the registered types, and the script-facing functions that need the engine (the
// factories, reached through generic wrappers, and the proxy's methods, bound with this object as their `this` through
// asCALL_THISCALL_OBJLAST).
struct DictionaryCache {
asIScriptEngine* engine=nullptr;
asITypeInfo* dictionary_type=nullptr;
asITypeInfo* string_type=nullptr;
asITypeInfo* string_array_type=nullptr;
int dictionary_type_id=0;
int value_type_id=0;
int string_type_id=0;
// array<string>'s ScriptArray::cache_for, fetched by the first keys(), so later ones skip the lookup under the engine's lock.
std::atomic<ArrayCache*> string_array_cache{nullptr};
// array<T> of each tag, looked up when deserialize() first needs it.
std::atomic<asITypeInfo*> arrays[static_cast<int>(ArrayTag::count)]={};

asITypeInfo* array_type(ArrayTag tag) noexcept;
Dictionary* make_dictionary() noexcept;
Dictionary* make_from_list(void* list) noexcept;
DictionaryValue& value_assign(const void* ref,int type_id,DictionaryValue* self) noexcept;
DictionaryValue& value_assign_value(const DictionaryValue& other,DictionaryValue* self) noexcept;
DictionaryValue& value_assign_integer(std::int64_t value,DictionaryValue* self) noexcept;
DictionaryValue& value_assign_number(double value,DictionaryValue* self) noexcept;
void value_read(void* out,int type_id,const DictionaryValue* self) noexcept;
std::int64_t value_integer(const DictionaryValue* self) noexcept;
double value_number(const DictionaryValue* self) noexcept;
};

// 64 bytes on a 64-byte boundary (segments come from the aligned operator new), so a lookup's key compare and value read
// touch one cache line, not the two most entries of a malloc'ed segment straddled.
struct alignas(64) Dictionary::Entry {
String key;
DictionaryValue value;
// Insertion-order links of a live entry (no_entry at the ends); free_mark or pending_mark otherwise.
std::uint32_t prev;
// The next live entry, or the next free entry of the free list.
std::uint32_t next;
};
namespace {
static_assert(sizeof(DictionaryValue)==24,"dictionary_value is registered with DictionaryValue's layout");
// Engine user data slot of the cache (Zodiac's own slots spell their owner).
constexpr asPWORD dictionary_cache_slot=0x5A444943;
constexpr std::uint32_t no_entry=0xFFFFFFFFu;
constexpr std::uint32_t free_mark=0xFFFFFFFEu;
// An entry clear() has unlinked and not yet released.
constexpr std::uint32_t pending_mark=0xFFFFFFFDu;
constexpr std::uint32_t first_segment_size=16;
constexpr std::uint32_t min_index_size=16;
constexpr std::uint32_t max_entries=1u<<30;
// Characters of a key a message shows before "...".
constexpr int max_shown=64;
constexpr std::string_view header="zodiac-dictionary 1";
constexpr const char* array_tag_names[]={"array<bool>","array<int8>","array<int16>","array<int>","array<int64>","array<uint8>","array<uint16>","array<uint>","array<uint64>","array<float>","array<double>","array<string>","array<dictionary@>","array<dictionary>"};
static_assert(std::size(array_tag_names)==static_cast<std::size_t>(ArrayTag::count));

void* load_pointer(const void* slot) noexcept
{
void* pointer;
std::memcpy(&pointer,slot,sizeof pointer);
return pointer;
}
void store_pointer(void* slot,void* pointer) noexcept
{
std::memcpy(slot,&pointer,sizeof pointer);
}
// Raises text unless the active context already holds an exception (a failed copy's own) or is being aborted.
void raise_unless_pending(const char* text) noexcept
{
asIScriptContext* context=asGetActiveContext();
if(!context)return;
const asEContextState state=context->GetState();
if(state==asEXECUTION_EXCEPTION||state==asEXECUTION_ABORTED)return;
raise_script_exception(text);
}
// "<key>" for messages: one line, at most max_shown characters followed by "...".
std::string shown_key(const String& key)
{
String shown(one_line(key.bytes()));
const bool cut=shown.length()>max_shown;
if(cut)shown=*shown.left(max_shown);
std::string text;
text.reserve(static_cast<std::size_t>(shown.byte_length())+5);
text.push_back('"');
text.append(shown.bytes());
if(cut)text.append("...");
text.push_back('"');
return text;
}
// A type as scripts write it, templates spelled out ("array<Item@>", where the engine says "Item@[]").
std::string declaration_of(asIScriptEngine* engine,int type_id)
{
const asITypeInfo* type=engine->GetTypeInfoById(type_id);
if(!type||type->GetSubTypeCount()==0){
const char* text=engine->GetTypeDeclaration(type_id,true);
return text?text:"?";
}
std::string text;
if(type_id&asTYPEID_HANDLETOCONST)text.append("const ");
if(const char* space=type->GetNamespace();space&&*space)text.append(space).append("::");
text.append(type->GetName()).push_back('<');
for(asUINT at=0;at<type->GetSubTypeCount();++at){
if(at)text.push_back(',');
text.append(declaration_of(engine,type->GetSubTypeId(at)));
}
text.push_back('>');
if(type_id&asTYPEID_OBJHANDLE)text.push_back('@');
return text;
}
// A String object as the engine allocates value types (asAllocMem), so ReleaseScriptObject frees it.
void* new_string(const String& text)
{
void* memory=asAllocMem(sizeof(String));
if(!memory)throw std::bad_alloc();
try{
return new(memory) String(text);
}
catch(...){
asFreeMem(memory);
throw;
}
}
void* new_string(String&& text)
{
void* memory=asAllocMem(sizeof(String));
if(!memory)throw std::bad_alloc();
return new(memory) String(std::move(text));
}
bool counted_type(const asITypeInfo* type) noexcept
{
const asQWORD flags=type->GetFlags();
return (flags&asOBJ_REF)&&!(flags&asOBJ_NOCOUNT);
}
// Reads a primitive of type_id (an integer type, or an enum's underlying type) as int64; false for other types.
bool read_integer(const void* ref,int type_id,std::int64_t& value) noexcept
{
switch(type_id){
case asTYPEID_INT8:{std::int8_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_INT16:{std::int16_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_INT32:{std::int32_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_INT64:{std::memcpy(&value,ref,sizeof value);return true;}
case asTYPEID_UINT8:{std::uint8_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_UINT16:{std::uint16_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_UINT32:{std::uint32_t v;std::memcpy(&v,ref,sizeof v);value=v;return true;}
case asTYPEID_UINT64:{std::uint64_t v;std::memcpy(&v,ref,sizeof v);value=static_cast<std::int64_t>(v);return true;}
default:return false;
}
}
// A double truncated toward zero into T, saturating at T's limits; NaN is 0.
template<class T>
T saturate(double value) noexcept
{
if(std::isnan(value))return 0;
// 2^digits is the first value above T's maximum, and exact as a double; T's minimum (0 or -2^digits) is exact too.
const double high=std::ldexp(1.0,std::numeric_limits<T>::digits);
const double low=static_cast<double>(std::numeric_limits<T>::min());
if(value>=high)return std::numeric_limits<T>::max();
if(value<=low)return std::numeric_limits<T>::min();
return static_cast<T>(value);
}
template<class T>
void write_number(void* out,std::int64_t integer,double number,bool is_double) noexcept
{
T value;
if constexpr(std::is_floating_point_v<T>){
value=is_double?static_cast<T>(number):static_cast<T>(integer);
}else{
value=is_double?saturate<T>(number):static_cast<T>(integer);
}
std::memcpy(out,&value,sizeof value);
}
[[gnu::cold]] void raise_copy_failure(asIScriptEngine* engine,int type_id) noexcept
{
try{
raise_unless_pending(("cannot copy a value of type "+declaration_of(engine,type_id)).c_str());
}
catch(...){
raise_unless_pending("out of memory");
}
}
}

// ---- DictionaryValue ----

DictionaryValue& DictionaryValue::operator=(DictionaryValue&& other) noexcept
{
// The old content is released last, after this value holds the new one.
DictionaryValue taken(std::move(other));
swap(taken);
return *this;
}

void DictionaryValue::swap(DictionaryValue& other) noexcept
{
std::swap(payload_,other.payload_);
std::swap(type_,other.type_);
std::swap(type_id_,other.type_id_);
}

void DictionaryValue::release() noexcept
{
void* object=payload_.object;
asITypeInfo* type=type_;
forget();
if(object)type->GetEngine()->ReleaseScriptObject(object,type);
}

void DictionaryValue::reset() noexcept
{
if(type_)release();
else forget();
}

void DictionaryValue::set_integer(std::int64_t value) noexcept
{
DictionaryValue fresh;
fresh.payload_.integer=value;
fresh.type_id_=asTYPEID_INT64;
swap(fresh);
}

void DictionaryValue::set_number(double value) noexcept
{
DictionaryValue fresh;
fresh.payload_.number=value;
fresh.type_id_=asTYPEID_DOUBLE;
swap(fresh);
}

void DictionaryValue::set_boolean(bool value) noexcept
{
DictionaryValue fresh;
fresh.payload_.integer=value?1:0;
fresh.type_id_=asTYPEID_BOOL;
swap(fresh);
}

void DictionaryValue::adopt(void* object,asITypeInfo* type,int type_id) noexcept
{
DictionaryValue fresh;
fresh.payload_.object=object;
fresh.type_=type;
fresh.type_id_=type_id;
swap(fresh);
}

bool DictionaryValue::counted_object() const noexcept
{
return type_&&!(type_id_&asTYPEID_OBJHANDLE)&&counted_type(type_);
}

bool DictionaryValue::make_object_copy(void* object,asITypeInfo* type,int type_id,const DictionaryCache& cache)
{
void* copy;
if(type==cache.string_type){
copy=new_string(*static_cast<const String*>(object));
}else{
// Script code run by the copy may release the original (by changing the dictionary holding it).
const bool counted=counted_type(type);
if(counted)cache.engine->AddRefScriptObject(object,type);
copy=cache.engine->CreateScriptObjectCopy(object,type);
if(counted)cache.engine->ReleaseScriptObject(object,type);
if(!copy){
raise_copy_failure(cache.engine,type_id);
return false;
}
}
payload_.object=copy;
type_=type;
type_id_=type_id;
return true;
}

bool DictionaryValue::make_copy(const DictionaryValue& other,const DictionaryCache& cache)
{
if(!other.type_){
payload_=other.payload_;
type_id_=other.type_id_;
return true;
}
if(other.type_id_&asTYPEID_OBJHANDLE){
if(other.payload_.object)cache.engine->AddRefScriptObject(other.payload_.object,other.type_);
payload_=other.payload_;
type_=other.type_;
type_id_=other.type_id_;
return true;
}
return make_object_copy(other.payload_.object,other.type_,other.type_id_,cache);
}

bool DictionaryValue::snapshot(const DictionaryValue& other,const DictionaryCache& cache)
{
if(!other.counted_object())return make_copy(other,cache);
cache.engine->AddRefScriptObject(other.payload_.object,other.type_);
payload_=other.payload_;
type_=other.type_;
type_id_=other.type_id_;
return true;
}

bool DictionaryValue::make(const void* ref,int type_id,const DictionaryCache& cache)
{
if(!(type_id&asTYPEID_MASK_OBJECT)){
switch(type_id){
case asTYPEID_VOID:
// The null literal: an empty value.
return true;
case asTYPEID_BOOL:{
bool value;
std::memcpy(&value,ref,sizeof value);
payload_.integer=value?1:0;
type_id_=asTYPEID_BOOL;
return true;
}
case asTYPEID_FLOAT:{
float value;
std::memcpy(&value,ref,sizeof value);
payload_.number=value;
type_id_=asTYPEID_DOUBLE;
return true;
}
case asTYPEID_DOUBLE:
std::memcpy(&payload_.number,ref,sizeof payload_.number);
type_id_=asTYPEID_DOUBLE;
return true;
default:
break;
}
std::int64_t value=0;
if(!read_integer(ref,type_id,value)){
// An enum: its underlying integer type.
asITypeInfo* type=cache.engine->GetTypeInfoById(type_id);
if(!type||!read_integer(ref,type->GetUnderlyingTypeId(),value)){
raise_script_exception(("cannot store a value of type "+declaration_of(cache.engine,type_id)).c_str());
return false;
}
}
payload_.integer=value;
type_id_=asTYPEID_INT64;
return true;
}
if(type_id==cache.value_type_id)return make_copy(*static_cast<const DictionaryValue*>(ref),cache);
asITypeInfo* type=cache.engine->GetTypeInfoById(type_id);
if(type_id&asTYPEID_OBJHANDLE){
void* object=load_pointer(ref);
if(object)cache.engine->AddRefScriptObject(object,type);
payload_.object=object;
type_=type;
type_id_=type_id;
return true;
}
return make_object_copy(const_cast<void*>(ref),type,type_id,cache);
}

bool DictionaryValue::assign(const void* ref,int type_id,const DictionaryCache& cache)
{
DictionaryValue fresh;
if(!fresh.make(ref,type_id,cache))return false;
swap(fresh);
return true;
}

bool DictionaryValue::assign(const DictionaryValue& other,const DictionaryCache& cache)
{
if(this==&other)return true;
DictionaryValue fresh;
if(!fresh.make_copy(other,cache))return false;
swap(fresh);
return true;
}

bool DictionaryValue::get(void* out,int type_id,const DictionaryCache& cache) const
{
if(type_id&asTYPEID_OBJHANDLE){
if(!type_)return false;
// A handle to a const object only reads into a handle to const.
if((type_id_&asTYPEID_HANDLETOCONST)&&!(type_id&asTYPEID_HANDLETOCONST))return false;
void* result=nullptr;
if(payload_.object){
cache.engine->RefCastObject(payload_.object,type_,cache.engine->GetTypeInfoById(type_id),&result);
if(!result)return false;
}
store_pointer(out,result);
return true;
}
if(type_id&asTYPEID_MASK_OBJECT){
if(type_id==cache.value_type_id)return static_cast<DictionaryValue*>(out)->assign(*this,cache);
if(!type_||!payload_.object)return false;
if((type_id_&~(asTYPEID_OBJHANDLE|asTYPEID_HANDLETOCONST))!=type_id)return false;
if(type_==cache.string_type){
*static_cast<String*>(out)=*static_cast<const String*>(payload_.object);
return true;
}
// The object's opAssign may run script code that releases this value: the object is held meanwhile.
void* object=payload_.object;
asITypeInfo* type=type_;
const bool counted=counted_type(type);
if(counted)cache.engine->AddRefScriptObject(object,type);
const int result=cache.engine->AssignScriptObject(out,object,type);
if(counted)cache.engine->ReleaseScriptObject(object,type);
return result>=0;
}
if(type_||type_id_==0)return false;
int target=type_id;
if(target>asTYPEID_DOUBLE){
asITypeInfo* type=cache.engine->GetTypeInfoById(target);
if(!type||!(type->GetFlags()&asOBJ_ENUM))return false;
target=type->GetUnderlyingTypeId();
}
const bool is_double=type_id_==asTYPEID_DOUBLE;
const std::int64_t integer=payload_.integer;
const double number=payload_.number;
switch(target){
case asTYPEID_BOOL:{
const bool value=is_double?number!=0:integer!=0;
std::memcpy(out,&value,sizeof value);
return true;
}
case asTYPEID_INT8:write_number<std::int8_t>(out,integer,number,is_double);return true;
case asTYPEID_INT16:write_number<std::int16_t>(out,integer,number,is_double);return true;
case asTYPEID_INT32:write_number<std::int32_t>(out,integer,number,is_double);return true;
case asTYPEID_INT64:write_number<std::int64_t>(out,integer,number,is_double);return true;
case asTYPEID_UINT8:write_number<std::uint8_t>(out,integer,number,is_double);return true;
case asTYPEID_UINT16:write_number<std::uint16_t>(out,integer,number,is_double);return true;
case asTYPEID_UINT32:write_number<std::uint32_t>(out,integer,number,is_double);return true;
case asTYPEID_UINT64:write_number<std::uint64_t>(out,integer,number,is_double);return true;
case asTYPEID_FLOAT:write_number<float>(out,integer,number,is_double);return true;
case asTYPEID_DOUBLE:write_number<double>(out,integer,number,is_double);return true;
default:return false;
}
}

void DictionaryValue::enum_references(asIScriptEngine* engine) noexcept
{
if(!type_||!payload_.object)return;
const asQWORD flags=type_->GetFlags();
if(flags&asOBJ_VALUE){
// Value types only hold references when they are garbage collected themselves.
if(flags&asOBJ_GC)engine->ForwardGCEnumReferences(payload_.object,type_);
return;
}
engine->GCEnumCallback(payload_.object);
}

void DictionaryValue::release_references(asIScriptEngine*) noexcept
{
reset();
}

// ---- Dictionary: storage ----

namespace {
// Per-thread caches of the blocks a small dictionary is made of (Task 5c fix round 1): the Dictionary object, its first segment
// (first_segment_size entries) and an index of min_index_size or twice that many slots (up to 16 keys). A script that makes and drops
// small dictionaries in a loop (a dictionary per frame) then never reaches the C heap for them. On the Segment Heap, which games run on
// (src/platform/windows/application.manifest), those three allocations made dict_insert_10 4 % slower than on the NT heap
// (placement-study 45563514749). Any thread may free any dictionary: a block goes to the cache of the thread that frees it, as ScriptArray
// objects do (array.cpp). Under AddressSanitizer the caches step aside, so it sees every block.
constexpr bool dictionary_cache_on=!heap::under_address_sanitizer;
constexpr int dictionary_block_limit=16;
// Entry's alignment (asserted where Entry is complete, in the constructor).
constexpr std::size_t entry_alignment=64;
enum class DictionaryBlock {object,segment,small_index,index,count};
constexpr int dictionary_block_kinds=static_cast<int>(DictionaryBlock::count);
// Trivially destructible members, so the caches stay usable after the thread's cache is destroyed (dictionaries freed by later destructors).
struct DictionaryBlocks {
void* blocks[dictionary_block_kinds][dictionary_block_limit];
int counts[dictionary_block_kinds];
~DictionaryBlocks();
};
thread_local bool dictionary_blocks_closed=false;
thread_local DictionaryBlocks dictionary_blocks{};
void release_dictionary_block(DictionaryBlock kind,void* block) noexcept
{
switch(kind){
case DictionaryBlock::object:
heap::release_aligned(block,alignof(Dictionary));
break;
case DictionaryBlock::segment:
heap::release_aligned(block,entry_alignment);
break;
default:
heap::release(block);
break;
}
}
DictionaryBlocks::~DictionaryBlocks()
{
dictionary_blocks_closed=true;
for(int kind=0;kind<dictionary_block_kinds;++kind){
while(counts[kind]>0)release_dictionary_block(static_cast<DictionaryBlock>(kind),blocks[kind][--counts[kind]]);
}
}
// A cached block of kind, or nullptr.
void* take_dictionary_block(DictionaryBlock kind) noexcept
{
if(!dictionary_cache_on||dictionary_blocks_closed)return nullptr;
DictionaryBlocks& cache=dictionary_blocks;
int& count=cache.counts[static_cast<int>(kind)];
return count>0?cache.blocks[static_cast<int>(kind)][--count]:nullptr;
}
// Caches block, or releases it when the cache is full or closed.
void give_dictionary_block(DictionaryBlock kind,void* block) noexcept
{
if(dictionary_cache_on&&!dictionary_blocks_closed){
DictionaryBlocks& cache=dictionary_blocks;
int& count=cache.counts[static_cast<int>(kind)];
if(count<dictionary_block_limit){
cache.blocks[static_cast<int>(kind)][count++]=block;
return;
}
}
release_dictionary_block(kind,block);
}
// The cache that holds an index of size slots, if any.
inline bool cached_index(std::uint32_t size,DictionaryBlock& kind) noexcept
{
if(size==min_index_size)kind=DictionaryBlock::small_index;
else if(size==2*min_index_size)kind=DictionaryBlock::index;
else return false;
return true;
}
void release_index(void* index,std::uint32_t size) noexcept
{
if(!index)return;
DictionaryBlock kind{};
if(cached_index(size,kind))give_dictionary_block(kind,index);
else heap::release(index);
}
}

Dictionary::Dictionary(DictionaryCache& cache) noexcept:cache_(&cache),free_(no_entry),first_(no_entry),last_(no_entry)
{
static_assert(sizeof(Entry)==64&&sizeof(Slot)==16&&alignof(Entry)==entry_alignment);
}

Dictionary::~Dictionary()
{
// Nothing can reach a dictionary being destroyed, so the values are released in place.
release_index(index_,index_?mask_+1:0);
for(int segment=0;segment<segment_count&&segments_[segment];++segment){
Entry* block=segments_[segment];
const std::uint32_t size=first_segment_size<<segment;
for(std::uint32_t at=0;at<size;++at)block[at].~Entry();
if(segment==0)give_dictionary_block(DictionaryBlock::segment,block);
else heap::release_aligned(block,alignof(Entry));
}
}

void* Dictionary::operator new(std::size_t size)
{
if(size==sizeof(Dictionary)){
if(void* block=take_dictionary_block(DictionaryBlock::object))return block;
}
return heap::allocate_aligned_or_throw(size,alignof(Dictionary));
}

void Dictionary::operator delete(void* block) noexcept
{
// Every Dictionary is sizeof(Dictionary) (the class has no derived types), so every block fits the cache.
if(block)give_dictionary_block(DictionaryBlock::object,block);
}

Dictionary* Dictionary::create(DictionaryCache& cache)
{
auto* dictionary=new Dictionary(cache);
cache.engine->NotifyGarbageCollectorOfNewObject(dictionary,cache.dictionary_type);
return dictionary;
}

void Dictionary::add_ref() const noexcept
{
gc_flag_=false;
asAtomicInc(ref_count_);
}

void Dictionary::release() const noexcept
{
gc_flag_=false;
if(asAtomicDec(ref_count_)==0)delete const_cast<Dictionary*>(this);
}

Dictionary::Entry& Dictionary::entry(std::uint32_t index) const noexcept
{
// Segment s holds first_segment_size << s entries, starting at first_segment_size * (2^s - 1).
const unsigned segment=static_cast<unsigned>(std::bit_width((index/first_segment_size)+1))-1;
return segments_[segment][index-first_segment_size*((1u<<segment)-1)];
}

void Dictionary::add_segment()
{
const unsigned segment=static_cast<unsigned>(std::bit_width((capacity_/first_segment_size)+1))-1;
if(segment>=static_cast<unsigned>(segment_count))throw std::bad_alloc();
const std::uint32_t size=first_segment_size<<segment;
auto* block=static_cast<Entry*>(segment==0?take_dictionary_block(DictionaryBlock::segment):nullptr);
if(!block)block=static_cast<Entry*>(heap::allocate_aligned(sizeof(Entry)*size,alignof(Entry)));
if(!block)throw std::bad_alloc();
for(std::uint32_t at=0;at<size;++at)new(block+at) Entry{String(),DictionaryValue(),free_mark,no_entry};
segments_[segment]=block;
capacity_+=size;
}

void Dictionary::rehash(std::uint32_t size)
{
Slot* slots=nullptr;
DictionaryBlock kind{};
if(cached_index(size,kind)){
if(void* block=take_dictionary_block(kind)){
std::memset(block,0,std::size_t{size}*sizeof(Slot));
slots=static_cast<Slot*>(block);
}
}
if(!slots)slots=static_cast<Slot*>(heap::allocate_zeroed(size,sizeof(Slot)));
if(!slots)throw std::bad_alloc();
Slot* const old=index_;
const std::uint32_t old_size=old?mask_+1:0;
// The new mask is in place before the slots move, so home() places them in the new index.
index_=slots;
mask_=size-1;
for(std::uint32_t at=0;at<old_size;++at){
const Slot& slot=old[at];
if(!slot.entry)continue;
std::uint32_t position=home(slot.hash);
while(slots[position].entry)position=(position+1)&mask_;
slots[position]=slot;
}
release_index(old,old_size);
}

void Dictionary::reserve(std::uint32_t count)
{
const std::uint64_t needed=std::uint64_t{live_}+count;
if(needed>max_entries)throw std::bad_alloc();
// The index stays at most half full.
const std::uint64_t size=index_?std::uint64_t{mask_}+1:0;
if(needed*2>size){
std::uint64_t grown=size?size*2:min_index_size;
while(grown<needed*2)grown*=2;
rehash(static_cast<std::uint32_t>(grown));
}
while(std::uint64_t{capacity_}-used_+free_count_<count)add_segment();
}

std::uint32_t Dictionary::find_position(const String& key,std::uint64_t hash) const noexcept
{
if(!index_)return no_entry;
for(std::uint32_t position=home(hash);;position=(position+1)&mask_){
const Slot& slot=index_[position];
if(!slot.entry)return no_entry;
// The bytes are compared inline (no C runtime memcmp call).
if(slot.hash==hash&&string_detail::same_text(entry(slot.entry-1).key.bytes(),key.bytes()))return position;
}
}

void Dictionary::link_last(std::uint32_t index) noexcept
{
Entry& item=entry(index);
item.prev=last_;
item.next=no_entry;
if(last_!=no_entry)entry(last_).next=index;
else first_=index;
last_=index;
}

void Dictionary::unlink(std::uint32_t index) noexcept
{
Entry& item=entry(index);
if(item.prev!=no_entry)entry(item.prev).next=item.next;
else first_=item.next;
if(item.next!=no_entry)entry(item.next).prev=item.prev;
else last_=item.prev;
}

void Dictionary::link_before(std::uint32_t index,std::uint32_t at) noexcept
{
Entry& item=entry(index);
Entry& after=entry(at);
item.prev=after.prev;
item.next=at;
if(after.prev!=no_entry)entry(after.prev).next=index;
else first_=index;
after.prev=index;
}

void Dictionary::link_after(std::uint32_t index,std::uint32_t at) noexcept
{
Entry& item=entry(index);
Entry& before=entry(at);
item.next=before.next;
item.prev=at;
if(before.next!=no_entry)entry(before.next).prev=index;
else last_=index;
before.next=index;
}

std::uint32_t Dictionary::position_of(std::uint32_t index) const noexcept
{
// Two independent chains, one per direction, so their loads overlap; the first to run out says where the entry is.
const Entry& item=entry(index);
std::uint32_t back=item.prev;
std::uint32_t ahead=item.next;
for(std::uint32_t steps=0;;++steps){
if(back==no_entry)return steps;
if(ahead==no_entry)return live_-1-steps;
back=entry(back).prev;
ahead=entry(ahead).next;
}
}

std::uint32_t Dictionary::walk(std::uint32_t index,std::uint32_t steps,bool backward) const noexcept
{
if(backward){
while(steps--&&index!=no_entry)index=entry(index).prev;
}else{
while(steps--&&index!=no_entry)index=entry(index).next;
}
return index;
}

std::uint32_t Dictionary::entry_at(std::uint32_t position) const noexcept
{
const std::uint32_t from_back=live_-1-position;
return position<=from_back?walk(first_,position,false):walk(last_,from_back,true);
}

bool Dictionary::move_by(const String& key,int delta) noexcept
{
if(delta==0)return false;
const std::uint32_t found=find_position(key,key.table_hash());
if(found==no_entry)return false;
const std::uint32_t index=index_[found].entry-1;
const bool backward=delta<0;
// |delta| without overflow at INT_MIN; a move past either end needs more places than there are other keys.
const std::uint64_t places=backward?std::uint64_t{0}-static_cast<std::uint64_t>(static_cast<std::int64_t>(delta)):static_cast<std::uint64_t>(delta);
if(places>=live_)return false;
const std::uint32_t at=walk(index,static_cast<std::uint32_t>(places),backward);
if(at==no_entry)return false;
unlink(index);
if(backward)link_before(index,at);
else link_after(index,at);
return true;
}

bool Dictionary::move(const String& key,int index) noexcept
{
if(index<0||static_cast<std::uint32_t>(index)>=live_)return false;
const std::uint32_t found=find_position(key,key.table_hash());
if(found==no_entry)return false;
const std::uint32_t item=index_[found].entry-1;
const std::uint32_t target=static_cast<std::uint32_t>(index);
const std::uint32_t from=position_of(item);
if(from==target)return true;
// The entry now at target: from the key itself or from the nearer end, whichever walk is shorter.
const bool backward=target<from;
const std::uint32_t distance=backward?from-target:target-from;
const std::uint32_t from_end=std::min(target,live_-1-target);
const std::uint32_t at=distance<=from_end?walk(item,distance,backward):entry_at(target);
unlink(item);
// Moving back, the key goes before the entry at target; moving on, that entry slides back one place and the key follows it.
if(backward)link_before(item,at);
else link_after(item,at);
return true;
}

int Dictionary::index_of(const String& key) const noexcept
{
const std::uint32_t found=find_position(key,key.table_hash());
return found==no_entry?-1:static_cast<int>(position_of(index_[found].entry-1));
}

const String* Dictionary::key_at(int index) const noexcept
{
if(index<0||static_cast<std::uint32_t>(index)>=live_)return nullptr;
return &entry(entry_at(static_cast<std::uint32_t>(index))).key;
}

void Dictionary::free_entry(std::uint32_t index) noexcept
{
Entry& item=entry(index);
item.prev=free_mark;
item.next=free_;
free_=index;
++free_count_;
}

std::uint32_t Dictionary::insert_new(String&& key,std::uint64_t hash,DictionaryValue& value) noexcept
{
std::uint32_t index;
if(free_!=no_entry){
index=free_;
free_=entry(index).next;
--free_count_;
}else{
index=used_++;
}
Entry& item=entry(index);
item.key=std::move(key);
// A value written through a stale reference after this entry was removed comes back to the caller for release.
item.value.swap(value);
link_last(index);
std::uint32_t position=home(hash);
while(index_[position].entry)position=(position+1)&mask_;
index_[position]=Slot{hash,index+1,0};
++live_;
return index;
}

void Dictionary::erase_position(std::uint32_t position) noexcept
{
// Backward shift: every later slot of the probe run that may move into the hole does, so no tombstones are needed.
std::uint32_t hole=position;
for(std::uint32_t next=(hole+1)&mask_;index_[next].entry;next=(next+1)&mask_){
const std::uint32_t start=home(index_[next].hash);
if(((next-start)&mask_)>=((next-hole)&mask_)){
index_[hole]=index_[next];
hole=next;
}
}
index_[hole]=Slot{};
}

// ---- Dictionary: operations ----

bool Dictionary::exists(const String& key) const noexcept
{
return find_position(key,key.table_hash())!=no_entry;
}

const DictionaryValue* Dictionary::find(const String& key) const noexcept
{
const std::uint32_t position=find_position(key,key.table_hash());
return position==no_entry?nullptr:&entry(index_[position].entry-1).value;
}

DictionaryValue* Dictionary::find(const String& key) noexcept
{
const std::uint32_t position=find_position(key,key.table_hash());
return position==no_entry?nullptr:&entry(index_[position].entry-1).value;
}

void Dictionary::put(const String& key,DictionaryValue& value)
{
const std::uint64_t hash=key.table_hash();
const std::uint32_t position=find_position(key,hash);
if(position!=no_entry){
entry(index_[position].entry-1).value.swap(value);
return;
}
reserve(1);
insert_new(String(key),hash,value);
}

DictionaryValue& Dictionary::at(const String& key)
{
const std::uint64_t hash=key.table_hash();
const std::uint32_t position=find_position(key,hash);
if(position!=no_entry)return entry(index_[position].entry-1).value;
reserve(1);
DictionaryValue stray;
const std::uint32_t index=insert_new(String(key),hash,stray);
return entry(index).value;
}

bool Dictionary::set(const String& key,const void* ref,int type_id)
{
DictionaryValue fresh;
if(!fresh.make(ref,type_id,*cache_))return false;
// fresh receives the previous value, released when it goes out of scope.
put(key,fresh);
return true;
}

void Dictionary::set(const String& key,std::int64_t value)
{
DictionaryValue fresh;
fresh.set_integer(value);
put(key,fresh);
}

void Dictionary::set(const String& key,double value)
{
DictionaryValue fresh;
fresh.set_number(value);
put(key,fresh);
}

bool Dictionary::get(const String& key,void* out,int type_id) const
{
const DictionaryValue* value=find(key);
return value&&value->get(out,type_id,*cache_);
}

bool Dictionary::remove(const String& key) noexcept
{
const std::uint32_t position=find_position(key,key.table_hash());
if(position==no_entry)return false;
const std::uint32_t index=index_[position].entry-1;
erase_position(position);
unlink(index);
Entry& item=entry(index);
String old_key(std::move(item.key));
DictionaryValue old_value(std::move(item.value));
free_entry(index);
--live_;
// The old key and value are released here, after the table is consistent.
return true;
}

void Dictionary::clear() noexcept
{
// First every entry is unlinked, so script code run by the releases below finds an empty, consistent dictionary (and may
// fill it again); the unlinked entries are marked pending and released one by one.
if(index_&&live_)std::memset(index_,0,(std::size_t{mask_}+1)*sizeof(Slot));
for(std::uint32_t index=first_;index!=no_entry;){
Entry& item=entry(index);
const std::uint32_t next=item.next;
item.prev=pending_mark;
item.next=no_entry;
item.key=String();
index=next;
}
first_=no_entry;
last_=no_entry;
live_=0;
++clearing_;
const std::uint32_t end=used_;
for(std::uint32_t index=0;index<end;++index){
Entry& item=entry(index);
if(item.prev==pending_mark){
DictionaryValue old(std::move(item.value));
free_entry(index);
}else if(item.prev==free_mark&&!item.value.empty()){
DictionaryValue stray(std::move(item.value));
}
// A nested clear() (run by a release) handles the pending entries it finds the same way.
}
if(--clearing_==0&&live_==0){
// Start again from the first entry, unless a value was written through a stale reference meanwhile.
for(std::uint32_t index=0;index<used_;++index){
if(!entry(index).value.empty())return;
}
used_=0;
free_=no_entry;
free_count_=0;
}
}

ScriptArray* Dictionary::keys() const noexcept
{
// Each key copy-constructed straight into its slot (no default construction, assignment or engine call per key).
ArrayCache* strings=cache_->string_array_cache.load(std::memory_order_acquire);
if(!strings){
// Threads racing here find the same cache.
strings=ScriptArray::cache_for(cache_->string_array_type);
if(!strings)return nullptr;
cache_->string_array_cache.store(strings,std::memory_order_release);
}
ScriptArray* result=ScriptArray::create_strings(cache_->string_array_type,static_cast<int>(live_),strings);
if(!result)return nullptr;
for(std::uint32_t index=first_;index!=no_entry;index=entry(index).next){
if(!result->append_string(entry(index).key)){
result->release();
return nullptr;
}
}
return result;
}

void Dictionary::commit(Dictionary& source)
{
reserve(source.live_);
// Past clear(), a bad_alloc from reserve(1) can leave the content partly replaced; "unchanged" covers malformed input only.
clear();
for(std::uint32_t index=source.first_;index!=no_entry;index=source.entry(index).next){
Entry& item=source.entry(index);
const std::uint64_t hash=item.key.table_hash();
DictionaryValue value(std::move(item.value));
// Script code run by clear() or by an earlier release may have stored this key again.
const std::uint32_t position=find_position(item.key,hash);
if(position!=no_entry){
entry(index_[position].entry-1).value.swap(value);
}else{
reserve(1);
insert_new(std::move(item.key),hash,value);
}
}
}

bool Dictionary::assign(const Dictionary& other)
{
if(this==&other)return true;
// A snapshot of other without running script code; script objects stored by value are held by reference and copied
// afterwards, when script code changing other can no longer affect the copy.
Dictionary copy(*cache_);
copy.reserve(other.live_);
for(std::uint32_t index=other.first_;index!=no_entry;index=other.entry(index).next){
const Entry& item=other.entry(index);
DictionaryValue value;
if(!value.snapshot(item.value,*cache_))return false;
copy.insert_new(String(item.key),item.key.table_hash(),value);
}
for(std::uint32_t index=copy.first_;index!=no_entry;index=copy.entry(index).next){
DictionaryValue& value=copy.entry(index).value;
if(!value.counted_object())continue;
DictionaryValue fresh;
if(!fresh.make_object_copy(value.payload_.object,value.type_,value.type_id_,*cache_))return false;
value.swap(fresh);
}
commit(copy);
return true;
}

int Dictionary::ref_count() const noexcept
{
return ref_count_;
}

void Dictionary::set_gc_flag() noexcept
{
gc_flag_=true;
}

bool Dictionary::gc_flag() const noexcept
{
return gc_flag_;
}

void Dictionary::enum_references(asIScriptEngine* engine) noexcept
{
// Every entry, free ones included: a value written through a stale reference is still held.
for(int segment=0;segment<segment_count&&segments_[segment];++segment){
Entry* block=segments_[segment];
const std::uint32_t size=first_segment_size<<segment;
for(std::uint32_t at=0;at<size;++at)block[at].value.enum_references(engine);
}
}

void Dictionary::release_all_references(asIScriptEngine*) noexcept
{
clear();
}

// ---- Text format ----

namespace {
void append_integer(std::string& out,std::int64_t value)
{
char digits[24];
out.append(digits,std::to_chars(digits,digits+sizeof digits,value).ptr);
}
void append_unsigned(std::string& out,std::uint64_t value)
{
char digits[24];
out.append(digits,std::to_chars(digits,digits+sizeof digits,value).ptr);
}
template<class T>
void append_floating(std::string& out,T value)
{
if(std::isnan(value)){
out.append("nan");
return;
}
if(std::isinf(value)){
out.append(value<0?"-inf":"inf");
return;
}
char digits[64];
out.append(digits,std::to_chars(digits,digits+sizeof digits,value).ptr);
}
void append_quoted(std::string& out,std::string_view text)
{
constexpr char hex[]="0123456789abcdef";
out.push_back('"');
std::size_t run=0;
for(std::size_t at=0;at<text.size();++at){
const auto byte=static_cast<unsigned char>(text[at]);
if(byte>=0x20&&byte!=0x7F&&byte!='"'&&byte!='\\')continue;
out.append(text.substr(run,at-run));
run=at+1;
switch(byte){
case '"':out.append("\\\"");break;
case '\\':out.append("\\\\");break;
case '\n':out.append("\\n");break;
case '\r':out.append("\\r");break;
case '\t':out.append("\\t");break;
default:
out.append("\\x");
out.push_back(hex[byte>>4]);
out.push_back(hex[byte&0xF]);
break;
}
}
out.append(text.substr(run));
out.push_back('"');
}
int hex_digit(char c) noexcept
{
if(c>='0'&&c<='9')return c-'0';
if(c>='a'&&c<='f')return c-'a'+10;
if(c>='A'&&c<='F')return c-'A'+10;
return -1;
}
// Reads "<text>" from the front of rest into out and drops it from rest.
bool read_quoted(std::string_view& rest,String& out)
{
if(rest.empty()||rest[0]!='"')return false;
std::string bytes;
std::size_t at=1;
for(;;){
if(at>=rest.size())return false;
const auto byte=static_cast<unsigned char>(rest[at]);
if(byte=='"')break;
if(byte<0x20||byte==0x7F)return false;
if(byte!='\\'){
const std::size_t start=at;
while(at<rest.size()){
const auto next=static_cast<unsigned char>(rest[at]);
if(next=='"'||next=='\\'||next<0x20||next==0x7F)break;
++at;
}
bytes.append(rest.substr(start,at-start));
continue;
}
if(at+1>=rest.size())return false;
switch(rest[at+1]){
case '"':bytes.push_back('"');break;
case '\\':bytes.push_back('\\');break;
case 'n':bytes.push_back('\n');break;
case 'r':bytes.push_back('\r');break;
case 't':bytes.push_back('\t');break;
case 'x':{
if(at+3>=rest.size())return false;
const int high=hex_digit(rest[at+2]);
const int low=hex_digit(rest[at+3]);
if(high<0||low<0)return false;
bytes.push_back(static_cast<char>(high*16+low));
at+=2;
break;
}
default:return false;
}
at+=2;
}
out=std::string_view(bytes);
rest.remove_prefix(at+1);
return true;
}
template<class T>
bool read_whole_integer(std::string_view text,T& value) noexcept
{
const char* end=text.data()+text.size();
const std::from_chars_result result=std::from_chars(text.data(),end,value);
return result.ec==std::errc{}&&result.ptr==end&&!text.empty();
}
bool read_double(std::string_view text,double& value)
{
if(text=="inf"){
value=std::numeric_limits<double>::infinity();
return true;
}
if(text=="-inf"){
value=-std::numeric_limits<double>::infinity();
return true;
}
if(text=="nan"){
value=std::numeric_limits<double>::quiet_NaN();
return true;
}
// Only the writer's own forms: no sign but '-', digits first.
if(text.empty()||!(text[0]=='-'||(text[0]>='0'&&text[0]<='9')))return false;
const std::expected<double,StringError> number=String(text).to_number();
if(!number)return false;
value=*number;
return true;
}
bool read_bool(std::string_view text,bool& value) noexcept
{
if(text=="true"){
value=true;
return true;
}
if(text=="false"){
value=false;
return true;
}
return false;
}
bool is_array_type(const asITypeInfo* type) noexcept
{
if(!(type->GetFlags()&asOBJ_TEMPLATE)||std::strcmp(type->GetName(),"array")!=0)return false;
const char* space=type->GetNamespace();
return !space||!*space;
}
}

asITypeInfo* DictionaryCache::array_type(ArrayTag tag) noexcept
{
std::atomic<asITypeInfo*>& slot=arrays[static_cast<int>(tag)];
if(asITypeInfo* type=slot.load(std::memory_order_acquire))return type;
// Threads racing here find the same type.
asITypeInfo* type=engine->GetTypeInfoByDecl(array_tag_names[static_cast<int>(tag)]);
if(type)slot.store(type,std::memory_order_release);
return type;
}

// Writes the text format; throws std::runtime_error for values it cannot write.
class DictionaryWriter {
public:
explicit DictionaryWriter(const DictionaryCache& cache) noexcept:cache_(cache) {}
String write(const Dictionary& dictionary)
{
out_.append(header).push_back('\n');
open_.push_back(&dictionary);
entries(dictionary,0);
return String(out_);
}
private:
[[noreturn]] void fail(const String& key,std::string_view reason)
{
throw std::runtime_error("cannot serialize "+shown_key(key)+": "+std::string(reason));
}
[[noreturn]] void unsupported(const String& key,int type_id)
{
fail(key,"unsupported type "+declaration_of(cache_.engine,type_id));
}
void entries(const Dictionary& dictionary,int depth)
{
for(std::uint32_t index=dictionary.first_;index!=no_entry;index=dictionary.entry(index).next){
const Dictionary::Entry& item=dictionary.entry(index);
value(item.key,item.value,depth);
}
}
// The count line and entries of a nested dictionary (or "null"), at depth.
void nested(const String& key,const Dictionary* dictionary,int depth)
{
if(!dictionary){
out_.append("null\n");
return;
}
if(depth>Dictionary::max_depth)fail(key,"nested too deeply");
for(const Dictionary* open:open_){
if(open==dictionary)fail(key,"the dictionary contains itself");
}
append_integer(out_,dictionary->live_);
out_.push_back('\n');
open_.push_back(dictionary);
entries(*dictionary,depth);
open_.pop_back();
}
void value(const String& key,const DictionaryValue& item,int depth)
{
const int type_id=item.type_id();
switch(type_id){
case 0:
out_.append("none ");
append_quoted(out_,key.bytes());
out_.push_back('\n');
return;
case asTYPEID_INT64:
line_start("int64",key);
append_integer(out_,item.integer());
out_.push_back('\n');
return;
case asTYPEID_DOUBLE:
line_start("double",key);
append_floating(out_,item.number());
out_.push_back('\n');
return;
case asTYPEID_BOOL:
line_start("bool",key);
out_.append(item.boolean()?"true\n":"false\n");
return;
default:
break;
}
asITypeInfo* type=item.object_type();
if(type==cache_.string_type){
line_start("string",key);
append_quoted(out_,static_cast<const String*>(item.object())->bytes());
out_.push_back('\n');
return;
}
if(type==cache_.dictionary_type){
line_start("dictionary",key);
nested(key,static_cast<const Dictionary*>(item.object()),depth+1);
return;
}
if(type&&is_array_type(type)){
array(key,type_id,static_cast<const ScriptArray*>(item.object()),depth+1);
return;
}
unsupported(key,type_id);
}
void line_start(std::string_view tag,const String& key)
{
out_.append(tag).push_back(' ');
append_quoted(out_,key.bytes());
out_.push_back(' ');
}
bool tag_of(int element_type_id,ArrayTag& tag) const noexcept
{
switch(element_type_id){
case asTYPEID_BOOL:tag=ArrayTag::boolean;return true;
case asTYPEID_INT8:tag=ArrayTag::int8;return true;
case asTYPEID_INT16:tag=ArrayTag::int16;return true;
case asTYPEID_INT32:tag=ArrayTag::int32;return true;
case asTYPEID_INT64:tag=ArrayTag::int64;return true;
case asTYPEID_UINT8:tag=ArrayTag::uint8;return true;
case asTYPEID_UINT16:tag=ArrayTag::uint16;return true;
case asTYPEID_UINT32:tag=ArrayTag::uint32;return true;
case asTYPEID_UINT64:tag=ArrayTag::uint64;return true;
case asTYPEID_FLOAT:tag=ArrayTag::float32;return true;
case asTYPEID_DOUBLE:tag=ArrayTag::float64;return true;
default:break;
}
if(element_type_id==cache_.string_type_id)tag=ArrayTag::string;
else if(element_type_id==(cache_.dictionary_type_id|asTYPEID_OBJHANDLE))tag=ArrayTag::dictionary_handle;
else if(element_type_id==cache_.dictionary_type_id)tag=ArrayTag::dictionary_object;
else return false;
return true;
}
void array(const String& key,int type_id,const ScriptArray* items,int depth)
{
asITypeInfo* type=cache_.engine->GetTypeInfoById(type_id);
const int element_type_id=type->GetSubTypeId();
ArrayTag tag;
if(!tag_of(element_type_id,tag))unsupported(key,type_id);
if(depth>Dictionary::max_depth)fail(key,"nested too deeply");
line_start(array_tag_names[static_cast<int>(tag)],key);
if(!items){
out_.append("null\n");
return;
}
const int count=items->length();
append_integer(out_,count);
out_.push_back('\n');
for(int at=0;at<count;++at){
const void* element=items->at(at);
switch(tag){
case ArrayTag::boolean:out_.append(*static_cast<const bool*>(element)?"true":"false");break;
case ArrayTag::int8:append_integer(out_,*static_cast<const std::int8_t*>(element));break;
case ArrayTag::int16:append_integer(out_,*static_cast<const std::int16_t*>(element));break;
case ArrayTag::int32:append_integer(out_,*static_cast<const std::int32_t*>(element));break;
case ArrayTag::int64:append_integer(out_,*static_cast<const std::int64_t*>(element));break;
case ArrayTag::uint8:append_unsigned(out_,*static_cast<const std::uint8_t*>(element));break;
case ArrayTag::uint16:append_unsigned(out_,*static_cast<const std::uint16_t*>(element));break;
case ArrayTag::uint32:append_unsigned(out_,*static_cast<const std::uint32_t*>(element));break;
case ArrayTag::uint64:append_unsigned(out_,*static_cast<const std::uint64_t*>(element));break;
case ArrayTag::float32:append_floating(out_,*static_cast<const float*>(element));break;
case ArrayTag::float64:append_floating(out_,*static_cast<const double*>(element));break;
case ArrayTag::string:append_quoted(out_,static_cast<const String*>(element)->bytes());break;
case ArrayTag::dictionary_handle:nested(key,static_cast<const Dictionary*>(load_pointer(element)),depth+1);continue;
case ArrayTag::dictionary_object:nested(key,static_cast<const Dictionary*>(element),depth+1);continue;
case ArrayTag::count:break;
}
out_.push_back('\n');
}
}

const DictionaryCache& cache_;
std::string out_;
// The dictionaries being written, outermost first.
std::vector<const Dictionary*> open_;
};

// Reads the text format into a private dictionary; false at the first malformed line.
class DictionaryReader {
public:
DictionaryReader(std::string_view text,DictionaryCache& cache) noexcept:text_(text),cache_(cache) {}
bool read(Dictionary& target)
{
std::string_view line;
if(!next_line(line)||line!=header)return false;
while(next_line(line)){
if(!entry(line,target,0))return false;
}
return true;
}
private:
bool next_line(std::string_view& line) noexcept
{
if(position_>=text_.size())return false;
const std::size_t end=text_.find('\n',position_);
const std::size_t stop=end==std::string_view::npos?text_.size():end;
line=text_.substr(position_,stop-position_);
if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
position_=end==std::string_view::npos?text_.size():end+1;
return true;
}
// A count no larger than the lines the rest of the text can hold (each at least one byte and a line end).
bool read_count(std::string_view text,std::uint32_t& count) const noexcept
{
if(!read_whole_integer(text,count)||count>static_cast<std::uint32_t>(INT_MAX))return false;
return count<=(text_.size()-position_+1)/2;
}
bool entry(std::string_view line,Dictionary& target,int depth)
{
const std::size_t space=line.find(' ');
if(space==std::string_view::npos)return false;
const std::string_view tag=line.substr(0,space);
std::string_view rest=line.substr(space+1);
String key;
if(!read_quoted(rest,key))return false;
const std::uint64_t hash=key.table_hash();
if(target.find_position(key,hash)!=no_entry)return false;
DictionaryValue item;
if(tag=="none"){
if(!rest.empty())return false;
}else{
if(rest.size()<2||rest[0]!=' ')return false;
if(!payload(tag,rest.substr(1),item,depth))return false;
}
target.reserve(1);
target.insert_new(std::move(key),hash,item);
return true;
}
bool payload(std::string_view tag,std::string_view text,DictionaryValue& item,int depth)
{
if(tag=="int64"){
std::int64_t value;
if(!read_whole_integer(text,value))return false;
item.set_integer(value);
return true;
}
if(tag=="double"){
double value;
if(!read_double(text,value))return false;
item.set_number(value);
return true;
}
if(tag=="bool"){
bool value;
if(!read_bool(text,value))return false;
item.set_boolean(value);
return true;
}
if(tag=="string"){
String value;
if(!read_quoted(text,value)||!text.empty())return false;
item.adopt(new_string(std::move(value)),cache_.string_type,cache_.string_type_id);
return true;
}
if(tag=="dictionary"){
if(text=="null"){
item.adopt(nullptr,cache_.dictionary_type,cache_.dictionary_type_id|asTYPEID_OBJHANDLE);
return true;
}
Dictionary* nested=nullptr;
if(!nested_dictionary(text,nested,depth+1))return false;
item.adopt(nested,cache_.dictionary_type,cache_.dictionary_type_id|asTYPEID_OBJHANDLE);
return fill(*nested,depth+1);
}
for(int at=0;at<static_cast<int>(ArrayTag::count);++at){
if(tag==array_tag_names[at])return array(static_cast<ArrayTag>(at),text,item,depth+1);
}
return false;
}
// Reads a nested dictionary's count and creates it; fill() then reads its entries. The caller owns the reference.
bool nested_dictionary(std::string_view text,Dictionary*& nested,int depth)
{
if(depth>Dictionary::max_depth||!read_count(text,pending_))return false;
nested=Dictionary::create(cache_);
return true;
}
bool fill(Dictionary& nested,int depth)
{
const std::uint32_t count=pending_;
for(std::uint32_t at=0;at<count;++at){
std::string_view line;
if(!next_line(line)||!entry(line,nested,depth))return false;
}
return true;
}
bool array(ArrayTag tag,std::string_view text,DictionaryValue& item,int depth)
{
if(depth>Dictionary::max_depth)return false;
asITypeInfo* type=cache_.array_type(tag);
if(!type)return false;
const int handle_id=type->GetTypeId()|asTYPEID_OBJHANDLE;
if(text=="null"){
item.adopt(nullptr,type,handle_id);
return true;
}
std::uint32_t count;
if(!read_count(text,count))return false;
ScriptArray* items=ScriptArray::create(type,static_cast<int>(count));
if(!items)return false;
item.adopt(items,type,handle_id);
for(std::uint32_t at=0;at<count;++at){
std::string_view line;
if(!next_line(line)||!element(tag,line,*items,static_cast<int>(at),depth))return false;
}
return true;
}
template<class T>
static bool read_ranged(std::string_view text,void* slot) noexcept
{
std::int64_t value;
if(!read_whole_integer(text,value)||value<std::numeric_limits<T>::min()||value>static_cast<std::int64_t>(std::numeric_limits<T>::max()))return false;
const T narrow=static_cast<T>(value);
std::memcpy(slot,&narrow,sizeof narrow);
return true;
}
bool element(ArrayTag tag,std::string_view text,ScriptArray& items,int at,int depth)
{
void* slot=items.at(at);
switch(tag){
case ArrayTag::boolean:{
bool value;
if(!read_bool(text,value))return false;
*static_cast<bool*>(slot)=value;
return true;
}
case ArrayTag::int8:return read_ranged<std::int8_t>(text,slot);
case ArrayTag::int16:return read_ranged<std::int16_t>(text,slot);
case ArrayTag::int32:return read_ranged<std::int32_t>(text,slot);
case ArrayTag::int64:return read_whole_integer(text,*static_cast<std::int64_t*>(slot));
case ArrayTag::uint8:return read_ranged<std::uint8_t>(text,slot);
case ArrayTag::uint16:return read_ranged<std::uint16_t>(text,slot);
case ArrayTag::uint32:return read_ranged<std::uint32_t>(text,slot);
case ArrayTag::uint64:return read_whole_integer(text,*static_cast<std::uint64_t*>(slot));
case ArrayTag::float32:{
double value;
// Refuse only what would round to infinity (the shortest text of FLT_MAX reads as a double just above it); the cast of
// anything below is defined and rounds to the nearest float.
if(!read_double(text,value)||(std::isfinite(value)&&std::fabs(value)>=0x1.ffffffp127))return false;
*static_cast<float*>(slot)=static_cast<float>(value);
return true;
}
case ArrayTag::float64:return read_double(text,*static_cast<double*>(slot));
case ArrayTag::string:{
String value;
if(!read_quoted(text,value)||!text.empty())return false;
*static_cast<String*>(slot)=std::move(value);
return true;
}
case ArrayTag::dictionary_handle:{
if(text=="null")return true;
Dictionary* nested=nullptr;
if(!nested_dictionary(text,nested,depth+1))return false;
// The array's handle slot takes over the reference.
store_pointer(slot,nested);
return fill(*nested,depth+1);
}
case ArrayTag::dictionary_object:{
std::uint32_t count;
if(depth+1>Dictionary::max_depth||!read_count(text,count))return false;
pending_=count;
return fill(*static_cast<Dictionary*>(slot),depth+1);
}
case ArrayTag::count:break;
}
return false;
}

std::string_view text_;
std::size_t position_=0;
DictionaryCache& cache_;
// The entry count nested_dictionary() read, for the following fill().
std::uint32_t pending_=0;
};

String Dictionary::serialize() const
{
return DictionaryWriter(*cache_).write(*this);
}

bool Dictionary::deserialize(std::string_view text)
{
Dictionary parsed(*cache_);
if(!DictionaryReader(text,*cache_).read(parsed))return false;
commit(parsed);
return true;
}

// ---- Script bindings ----

Dictionary* DictionaryCache::make_dictionary() noexcept
{
try{
return Dictionary::create(*this);
}
catch(...){
raise_current_exception();
}
return nullptr;
}

Dictionary* DictionaryCache::make_from_list(void* list) noexcept
{
Dictionary* dictionary=nullptr;
try{
dictionary=Dictionary::create(*this);
// The add-on's layout: a count, then per pair (at a 4-byte boundary) the key, the value's type id and the value, inline
// for primitives and value types, a pointer for reference types and handles.
auto* buffer=static_cast<std::byte*>(list);
asUINT count;
std::memcpy(&count,buffer,sizeof count);
buffer+=sizeof count;
while(count--){
if(const auto misaligned=reinterpret_cast<std::uintptr_t>(buffer)&3)buffer+=4-misaligned;
// The key String lies at the 4-byte boundary the engine's list buffer layout gives it, as the add-on also relies on.
const String& key=*reinterpret_cast<const String*>(buffer);
buffer+=sizeof(String);
int type_id;
std::memcpy(&type_id,buffer,sizeof type_id);
buffer+=sizeof type_id;
const void* ref=buffer;
if(type_id&asTYPEID_MASK_OBJECT){
asITypeInfo* type=engine->GetTypeInfoById(type_id);
const bool by_value=type->GetFlags()&asOBJ_VALUE;
if(!by_value&&!(type_id&asTYPEID_OBJHANDLE))ref=load_pointer(buffer);
buffer+=by_value?type->GetSize():sizeof(void*);
}else if(type_id==asTYPEID_VOID){
buffer+=sizeof(void*);
}else{
buffer+=engine->GetSizeOfPrimitiveType(type_id);
}
if(!dictionary->set(key,ref,type_id)){
dictionary->release();
return nullptr;
}
}
return dictionary;
}
catch(...){
raise_current_exception();
}
if(dictionary)dictionary->release();
return nullptr;
}

DictionaryValue& DictionaryCache::value_assign(const void* ref,int type_id,DictionaryValue* self) noexcept
{
try{
self->assign(ref,type_id,*this);
}
catch(...){
raise_current_exception();
}
return *self;
}

DictionaryValue& DictionaryCache::value_assign_value(const DictionaryValue& other,DictionaryValue* self) noexcept
{
try{
self->assign(other,*this);
}
catch(...){
raise_current_exception();
}
return *self;
}

DictionaryValue& DictionaryCache::value_assign_integer(std::int64_t value,DictionaryValue* self) noexcept
{
self->set_integer(value);
return *self;
}

DictionaryValue& DictionaryCache::value_assign_number(double value,DictionaryValue* self) noexcept
{
self->set_number(value);
return *self;
}

void DictionaryCache::value_read(void* out,int type_id,const DictionaryValue* self) noexcept
{
try{
self->get(out,type_id,*this);
}
catch(...){
raise_current_exception();
}
}

std::int64_t DictionaryCache::value_integer(const DictionaryValue* self) noexcept
{
std::int64_t value=0;
try{
self->get(&value,asTYPEID_INT64,*this);
}
catch(...){
raise_current_exception();
}
return value;
}

double DictionaryCache::value_number(const DictionaryValue* self) noexcept
{
double value=0;
try{
self->get(&value,asTYPEID_DOUBLE,*this);
}
catch(...){
raise_current_exception();
}
return value;
}

namespace {
void construct_value(DictionaryValue* self) noexcept
{
new(self) DictionaryValue();
}
void destruct_value(DictionaryValue* self) noexcept
{
self->~DictionaryValue();
}
// Method-style entry points (asCALL_CDECL_OBJFIRST), wrapped by guarded<> so no C++ exception reaches the engine.
void set_any(Dictionary& self,const String& key,const void* ref,int type_id)
{
self.set(key,ref,type_id);
}
void set_integer(Dictionary& self,const String& key,const std::int64_t& value)
{
self.set(key,value);
}
void set_number(Dictionary& self,const String& key,const double& value)
{
self.set(key,value);
}
bool get_any(const Dictionary& self,const String& key,void* out,int type_id)
{
return self.get(key,out,type_id);
}
bool get_integer(const Dictionary& self,const String& key,std::int64_t& out)
{
return self.get(key,&out,asTYPEID_INT64);
}
bool get_number(const Dictionary& self,const String& key,double& out)
{
return self.get(key,&out,asTYPEID_DOUBLE);
}
DictionaryValue* value_at(Dictionary& self,const String& key)
{
return &self.at(key);
}
const DictionaryValue* value_at_const(const Dictionary& self,const String& key)
{
if(const DictionaryValue* value=self.find(key))return value;
throw std::runtime_error("key "+shown_key(key)+" not found");
}
ScriptArray* keys(const Dictionary& self)
{
return self.keys();
}
String key_at(const Dictionary& self,int index)
{
const String* key=self.key_at(index);
if(!key)throw std::out_of_range("index out of range");
return *key;
}
String serialize(const Dictionary& self)
{
return self.serialize();
}
bool deserialize(Dictionary& self,const String& text)
{
return self.deserialize(text.bytes());
}
Dictionary& assign(Dictionary& self,const Dictionary& other)
{
self.assign(other);
return self;
}
DictionaryCache& cache_of(asIScriptGeneric* generic) noexcept
{
return *static_cast<DictionaryCache*>(generic->GetEngine()->GetUserData(dictionary_cache_slot));
}
// Not declared noexcept (they cannot throw): asFUNCTION recognizes the generic convention by the exact asGENFUNC_t type.
void factory(asIScriptGeneric* generic)
{
Dictionary* dictionary=cache_of(generic).make_dictionary();
store_pointer(generic->GetAddressOfReturnLocation(),dictionary);
}
void list_factory(asIScriptGeneric* generic)
{
Dictionary* dictionary=cache_of(generic).make_from_list(generic->GetArgAddress(0));
store_pointer(generic->GetAddressOfReturnLocation(),dictionary);
}
void cleanup_cache(asIScriptEngine* engine)
{
delete static_cast<DictionaryCache*>(engine->GetUserData(dictionary_cache_slot));
}
void check(int result,const char* declaration)
{
if(result>=0)return;
throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
}

void register_dictionary(asIScriptEngine* engine)
{
check(engine->RegisterObjectType("dictionary_value",sizeof(DictionaryValue),asOBJ_VALUE|asOBJ_ASHANDLE|asOBJ_GC|asGetTypeTraits<DictionaryValue>()),"dictionary_value");
check(engine->RegisterObjectType("dictionary",0,asOBJ_REF|asOBJ_GC),"dictionary");
auto* cache=new DictionaryCache();
cache->engine=engine;
engine->SetUserData(cache,dictionary_cache_slot);
engine->SetEngineUserDataCleanupCallback(cleanup_cache,dictionary_cache_slot);
auto value_behaviour=[engine](asEBehaviours kind,const char* declaration,const asSFuncPtr& function,asDWORD convention){
check(engine->RegisterObjectBehaviour("dictionary_value",kind,declaration,function,convention),declaration);
};
auto value_method=[engine,cache](const char* declaration,const asSFuncPtr& function){
check(engine->RegisterObjectMethod("dictionary_value",declaration,function,asCALL_THISCALL_OBJLAST,cache),declaration);
};
value_behaviour(asBEHAVE_CONSTRUCT,"void f()",asFUNCTION(construct_value),asCALL_CDECL_OBJLAST);
value_behaviour(asBEHAVE_DESTRUCT,"void f()",asFUNCTION(destruct_value),asCALL_CDECL_OBJLAST);
value_behaviour(asBEHAVE_ENUMREFS,"void f(int&in)",asMETHOD(DictionaryValue,enum_references),asCALL_THISCALL);
value_behaviour(asBEHAVE_RELEASEREFS,"void f(int&in)",asMETHOD(DictionaryValue,release_references),asCALL_THISCALL);
value_method("dictionary_value &opAssign(const dictionary_value &in)",asMETHOD(DictionaryCache,value_assign_value));
value_method("dictionary_value &opHndlAssign(const ?&in)",asMETHOD(DictionaryCache,value_assign));
value_method("dictionary_value &opHndlAssign(const dictionary_value &in)",asMETHOD(DictionaryCache,value_assign_value));
value_method("dictionary_value &opAssign(const ?&in)",asMETHOD(DictionaryCache,value_assign));
value_method("dictionary_value &opAssign(double)",asMETHOD(DictionaryCache,value_assign_number));
value_method("dictionary_value &opAssign(int64)",asMETHOD(DictionaryCache,value_assign_integer));
value_method("void opCast(?&out) const",asMETHOD(DictionaryCache,value_read));
value_method("void opConv(?&out) const",asMETHOD(DictionaryCache,value_read));
value_method("int64 opConv() const",asMETHOD(DictionaryCache,value_integer));
value_method("double opConv() const",asMETHOD(DictionaryCache,value_number));
auto behaviour=[engine](asEBehaviours kind,const char* declaration,const asSFuncPtr& function,asDWORD convention){
check(engine->RegisterObjectBehaviour("dictionary",kind,declaration,function,convention),declaration);
};
auto method=[engine](const char* declaration,const asSFuncPtr& function,asDWORD convention){
check(engine->RegisterObjectMethod("dictionary",declaration,function,convention),declaration);
};
// The factories use the generic convention: the engine also calls them itself (CreateScriptObject), where it supports
// neither auxiliary objects nor finding the engine otherwise.
behaviour(asBEHAVE_FACTORY,"dictionary@ f()",asFUNCTION(factory),asCALL_GENERIC);
behaviour(asBEHAVE_LIST_FACTORY,"dictionary@ f(int&in) {repeat {string, ?}}",asFUNCTION(list_factory),asCALL_GENERIC);
behaviour(asBEHAVE_ADDREF,"void f()",asMETHOD(Dictionary,add_ref),asCALL_THISCALL);
behaviour(asBEHAVE_RELEASE,"void f()",asMETHOD(Dictionary,release),asCALL_THISCALL);
behaviour(asBEHAVE_GETREFCOUNT,"int f()",asMETHOD(Dictionary,ref_count),asCALL_THISCALL);
behaviour(asBEHAVE_SETGCFLAG,"void f()",asMETHOD(Dictionary,set_gc_flag),asCALL_THISCALL);
behaviour(asBEHAVE_GETGCFLAG,"bool f()",asMETHOD(Dictionary,gc_flag),asCALL_THISCALL);
behaviour(asBEHAVE_ENUMREFS,"void f(int&in)",asMETHOD(Dictionary,enum_references),asCALL_THISCALL);
behaviour(asBEHAVE_RELEASEREFS,"void f(int&in)",asMETHOD(Dictionary,release_all_references),asCALL_THISCALL);
method("dictionary &opAssign(const dictionary &in other)",asFUNCTION(guarded<&assign>::call),asCALL_CDECL_OBJFIRST);
method("void set(const string &in key, const ?&in value)",asFUNCTION(guarded<&set_any>::call),asCALL_CDECL_OBJFIRST);
method("void set(const string &in key, const int64 &in value)",asFUNCTION(guarded<&set_integer>::call),asCALL_CDECL_OBJFIRST);
method("void set(const string &in key, const double &in value)",asFUNCTION(guarded<&set_number>::call),asCALL_CDECL_OBJFIRST);
method("bool get(const string &in key, ?&out value) const",asFUNCTION(guarded<&get_any>::call),asCALL_CDECL_OBJFIRST);
method("bool get(const string &in key, int64 &out value) const",asFUNCTION(guarded<&get_integer>::call),asCALL_CDECL_OBJFIRST);
method("bool get(const string &in key, double &out value) const",asFUNCTION(guarded<&get_number>::call),asCALL_CDECL_OBJFIRST);
method("dictionary_value &opIndex(const string &in key)",asFUNCTION(guarded<&value_at>::call),asCALL_CDECL_OBJFIRST);
method("const dictionary_value &opIndex(const string &in key) const",asFUNCTION(guarded<&value_at_const>::call),asCALL_CDECL_OBJFIRST);
method("bool exists(const string &in key) const",asMETHOD(Dictionary,exists),asCALL_THISCALL);
method("bool remove(const string &in key)",asMETHOD(Dictionary,remove),asCALL_THISCALL);
method("void clear()",asMETHOD(Dictionary,clear),asCALL_THISCALL);
method("int length() const",asMETHOD(Dictionary,length),asCALL_THISCALL);
method("bool empty() const",asMETHOD(Dictionary,empty),asCALL_THISCALL);
method("array<string>@ keys() const",asFUNCTION(guarded<&keys>::call),asCALL_CDECL_OBJFIRST);
method("bool move_by(const string &in key, int delta)",asMETHOD(Dictionary,move_by),asCALL_THISCALL);
method("bool move(const string &in key, int index)",asMETHOD(Dictionary,move),asCALL_THISCALL);
method("int index_of(const string &in key) const",asMETHOD(Dictionary,index_of),asCALL_THISCALL);
method("string key_at(int index) const",asFUNCTION(guarded<&key_at>::call),asCALL_CDECL_OBJFIRST);
method("string serialize() const",asFUNCTION(guarded<&serialize>::call),asCALL_CDECL_OBJFIRST);
method("bool deserialize(const string &in text)",asFUNCTION(guarded<&deserialize>::call),asCALL_CDECL_OBJFIRST);
cache->dictionary_type=engine->GetTypeInfoByName("dictionary");
cache->dictionary_type_id=cache->dictionary_type->GetTypeId();
cache->value_type_id=engine->GetTypeIdByDecl("dictionary_value");
cache->string_type=engine->GetTypeInfoByDecl("string");
cache->string_array_type=engine->GetTypeInfoByDecl("array<string>");
if(!cache->string_type||!cache->string_array_type)throw std::runtime_error("register_dictionary needs Zodiac's string and array registered first");
cache->string_type_id=cache->string_type->GetTypeId();
}
}
