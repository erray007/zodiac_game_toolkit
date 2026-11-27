#include "core/host.hpp"
#include "core/types/array.hpp"
#include "core/types/script_guard.hpp"
#include "core/types/string.hpp"
#include "core/types/string_binding.hpp"
#include "core/vfs/vfs.hpp"
#include <angelscript.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <type_traits>

namespace zodiac {
namespace {
constexpr std::string_view reason_invalid_argument="invalid argument";
constexpr std::string_view reason_declaration="invalid declaration";
constexpr std::string_view reason_not_primitive="only primitive types are allowed on the native fast path";
constexpr std::string_view reason_already_registered="already registered";
bool is_space(char c) noexcept
{
return c==' '||c=='\t'||c=='\n'||c=='\r';
}
bool is_word(char c) noexcept
{
return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}
bool is_digit(char c) noexcept
{
return c>='0'&&c<='9';
}
bool is_primitive_name(std::string_view word) noexcept
{
static constexpr std::string_view names[]={"void","bool","int8","int16","int","int32","int64","uint8","uint16","uint","uint32","uint64","float","double"};
for(std::string_view name:names)if(word==name)return true;
return false;
}
// Minimal tokenizer for the native fast path; AngelScript still parses the declaration fully afterwards.
struct Cursor{
std::string_view text;
std::size_t at=0;
void skip() noexcept
{
while(at<text.size()&&is_space(text[at]))++at;
}
char peek() noexcept
{
skip();
return at<text.size()?text[at]:'\0';
}
bool eat(char c) noexcept
{
if(peek()!=c)return false;
++at;
return true;
}
std::string_view word() noexcept
{
skip();
const std::size_t start=at;
while(at<text.size()&&is_word(text[at]))++at;
return text.substr(start,at-start);
}
// A default argument constant: number (with optional exponent sign), true or false.
std::string_view literal() noexcept
{
skip();
const std::size_t start=at;
while(at<text.size()){
const char c=text[at];
const bool exponent_sign=(c=='+'||c=='-')&&at>start&&(text[at-1]=='e'||text[at-1]=='E')&&is_digit(text[start])&&!(at-start>=2&&(text[start+1]=='x'||text[start+1]=='X'));
if(!is_word(c)&&c!='.'&&!exponent_sign)break;
++at;
}
return text.substr(start,at-start);
}
bool done() noexcept
{
skip();
return at==text.size();
}
};
// Returns ZODIAC_OK, ZODIAC_E_NOT_PRIMITIVE or ZODIAC_E_DECLARATION.
int check_type(Cursor& cursor,bool is_return) noexcept
{
std::string_view word=cursor.word();
if(word=="const")word=cursor.word();
if(word.empty())return ZODIAC_E_DECLARATION;
if(!is_primitive_name(word))return ZODIAC_E_NOT_PRIMITIVE;
const char next=cursor.peek();
if(next=='&'||next=='@'||next=='<'||next=='[')return ZODIAC_E_NOT_PRIMITIVE;
if(word=="void"&&!is_return)return ZODIAC_E_DECLARATION;
return ZODIAC_OK;
}
int check_native_declaration(std::string_view declaration) noexcept
{
Cursor cursor{declaration};
if(const int result=check_type(cursor,true);result!=ZODIAC_OK)return result;
const std::string_view name=cursor.word();
if(name.empty()||is_digit(name.front())||!cursor.eat('('))return ZODIAC_E_DECLARATION;
if(!cursor.eat(')')){
const std::size_t parameters=cursor.at;
if(!(cursor.word()=="void"&&cursor.eat(')'))){
cursor.at=parameters;
for(;;){
if(const int result=check_type(cursor,false);result!=ZODIAC_OK)return result;
if(is_word(cursor.peek()))cursor.word();
if(cursor.eat('=')){
if(!cursor.eat('-'))cursor.eat('+');
if(cursor.literal().empty())return ZODIAC_E_DECLARATION;
}
if(cursor.eat(','))continue;
if(cursor.eat(')'))break;
return ZODIAC_E_DECLARATION;
}
}
}
if(!cursor.done()&&(cursor.word()!="property"||!cursor.done()))return ZODIAC_E_DECLARATION;
return ZODIAC_OK;
}
bool is_named(const asITypeInfo* type,const char* name) noexcept
{
const char* name_space=type->GetNamespace();
return std::strcmp(type->GetName(),name)==0&&(!name_space||!*name_space);
}
// The script string registered by register_string (zodiac::String), recognised by the engine's string factory.
bool is_script_string(const asITypeInfo* type) noexcept
{
return is_script_string_type(type);
}
ScriptArray* to_array(zodiac_array* array) noexcept
{
return reinterpret_cast<ScriptArray*>(array);
}
// Appends one default element (ScriptArray grows geometrically, so repeated pushes stay amortized O(1)) and returns its
// index, or -1 when the array could not grow (the array has raised the script exception).
int push_default(ScriptArray* array) noexcept
{
const int size=array->length();
if(size==std::numeric_limits<int>::max()){
raise_script_exception("out of memory");
return -1;
}
array->resize(size+1);
return array->length()==size+1?size:-1;
}
// Only accepts noexcept functions, so every table entry is checked at compile time to never throw into plugin code.
template<class R,class... A> constexpr auto nothrow_entry(R (*fn)(A...) noexcept) noexcept
{
return fn;
}
template<class T> T load(const void* address) noexcept
{
T value;
std::memcpy(&value,address,sizeof(T));
return value;
}
template<class T> void store(void* address,T value) noexcept
{
std::memcpy(address,&value,sizeof(T));
}
// The plugin code of a FileError (plugin.h, VFS rules).
int vfs_code(vfs::FileError error) noexcept
{
switch(error){
case vfs::FileError::not_found:return ZODIAC_E_NOT_FOUND;
case vfs::FileError::damaged:return ZODIAC_E_DAMAGED;
case vfs::FileError::bad_path:
case vfs::FileError::ambiguous:
case vfs::FileError::is_directory:
return ZODIAC_E_BAD_PATH;
default:return ZODIAC_E_IO;
}
}
}

// The plugins' open VFS files. A handle is (generation << 32) | index: index names a slot, generation is the slot's current one
// (1 to 2^30 - 1, so no handle is null). Slots live in segments of 64, 128, 256, ... slots that are made once and never move or
// go before the table, so a call finds its slot with two loads and no lock. Each slot's state word holds its generation, a locked
// bit and a waiting bit: a call compare-exchanges the locked bit on only while the generation is the handle's, which refuses a
// stale or forged handle and serializes the calls on one handle (a waiter sleeps on the word; an unlock wakes it only when the
// waiting bit says someone sleeps, so the usual call costs one compare-exchange and one exchange). close moves the generation on
// under the lock, then returns the index to the free list, whose room was reserved when the slot was made, so it never allocates.
class PluginFiles{
public:
// One slot per cache line, so threads reading different handles never share the line their state words are on.
struct alignas(64) Slot{
std::atomic<std::uint32_t> state{first_generation<<generation_shift};
std::shared_ptr<vfs::Reader> reader;
std::uint64_t position=0;
// The reader's size, at most INT64_MAX, so every position fits the int64_t results.
std::uint64_t size=0;
};
// The slot a handle names, locked until this is destroyed; empty for a handle that names no open file.
class Locked{
public:
Locked() noexcept=default;
Locked(Slot* slot,std::uint32_t generation) noexcept:slot_(slot),generation_(generation){}
Locked(const Locked&)=delete;
Locked& operator=(const Locked&)=delete;
~Locked()
{
if(slot_)unlock(*slot_,generation_);
}
explicit operator bool() const noexcept{return slot_!=nullptr;}
Slot* operator->() const noexcept{return slot_;}
// Hands the lock over to the caller, who must unlock or retire the slot.
Slot* release() noexcept{return std::exchange(slot_,nullptr);}
std::uint32_t generation() const noexcept{return generation_;}
private:
Slot* slot_=nullptr;
std::uint32_t generation_=0;
};
PluginFiles()=default;
PluginFiles(const PluginFiles&)=delete;
PluginFiles& operator=(const PluginFiles&)=delete;
~PluginFiles()
{
for(std::atomic<Slot*>& segment:segments_)delete[] segment.load(std::memory_order_relaxed);
}
Locked lock(const zodiac_vfs_file* handle) noexcept
{
const auto value=reinterpret_cast<std::uintptr_t>(handle);
const auto generation=static_cast<std::uint32_t>(value>>32);
if(generation==0||generation>max_generation)return {};
Slot* slot=slot_at(static_cast<std::uint32_t>(value));
if(!slot||!acquire(*slot,generation))return {};
// A slot never opened, or one a forged handle names with its next generation, holds no reader.
if(!slot->reader){
unlock(*slot,generation);
return {};
}
return Locked(slot,generation);
}
// A handle for reader, or null when no slot can be had (out of memory, or every index in use).
zodiac_vfs_file* open(std::shared_ptr<vfs::Reader> reader,std::uint64_t size) noexcept
{
std::uint32_t index=0;
{
const std::lock_guard guard(mutex_);
if(!free_.empty()){
index=free_.back();
free_.pop_back();
}
else{
if(made_==max_slots)return nullptr;
if(free_.capacity()<=made_){
try{
free_.reserve(std::max<std::size_t>(std::size_t{made_}*2,first_segment_size));
}
catch(...){
return nullptr;
}
}
const unsigned segment=segment_of(made_);
if(!segments_[segment].load(std::memory_order_relaxed)){
Slot* fresh=new(std::nothrow) Slot[std::size_t{first_segment_size}<<segment];
if(!fresh)return nullptr;
segments_[segment].store(fresh,std::memory_order_release);
}
index=made_++;
}
}
// The slot is free and only close changes a generation, so this one stays the slot's; a forged handle may hold its lock
// for a moment, which acquire waits out.
Slot& slot=*slot_at(index);
const std::uint32_t generation=slot.state.load(std::memory_order_relaxed)>>generation_shift;
static_cast<void>(acquire(slot,generation));
slot.reader=std::move(reader);
slot.position=0;
slot.size=std::min<std::uint64_t>(size,static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
unlock(slot,generation);
return reinterpret_cast<zodiac_vfs_file*>((static_cast<std::uintptr_t>(generation)<<32)|index);
}
void close(const zodiac_vfs_file* handle) noexcept
{
Locked locked=lock(handle);
if(!locked)return;
// Dropped after every lock is let go: the last reference may close a file or free a pack's storage.
const std::shared_ptr<vfs::Reader> reader=std::move(locked->reader);
const std::uint32_t generation=locked.generation();
Slot& slot=*locked.release();
const std::uint32_t next=generation==max_generation?first_generation:generation+1;
if(slot.state.exchange(next<<generation_shift,std::memory_order_release)&waiting)slot.state.notify_all();
const std::lock_guard guard(mutex_);
free_.push_back(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle)));
}
private:
static_assert(sizeof(std::uintptr_t)==8,"a plugin VFS handle packs a 32-bit generation and a 32-bit index");
static constexpr std::uint32_t locked_bit=1;
static constexpr std::uint32_t waiting=2;
static constexpr unsigned generation_shift=2;
static constexpr std::uint32_t first_generation=1;
static constexpr std::uint32_t max_generation=(std::uint32_t{1}<<(32-generation_shift))-1;
static constexpr unsigned first_segment_log2=6;
static constexpr std::uint32_t first_segment_size=std::uint32_t{1}<<first_segment_log2;
// 25 segments hold 64 * (2^25 - 1) slots, fewer than 2^32, so every index fits its 32 bits.
static constexpr unsigned segment_count=25;
static constexpr std::uint32_t max_slots=static_cast<std::uint32_t>((std::uint64_t{first_segment_size}<<segment_count)-first_segment_size);
// Segment s holds the indexes from 64 * (2^s - 1) on: index + 64 has its highest bit at position s + 6.
static unsigned segment_of(std::uint32_t index) noexcept
{
return static_cast<unsigned>(std::bit_width(std::uint64_t{index}+first_segment_size))-(first_segment_log2+1);
}
Slot* slot_at(std::uint32_t index) const noexcept
{
if(index>=max_slots)return nullptr;
const unsigned segment=segment_of(index);
Slot* slots=segments_[segment].load(std::memory_order_acquire);
return slots?slots+(std::uint64_t{index}+first_segment_size-(std::uint64_t{first_segment_size}<<segment)):nullptr;
}
// Locks slot while its generation is generation; false (nothing held) once it is another.
static bool acquire(Slot& slot,std::uint32_t generation) noexcept
{
const std::uint32_t open=generation<<generation_shift;
std::uint32_t state=open;
if(slot.state.compare_exchange_strong(state,open|locked_bit,std::memory_order_acquire,std::memory_order_relaxed))return true;
for(;;){
if((state>>generation_shift)!=generation)return false;
if(!(state&locked_bit)){
if(slot.state.compare_exchange_weak(state,state|locked_bit,std::memory_order_acquire,std::memory_order_relaxed))return true;
continue;
}
if(!(state&waiting)&&!slot.state.compare_exchange_weak(state,state|waiting,std::memory_order_relaxed,std::memory_order_relaxed))continue;
slot.state.wait(state|waiting,std::memory_order_relaxed);
state=slot.state.load(std::memory_order_relaxed);
}
}
static void unlock(Slot& slot,std::uint32_t generation) noexcept
{
if(slot.state.exchange(generation<<generation_shift,std::memory_order_release)&waiting)slot.state.notify_all();
}
std::array<std::atomic<Slot*>,segment_count> segments_{};
// Guards made_ and free_; open and close only, never a read.
std::mutex mutex_;
std::uint32_t made_=0;
std::vector<std::uint32_t> free_;
};

// The C entry points of the ABI table. A zodiac_call* is the asIScriptGeneric* of the current call, and the
// generic function's auxiliary pointer is its Binding, so every accessor is a cast plus a virtual call or two.
struct HostAbi{
using Kind=Host::Kind;
using Value=Host::Value;
using Binding=Host::Binding;
static_assert(std::is_standard_layout_v<Host::Table>);
static_assert(offsetof(Host::Table,abi)==0);
static Host* owner(zodiac_host* host) noexcept
{
return host?reinterpret_cast<Host::Table*>(host)->owner:nullptr;
}
static asIScriptGeneric* generic(zodiac_call* call) noexcept
{
return reinterpret_cast<asIScriptGeneric*>(call);
}
static const Binding& binding(asIScriptGeneric* gen) noexcept
{
return *static_cast<const Binding*>(gen->GetAuxiliary());
}
static void thunk(asIScriptGeneric* gen)
{
const Binding& bound=binding(gen);
if(bound.string_result)::new(gen->GetAddressOfReturnLocation()) String();
bound.fn(reinterpret_cast<zodiac_call*>(gen));
}
// Raises a script exception for an allocation failure inside a call-time accessor; never throws itself.
static void out_of_memory() noexcept
{
try{
if(asIScriptContext* context=asGetActiveContext())context->SetException("out of memory");
}
catch(...){
}
}
// Error path only. A failed message allocation or a throwing log handler drops the message.
static void misuse(asIScriptGeneric* gen,const Binding& bound,std::string_view accessor) noexcept
{
try{
std::string message(accessor);
message.append(" does not match \"").append(gen->GetFunction()->GetDeclaration(true,true,false)).append("\"");
bound.host->log(ZODIAC_LOG_ERROR,message);
}
catch(...){
}
}
// Registration entry points return an error code instead of letting an exception unwind into register_module.
template<class F> static int guarded(Host* self,const char* declaration,F&& registration) noexcept
{
if(!self)return ZODIAC_E_INVALID_ARGUMENT;
try{
return registration();
}
catch(...){
return self->fail(ZODIAC_E_INVALID_ARGUMENT,declaration?declaration:"","out of memory");
}
}
template<class T> static T read_number(zodiac_call* call,uint32_t index) noexcept
{
asIScriptGeneric* gen=generic(call);
const Binding& bound=binding(gen);
if(index>=bound.params.size())return T();
const Value& param=bound.params[index];
const void* address=gen->GetAddressOfArg(index);
if(param.by_reference)address=*static_cast<void* const*>(address);
switch(param.kind){
case Kind::boolean:return static_cast<T>(load<bool>(address));
case Kind::int8:return static_cast<T>(load<int8_t>(address));
case Kind::int16:return static_cast<T>(load<int16_t>(address));
case Kind::int32:return static_cast<T>(load<int32_t>(address));
case Kind::int64:return static_cast<T>(load<int64_t>(address));
case Kind::uint8:return static_cast<T>(load<uint8_t>(address));
case Kind::uint16:return static_cast<T>(load<uint16_t>(address));
case Kind::uint32:return static_cast<T>(load<uint32_t>(address));
case Kind::uint64:return static_cast<T>(load<uint64_t>(address));
case Kind::float32:return static_cast<T>(load<float>(address));
case Kind::float64:return static_cast<T>(load<double>(address));
default:return T();
}
}
template<class T> static void write_number(zodiac_call* call,T value,std::string_view accessor) noexcept
{
asIScriptGeneric* gen=generic(call);
const Binding& bound=binding(gen);
const Kind kind=bound.result.by_reference?Kind::other:bound.result.kind;
void* address=gen->GetAddressOfReturnLocation();
switch(kind){
case Kind::boolean:store(address,static_cast<bool>(value));return;
case Kind::int8:store(address,static_cast<int8_t>(value));return;
case Kind::int16:store(address,static_cast<int16_t>(value));return;
case Kind::int32:store(address,static_cast<int32_t>(value));return;
case Kind::int64:store(address,static_cast<int64_t>(value));return;
case Kind::uint8:store(address,static_cast<uint8_t>(value));return;
case Kind::uint16:store(address,static_cast<uint16_t>(value));return;
case Kind::uint32:store(address,static_cast<uint32_t>(value));return;
case Kind::uint64:store(address,static_cast<uint64_t>(value));return;
case Kind::float32:store(address,static_cast<float>(value));return;
case Kind::float64:store(address,static_cast<double>(value));return;
default:misuse(gen,bound,accessor);
}
}
static const Value* param(asIScriptGeneric* gen,uint32_t index) noexcept
{
const Binding& bound=binding(gen);
return index<bound.params.size()?&bound.params[index]:nullptr;
}
// Replaces the returned handle; the host owns exactly one reference to whatever is returned.
static void return_handle(asIScriptGeneric* gen,const Value& result,void* object) noexcept
{
void** slot=static_cast<void**>(gen->GetAddressOfReturnLocation());
if(*slot)gen->GetEngine()->ReleaseScriptObject(*slot,result.type);
*slot=object;
}
static int register_function(zodiac_host* host,const char* declaration,zodiac_generic_fn fn,void* user_data) noexcept
{
Host* self=owner(host);
return guarded(self,declaration,[&]{return self->register_generic(Host::Target::function,nullptr,declaration,fn,user_data);});
}
static int register_native_function(zodiac_host* host,const char* declaration,void (*fn)(void)) noexcept
{
Host* self=owner(host);
return guarded(self,declaration,[&]{return self->register_native(declaration,fn);});
}
static int register_ref_type(zodiac_host* host,const char* name,void (*add_ref)(void*),void (*release)(void*)) noexcept
{
Host* self=owner(host);
return guarded(self,name,[&]{return self->register_ref_type(name,add_ref,release);});
}
static int register_method(zodiac_host* host,const char* type_name,const char* declaration,zodiac_generic_fn fn,void* user_data) noexcept
{
Host* self=owner(host);
return guarded(self,declaration,[&]{return self->register_generic(Host::Target::method,type_name,declaration,fn,user_data);});
}
static int register_factory(zodiac_host* host,const char* type_name,const char* declaration,zodiac_generic_fn fn,void* user_data) noexcept
{
Host* self=owner(host);
return guarded(self,declaration,[&]{return self->register_generic(Host::Target::factory,type_name,declaration,fn,user_data);});
}
static void* call_user_data(zodiac_call* call) noexcept
{
return binding(generic(call)).user_data;
}
static void* call_object(zodiac_call* call) noexcept
{
return generic(call)->GetObject();
}
static int32_t arg_int32(zodiac_call* call,uint32_t index) noexcept
{
return read_number<int32_t>(call,index);
}
static int64_t arg_int64(zodiac_call* call,uint32_t index) noexcept
{
return read_number<int64_t>(call,index);
}
static float arg_float(zodiac_call* call,uint32_t index) noexcept
{
return read_number<float>(call,index);
}
static double arg_double(zodiac_call* call,uint32_t index) noexcept
{
return read_number<double>(call,index);
}
static int arg_bool(zodiac_call* call,uint32_t index) noexcept
{
return read_number<bool>(call,index)?1:0;
}
static const char* arg_string(zodiac_call* call,uint32_t index,size_t* length) noexcept
{
asIScriptGeneric* gen=generic(call);
const Value* value=param(gen,index);
const String* text=value&&value->kind==Kind::string?static_cast<const String*>(gen->GetArgObject(index)):nullptr;
if(length)*length=text?static_cast<size_t>(text->byte_length()):0;
return text?text->data():nullptr;
}
static void* arg_object(zodiac_call* call,uint32_t index) noexcept
{
asIScriptGeneric* gen=generic(call);
const Value* value=param(gen,index);
return value&&(value->kind==Kind::object||value->kind==Kind::array)?gen->GetArgObject(index):nullptr;
}
static zodiac_array* arg_array(zodiac_call* call,uint32_t index) noexcept
{
asIScriptGeneric* gen=generic(call);
const Value* value=param(gen,index);
return value&&value->kind==Kind::array?reinterpret_cast<zodiac_array*>(gen->GetArgObject(index)):nullptr;
}
static void ret_int32(zodiac_call* call,int32_t value) noexcept
{
write_number(call,value,"ret_int32");
}
static void ret_int64(zodiac_call* call,int64_t value) noexcept
{
write_number(call,value,"ret_int64");
}
static void ret_float(zodiac_call* call,float value) noexcept
{
write_number(call,value,"ret_float");
}
static void ret_double(zodiac_call* call,double value) noexcept
{
write_number(call,value,"ret_double");
}
static void ret_bool(zodiac_call* call,int value) noexcept
{
write_number(call,value!=0,"ret_bool");
}
static void ret_string(zodiac_call* call,const char* data,size_t length) noexcept
{
asIScriptGeneric* gen=generic(call);
const Binding& bound=binding(gen);
if(!bound.string_result){
misuse(gen,bound,"ret_string");
return;
}
String& result=*static_cast<String*>(gen->GetAddressOfReturnLocation());
try{
if(data)result=std::string_view(data,length);
else result.clear();
}
catch(...){
out_of_memory();
}
}
static void ret_object(zodiac_call* call,void* object) noexcept
{
asIScriptGeneric* gen=generic(call);
const Binding& bound=binding(gen);
if(!bound.result.handle){
misuse(gen,bound,"ret_object");
if(object&&bound.result.type&&!(bound.result.type->GetFlags()&asOBJ_VALUE))gen->GetEngine()->ReleaseScriptObject(object,bound.result.type);
return;
}
return_handle(gen,bound.result,object);
}
static zodiac_array* ret_array(zodiac_call* call,const char* element_type,uint32_t reserve) noexcept
{
asIScriptGeneric* gen=generic(call);
const Binding& bound=binding(gen);
const std::string_view element=element_type?element_type:"";
const Kind kind=element=="string"?Kind::string:element=="int"?Kind::int32:element=="double"?Kind::float64:Kind::other;
if(kind==Kind::other){
try{
std::string message="ret_array: unknown element type \"";
message.append(element).append("\" (expected \"string\", \"int\" or \"double\")");
bound.host->log(ZODIAC_LOG_ERROR,message);
}
catch(...){
}
return nullptr;
}
const Value& result=bound.result;
if(result.kind!=Kind::array||!result.handle||result.element!=kind){
misuse(gen,bound,"ret_array");
return nullptr;
}
ScriptArray* array=ScriptArray::create(result.type);
if(!array)return nullptr;
// The hint is only a capacity: when it cannot be had it is ignored, and pushes grow geometrically as needed.
if(reserve)static_cast<void>(array->try_reserve(static_cast<int>(std::min<uint32_t>(reserve,static_cast<uint32_t>(std::numeric_limits<int>::max())))));
return_handle(gen,result,array);
return reinterpret_cast<zodiac_array*>(array);
}
static uint32_t array_length(zodiac_array* array) noexcept
{
return array?static_cast<uint32_t>(to_array(array)->length()):0;
}
static const char* array_get_string(zodiac_array* array,uint32_t index,size_t* length) noexcept
{
const ScriptArray* items=to_array(array);
const String* text=items&&index<static_cast<uint32_t>(items->length())&&items->holds_strings()?static_cast<const String*>(items->at(static_cast<int>(index))):nullptr;
if(length)*length=text?static_cast<size_t>(text->byte_length()):0;
return text?text->data():nullptr;
}
static int32_t array_get_int32(zodiac_array* array,uint32_t index) noexcept
{
const ScriptArray* items=to_array(array);
return items&&index<static_cast<uint32_t>(items->length())&&items->element_type_id()==asTYPEID_INT32?load<int32_t>(items->at(static_cast<int>(index))):0;
}
static double array_get_double(zodiac_array* array,uint32_t index) noexcept
{
const ScriptArray* items=to_array(array);
return items&&index<static_cast<uint32_t>(items->length())&&items->element_type_id()==asTYPEID_DOUBLE?load<double>(items->at(static_cast<int>(index))):0.0;
}
static void array_push_string(zodiac_array* array,const char* data,size_t length) noexcept
{
ScriptArray* items=to_array(array);
if(!items||!items->holds_strings())return;
const int index=push_default(items);
if(index<0||!data)return;
try{
*static_cast<String*>(items->at(index))=std::string_view(data,length);
}
catch(...){
out_of_memory();
}
}
static void array_push_int32(zodiac_array* array,int32_t value) noexcept
{
ScriptArray* items=to_array(array);
if(items&&items->element_type_id()==asTYPEID_INT32)items->insert_last(&value);
}
static void array_push_double(zodiac_array* array,double value) noexcept
{
ScriptArray* items=to_array(array);
if(items&&items->element_type_id()==asTYPEID_DOUBLE)items->insert_last(&value);
}
static void set_exception(zodiac_call*,const char* message) noexcept
{
try{
if(asIScriptContext* context=asGetActiveContext())context->SetException(message?message:"");
}
catch(...){
}
}
static void log(zodiac_host* host,zodiac_log_level level,const char* message) noexcept
{
if(const Host* self=owner(host))self->log(level,message?message:"");
}
// The VFS entries (plugin.h, VFS rules). The Vfs never throws, but a reader may run out of memory; nothing reaches the plugin.
static int vfs_open(zodiac_host* host,const char* path,zodiac_vfs_file** handle) noexcept
{
if(handle)*handle=nullptr;
Host* self=owner(host);
if(!self||!path||!handle)return ZODIAC_E_INVALID_ARGUMENT;
vfs::Vfs* files=self->vfs_;
if(!files)return ZODIAC_E_UNAVAILABLE;
try{
auto reader=files->open_read(path);
if(!reader)return vfs_code(reader.error().code);
if(!*reader)return ZODIAC_E_NOT_FOUND;
const std::uint64_t size=(*reader)->size();
zodiac_vfs_file* opened=self->files_->open(std::move(*reader),size);
if(!opened)return ZODIAC_E_IO;
*handle=opened;
return ZODIAC_OK;
}
catch(...){
return ZODIAC_E_IO;
}
}
static int64_t vfs_read(zodiac_host* host,zodiac_vfs_file* file,void* buffer,size_t size) noexcept
{
Host* self=owner(host);
if(!self||(!buffer&&size))return ZODIAC_E_INVALID_ARGUMENT;
const PluginFiles::Locked slot=self->files_->lock(file);
if(!slot)return ZODIAC_E_INVALID_ARGUMENT;
const std::uint64_t at=slot->position;
if(at>=slot->size||!size)return 0;
// Never more than the file has left, so a large buffer costs the reader nothing; the reader fills buffer itself.
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(size,slot->size-at));
std::int64_t got=0;
try{
got=slot->reader->read_at(at,{static_cast<std::byte*>(buffer),wanted});
}
catch(...){
return ZODIAC_E_IO;
}
if(got<0)return vfs_code(vfs::read_error(got));
slot->position=at+static_cast<std::uint64_t>(got);
return got;
}
static int64_t vfs_seek(zodiac_host* host,zodiac_vfs_file* file,int64_t offset,int origin) noexcept
{
Host* self=owner(host);
if(!self)return ZODIAC_E_INVALID_ARGUMENT;
const PluginFiles::Locked slot=self->files_->lock(file);
if(!slot)return ZODIAC_E_INVALID_ARGUMENT;
std::int64_t base=0;
switch(origin){
case ZODIAC_SEEK_SET:break;
case ZODIAC_SEEK_CUR:base=static_cast<std::int64_t>(slot->position);break;
case ZODIAC_SEEK_END:base=static_cast<std::int64_t>(slot->size);break;
default:return ZODIAC_E_INVALID_ARGUMENT;
}
std::int64_t target=0;
if(__builtin_add_overflow(base,offset,&target)||target<0||static_cast<std::uint64_t>(target)>slot->size)return ZODIAC_E_INVALID_ARGUMENT;
slot->position=static_cast<std::uint64_t>(target);
return target;
}
static int64_t vfs_size(zodiac_host* host,zodiac_vfs_file* file) noexcept
{
Host* self=owner(host);
if(!self)return ZODIAC_E_INVALID_ARGUMENT;
const PluginFiles::Locked slot=self->files_->lock(file);
return slot?static_cast<std::int64_t>(slot->size):ZODIAC_E_INVALID_ARGUMENT;
}
static void vfs_close(zodiac_host* host,zodiac_vfs_file* file) noexcept
{
if(Host* self=owner(host))self->files_->close(file);
}
static int vfs_exists(zodiac_host* host,const char* path) noexcept
{
Host* self=owner(host);
if(!self||!path)return ZODIAC_E_INVALID_ARGUMENT;
vfs::Vfs* files=self->vfs_;
if(!files)return ZODIAC_E_UNAVAILABLE;
try{
const auto info=files->stat(path);
if(info)return info->kind==vfs::EntryKind::file?1:0;
// Nothing, a folder, or a name below a file: no file there.
switch(const vfs::FileError error=info.error().code){
case vfs::FileError::not_found:
case vfs::FileError::is_directory:
case vfs::FileError::not_a_directory:
return 0;
default:return vfs_code(error);
}
}
catch(...){
return ZODIAC_E_IO;
}
}
};

Host::Host(asIScriptEngine* engine):table_{},engine_(engine),files_(std::make_unique<PluginFiles>())
{
zodiac_host& abi=table_.abi;
abi.abi_version=ZODIAC_ABI_VERSION;
abi.struct_size=sizeof(zodiac_host);
abi.register_function=nothrow_entry(&HostAbi::register_function);
abi.register_native_function=nothrow_entry(&HostAbi::register_native_function);
abi.register_ref_type=nothrow_entry(&HostAbi::register_ref_type);
abi.register_method=nothrow_entry(&HostAbi::register_method);
abi.register_factory=nothrow_entry(&HostAbi::register_factory);
abi.call_user_data=nothrow_entry(&HostAbi::call_user_data);
abi.call_object=nothrow_entry(&HostAbi::call_object);
abi.arg_int32=nothrow_entry(&HostAbi::arg_int32);
abi.arg_int64=nothrow_entry(&HostAbi::arg_int64);
abi.arg_float=nothrow_entry(&HostAbi::arg_float);
abi.arg_double=nothrow_entry(&HostAbi::arg_double);
abi.arg_bool=nothrow_entry(&HostAbi::arg_bool);
abi.arg_string=nothrow_entry(&HostAbi::arg_string);
abi.arg_object=nothrow_entry(&HostAbi::arg_object);
abi.arg_array=nothrow_entry(&HostAbi::arg_array);
abi.ret_int32=nothrow_entry(&HostAbi::ret_int32);
abi.ret_int64=nothrow_entry(&HostAbi::ret_int64);
abi.ret_float=nothrow_entry(&HostAbi::ret_float);
abi.ret_double=nothrow_entry(&HostAbi::ret_double);
abi.ret_bool=nothrow_entry(&HostAbi::ret_bool);
abi.ret_string=nothrow_entry(&HostAbi::ret_string);
abi.ret_object=nothrow_entry(&HostAbi::ret_object);
abi.ret_array=nothrow_entry(&HostAbi::ret_array);
abi.array_length=nothrow_entry(&HostAbi::array_length);
abi.array_get_string=nothrow_entry(&HostAbi::array_get_string);
abi.array_get_int32=nothrow_entry(&HostAbi::array_get_int32);
abi.array_get_double=nothrow_entry(&HostAbi::array_get_double);
abi.array_push_string=nothrow_entry(&HostAbi::array_push_string);
abi.array_push_int32=nothrow_entry(&HostAbi::array_push_int32);
abi.array_push_double=nothrow_entry(&HostAbi::array_push_double);
abi.set_exception=nothrow_entry(&HostAbi::set_exception);
abi.log=nothrow_entry(&HostAbi::log);
abi.vfs_open=nothrow_entry(&HostAbi::vfs_open);
abi.vfs_read=nothrow_entry(&HostAbi::vfs_read);
abi.vfs_seek=nothrow_entry(&HostAbi::vfs_seek);
abi.vfs_size=nothrow_entry(&HostAbi::vfs_size);
abi.vfs_close=nothrow_entry(&HostAbi::vfs_close);
abi.vfs_exists=nothrow_entry(&HostAbi::vfs_exists);
table_.owner=this;
}

Host::~Host()=default;

// A throwing handler must never unwind into plugin code, so its exceptions are swallowed and the message is dropped.
void Host::log(zodiac_log_level level,std::string_view message) const noexcept
{
if(!log_handler_)return;
try{
log_handler_(level,message);
}
catch(...){
}
}

int Host::fail(int code,std::string_view declaration,std::string_view reason) const noexcept
{
++registration_failures_;
if(!log_handler_)return code;
try{
std::string message="module ";
message.append(current_module_).append(": cannot register \"").append(declaration).append("\": ").append(reason);
log_handler_(ZODIAC_LOG_ERROR,message);
}
catch(...){
}
return code;
}

int Host::fail_engine(int engine_code,std::string_view declaration) const noexcept
{
switch(engine_code){
case asALREADY_REGISTERED:
case asNAME_TAKEN:
return fail(ZODIAC_E_ALREADY_REGISTERED,declaration,reason_already_registered);
case asINVALID_DECLARATION:
case asINVALID_NAME:
return fail(ZODIAC_E_DECLARATION,declaration,reason_declaration);
case asILLEGAL_BEHAVIOUR_FOR_TYPE:
return fail(ZODIAC_E_DECLARATION,declaration,"declaration not allowed for this type");
case asINVALID_TYPE:
case asINVALID_OBJECT:
return fail(ZODIAC_E_UNKNOWN_TYPE,declaration,"unknown type");
case asNOT_SUPPORTED:
case asWRONG_CALLING_CONV:
return fail(ZODIAC_E_INVALID_ARGUMENT,declaration,"calling convention not supported on this platform");
default:{
char reason[40]="angelscript error ";
const std::size_t prefix=std::strlen(reason);
const std::to_chars_result written=std::to_chars(reason+prefix,reason+sizeof(reason),engine_code);
return fail(ZODIAC_E_INVALID_ARGUMENT,declaration,std::string_view(reason,written.ptr-reason));
}
}
}

Host::Value Host::classify(int type_id,bool by_reference) const
{
Value value;
value.by_reference=by_reference;
value.handle=(type_id&asTYPEID_OBJHANDLE)!=0;
if(!(type_id&asTYPEID_MASK_OBJECT)){
switch(type_id){
case asTYPEID_BOOL:value.kind=Kind::boolean;break;
case asTYPEID_INT8:value.kind=Kind::int8;break;
case asTYPEID_INT16:value.kind=Kind::int16;break;
case asTYPEID_INT32:value.kind=Kind::int32;break;
case asTYPEID_INT64:value.kind=Kind::int64;break;
case asTYPEID_UINT8:value.kind=Kind::uint8;break;
case asTYPEID_UINT16:value.kind=Kind::uint16;break;
case asTYPEID_UINT32:value.kind=Kind::uint32;break;
case asTYPEID_UINT64:value.kind=Kind::uint64;break;
case asTYPEID_FLOAT:value.kind=Kind::float32;break;
case asTYPEID_DOUBLE:value.kind=Kind::float64;break;
default:
// Enums are signed integers of their underlying size.
if(type_id>asTYPEID_DOUBLE){
switch(engine_->GetSizeOfPrimitiveType(type_id)){
case 1:value.kind=Kind::int8;break;
case 2:value.kind=Kind::int16;break;
case 4:value.kind=Kind::int32;break;
case 8:value.kind=Kind::int64;break;
default:break;
}
}
}
return value;
}
asITypeInfo* type=engine_->GetTypeInfoById(type_id);
value.type=type;
if(!type||(type->GetFlags()&asOBJ_FUNCDEF))return value;
if(is_script_string(type)){
if(!value.handle)value.kind=Kind::string;
return value;
}
if((type->GetFlags()&asOBJ_TEMPLATE)&&is_named(type,"array")&&type->GetSubTypeCount()==1){
value.kind=Kind::array;
const int element_id=type->GetSubTypeId(0);
if(element_id==asTYPEID_INT32)value.element=Kind::int32;
else if(element_id==asTYPEID_DOUBLE)value.element=Kind::float64;
else if(is_script_string(type->GetSubType(0)))value.element=Kind::string;
return value;
}
value.kind=Kind::object;
return value;
}

void Host::describe(Binding& binding,const asIScriptFunction* function) const
{
asDWORD flags=0;
const int return_id=function->GetReturnTypeId(&flags);
binding.result=classify(return_id,(flags&asTM_INOUTREF)!=0);
binding.string_result=binding.result.kind==Kind::string&&!binding.result.by_reference;
const asUINT count=function->GetParamCount();
binding.params.reserve(count);
for(asUINT index=0;index<count;++index){
int type_id=0;
flags=0;
function->GetParam(index,&type_id,&flags);
binding.params.push_back(classify(type_id,(flags&asTM_INOUTREF)!=0));
}
}

int Host::register_generic(Target target,const char* type_name,const char* declaration,zodiac_generic_fn fn,void* user_data)
{
const std::string_view shown=declaration?declaration:"";
if(!declaration||!fn||(target!=Target::function&&!type_name))return fail(ZODIAC_E_INVALID_ARGUMENT,shown,reason_invalid_argument);
asITypeInfo* type=nullptr;
if(target!=Target::function){
type=engine_->GetTypeInfoByName(type_name);
if(!type)return fail(ZODIAC_E_UNKNOWN_TYPE,shown,"unknown type \""+std::string(type_name)+"\"");
}
// Look duplicates up first: a failed AngelScript registration marks the whole engine configuration as invalid.
const asIScriptFunction* existing=target==Target::function?engine_->GetGlobalFunctionByDecl(declaration):target==Target::method?type->GetMethodByDecl(declaration,false):type->GetFactoryByDecl(declaration);
if(existing)return fail(ZODIAC_E_ALREADY_REGISTERED,shown,reason_already_registered);
Binding& binding=bindings_.emplace_back(Binding{fn,user_data,this,false,{},{}});
int result=0;
if(target==Target::function)result=engine_->RegisterGlobalFunction(declaration,asFUNCTION(HostAbi::thunk),asCALL_GENERIC,&binding);
else if(target==Target::method)result=engine_->RegisterObjectMethod(type_name,declaration,asFUNCTION(HostAbi::thunk),asCALL_GENERIC,&binding);
else result=engine_->RegisterObjectBehaviour(type_name,asBEHAVE_FACTORY,declaration,asFUNCTION(HostAbi::thunk),asCALL_GENERIC,&binding);
const asIScriptFunction* function=result>=0?engine_->GetFunctionById(result):nullptr;
if(!function){
bindings_.pop_back();
return fail_engine(result<0?result:asERROR,shown);
}
describe(binding,function);
return ZODIAC_OK;
}

int Host::register_native(const char* declaration,void (*fn)(void))
{
const std::string_view shown=declaration?declaration:"";
if(!declaration||!fn)return fail(ZODIAC_E_INVALID_ARGUMENT,shown,reason_invalid_argument);
switch(check_native_declaration(shown)){
case ZODIAC_OK:break;
case ZODIAC_E_NOT_PRIMITIVE:return fail(ZODIAC_E_NOT_PRIMITIVE,shown,reason_not_primitive);
default:return fail(ZODIAC_E_DECLARATION,shown,reason_declaration);
}
if(engine_->GetGlobalFunctionByDecl(declaration))return fail(ZODIAC_E_ALREADY_REGISTERED,shown,reason_already_registered);
const int result=engine_->RegisterGlobalFunction(declaration,asFUNCTION(fn),asCALL_CDECL);
return result<0?fail_engine(result,shown):ZODIAC_OK;
}

int Host::register_ref_type(const char* name,void (*add_ref)(void*),void (*release)(void*))
{
const std::string_view shown=name?name:"";
// Either both reference counting callbacks or neither (a type the host never counts).
if(shown.empty()||(add_ref==nullptr)!=(release==nullptr))return fail(ZODIAC_E_INVALID_ARGUMENT,shown,reason_invalid_argument);
if(engine_->GetTypeInfoByName(name))return fail(ZODIAC_E_ALREADY_REGISTERED,shown,reason_already_registered);
int result=engine_->RegisterObjectType(name,0,add_ref?asOBJ_REF:asOBJ_REF|asOBJ_NOCOUNT);
if(result>=0&&add_ref){
result=engine_->RegisterObjectBehaviour(name,asBEHAVE_ADDREF,"void f()",asFUNCTION(add_ref),asCALL_CDECL_OBJLAST);
if(result>=0)result=engine_->RegisterObjectBehaviour(name,asBEHAVE_RELEASE,"void f()",asFUNCTION(release),asCALL_CDECL_OBJLAST);
}
return result<0?fail_engine(result,shown):ZODIAC_OK;
}
}
