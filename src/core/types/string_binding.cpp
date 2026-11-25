// The registration of the script `string` and its wrappers (see string_binding.hpp for the contract): script exceptions for
// zodiac::String's errors, the shared literal constants, format(), and array<string>'s split() and join().
#include <core/types/string_binding.hpp>
#include <core/text.hpp>
#include <core/types/script_guard.hpp>
#include <core/types/string.hpp>
#include <core/types/array.hpp>
#include <angelscript.h>
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zodiac {
namespace {
static_assert(sizeof(String)==32&&alignof(String)==8,"the script string type is registered with zodiac::String's layout");
// Engine user data slot holding array<string>'s type, which split() creates its result with.
constexpr asPWORD string_array_slot=0x5A535452;
// Characters of the offending text an "is not a number" message shows before "...".
constexpr int max_shown=64;

// Literals: one shared, immutable String per distinct constant, counted by the modules holding it. Hashed with the keyed
// table hash (both overloads, so a lookup by bytes finds the String), since script source can be untrusted text too.
struct ConstantHash {
using is_transparent=void;
std::size_t operator()(std::string_view bytes) const noexcept
{
return static_cast<std::size_t>(string_detail::table_hash_bytes(bytes.data(),bytes.size()));
}
std::size_t operator()(const String& text) const noexcept
{
return static_cast<std::size_t>(text.table_hash());
}
};
struct ConstantEqual {
using is_transparent=void;
bool operator()(const String& a,const String& b) const noexcept
{
return a==b;
}
bool operator()(const String& a,std::string_view b) const noexcept
{
return a==b;
}
bool operator()(std::string_view a,const String& b) const noexcept
{
return b==a;
}
};
class StringFactory final:public asIStringFactory {
public:
const void* GetStringConstant(const char* data,asUINT length) override
{
const std::string_view bytes(length?data:"",length);
try{
const std::lock_guard lock(mutex_);
auto found=constants_.find(bytes);
if(found==constants_.end()){
found=constants_.try_emplace(String(bytes),0).first;
// Filled once here, under the lock, so contexts reading the constant at the same time find the caches ready.
static_cast<void>(found->first.is_ascii());
static_cast<void>(found->first.table_hash());
}
++found->second;
return &found->first;
}
catch(...){
return nullptr;
}
}
int ReleaseStringConstant(const void* constant) override
{
if(!constant)return asERROR;
try{
const std::lock_guard lock(mutex_);
const auto found=constants_.find(*static_cast<const String*>(constant));
if(found==constants_.end()||&found->first!=constant)return asERROR;
if(--found->second==0)constants_.erase(found);
return asSUCCESS;
}
catch(...){
return asERROR;
}
}
int GetRawStringData(const void* constant,char* data,asUINT* length) const override
{
if(!constant)return asERROR;
const String& text=*static_cast<const String*>(constant);
if(length)*length=static_cast<asUINT>(text.byte_length());
if(data)std::memcpy(data,text.data(),static_cast<std::size_t>(text.byte_length()));
return asSUCCESS;
}
private:
std::mutex mutex_;
std::unordered_map<String,int,ConstantHash,ConstantEqual> constants_;
};
// Never destroyed: an engine released during static destruction still returns its constants to a live factory.
StringFactory& string_factory()
{
static StringFactory* const factory=new StringFactory();
return *factory;
}

void raise_string_error(StringError error,const String& text) noexcept
{
switch(error){
case StringError::index_out_of_range:
raise_script_exception("index out of range");
return;
case StringError::negative_count:
raise_script_exception("negative count");
return;
case StringError::not_a_number:
break;
}
try{
String shown(one_line(text.bytes()));
const bool cut=shown.length()>max_shown;
if(cut)shown=*shown.left(max_shown);
std::string message;
message.reserve(static_cast<std::size_t>(shown.byte_length())+24);
message.push_back('"');
message.append(shown.bytes());
if(cut)message.append("...");
message.append("\" is not a number");
raise_script_exception(message.c_str());
}
catch(...){
raise_current_exception();
}
}
template<class T>
T value_or_raise(std::expected<T,StringError>&& result,const String& self)
{
if(result)return std::move(*result);
raise_string_error(result.error(),self);
return T();
}
template<class T>
T load(const void* address) noexcept
{
T value;
std::memcpy(&value,address,sizeof(T));
return value;
}
template<class T>
String text_of(T value)
{
if constexpr(std::is_same_v<T,bool>){
return String::from_bool(value);
}else{
return String::from_number(value);
}
}

// Behaviours. A failed construction leaves an empty string, so the engine may destroy the object either way.
void construct(void* memory) noexcept
{
::new(memory) String();
}
// A copy of heap text allocates and may throw. This, copy_construct and destruct start on a 64-byte boundary (see
// String::copy_heap in string.hpp).
[[gnu::noinline,gnu::aligned(64)]] void copy_construct_heap(const String& other,void* memory) noexcept
{
try{
::new(memory) String(other);
}
catch(...){
::new(memory) String();
raise_current_exception();
}
}
// Inline text is copied without allocating, so the common case is a leaf without an exception frame (a tail call otherwise).
[[gnu::aligned(64)]] void copy_construct(const String& other,void* memory) noexcept
{
if(other.is_inline()){
::new(memory) String(other);
return;
}
copy_construct_heap(other,memory);
}
template<class T>
void construct_from(T value,void* memory) noexcept
{
try{
::new(memory) String(text_of(value));
}
catch(...){
::new(memory) String();
raise_current_exception();
}
}
[[gnu::aligned(64)]] void destruct(String* self) noexcept
{
self->~String();
}

// Operators.
String& assign(String& self,const String& other)
{
return self=other;
}
template<class T>
String& assign_value(String& self,T value)
{
self.clear();
return self.append(value);
}
String& add_assign(String& self,const String& other)
{
return self.append(other);
}
template<class T>
String& add_assign_value(String& self,T value)
{
return self.append(value);
}
String add(const String& self,const String& other)
{
return self+other;
}
template<class T>
String add_value(const String& self,T value)
{
return self+text_of(value);
}
template<class T>
String add_value_reversed(const String& self,T value)
{
return text_of(value)+self;
}
bool equals(const String& self,const String& other) noexcept
{
return self==other;
}
int compare(const String& self,const String& other) noexcept
{
const std::strong_ordering order=self<=>other;
return order<0?-1:order>0?1:0;
}
// Methods.
// code_at and byte_at are registered as const uint &code_at(int) and const uint8 &byte_at(int): the engine calls methods of
// the form `type &f(int)` through asBC_Thiscall1, which costs about half of the general asBC_CALLSYS path (the add-on's
// uint8 &opIndex(uint) is called that way). Scripts copy the value out at once, so the reference only has to stay valid and
// unchanged while they do: byte_at refers to the byte inside the string (as the add-on's opIndex does), code_at to entry c
// of a table whose entry c holds c. The table's first block (U+0000-U+07FF: every one- and two-byte character) is constant;
// every other block is filled on first use and published once with a compare-and-swap, then never changed or freed.
constexpr std::uint32_t code_block_bits=11;
constexpr std::uint32_t code_block_size=1u<<code_block_bits;
constexpr std::uint32_t code_block_count=0x110000u>>code_block_bits;
struct CodeBlock {
std::uint32_t codes[code_block_size];
};
constexpr CodeBlock make_low_codes() noexcept
{
CodeBlock block{};
for(std::uint32_t code=0;code<code_block_size;++code)block.codes[code]=code;
return block;
}
constexpr CodeBlock low_codes=make_low_codes();
// Blocks 1 to code_block_count-1, null until first used; read and published with atomic builtins.
CodeBlock* high_codes[code_block_count];
constexpr std::uint8_t zero_byte=0;
// Fills and publishes the block holding value (out of line, so the lookups below stay small). Throws std::bad_alloc.
[[gnu::noinline]] CodeBlock* publish_code_block(std::uint32_t value)
{
auto* built=new CodeBlock;
const std::uint32_t first=value&~(code_block_size-1);
for(std::uint32_t offset=0;offset<code_block_size;++offset)built->codes[offset]=first+offset;
CodeBlock* expected=nullptr;
if(__atomic_compare_exchange_n(&high_codes[value>>code_block_bits],&expected,built,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return built;
delete built;
return expected;
}
// Entry code of the table; code is a code point (String never decodes anything above U+10FFFF). Throws std::bad_alloc.
inline const std::uint32_t& code_entry(char32_t code)
{
const auto value=static_cast<std::uint32_t>(code);
if(value<code_block_size)return low_codes.codes[value];
CodeBlock* block=__atomic_load_n(&high_codes[value>>code_block_bits],__ATOMIC_ACQUIRE);
if(!block)block=publish_code_block(value);
return block->codes[value&(code_block_size-1)];
}
[[gnu::noinline]] const std::uint32_t& code_at_any(const String& self,int index) noexcept
{
try{
const std::expected<char32_t,StringError> code=self.code_at(index);
if(code)return code_entry(*code);
raise_string_error(code.error(),self);
}
catch(...){
raise_current_exception();
}
return low_codes.codes[0];
}
// A leaf for ASCII characters of text known to have one byte per character (a tail call otherwise).
const std::uint32_t& code_at(const String& self,int index) noexcept
{
const int code=self.known_ascii_code_at(index);
if(code>=0)return low_codes.codes[code];
return code_at_any(self,index);
}
[[gnu::noinline]] int find_any(const String& self,const String& needle,int start) noexcept
{
try{
return value_or_raise(self.find(needle.bytes(),start),self);
}
catch(...){
raise_current_exception();
}
return -1;
}
}

[[gnu::noinline]] const std::uint8_t& String::script_byte_out_of_range(int) const
{
raise_string_error(StringError::index_out_of_range,*this);
return zero_byte;
}
#if defined(ZODIAC_CORE_PLACEMENT_PAD)&&ZODIAC_CORE_PLACEMENT_PAD>0
// Bench placement studies only (bench-reference.yml sets ZODIAC_CORE_PLACEMENT_PAD through tests/bench's ZODIAC_BENCH_CORE_PAD; no preset
// does): an unused function of that many nop bytes just before the byte_at leaf (StringBindingAccess below), so a no-op relink moves
// code near the leaf against other code, not all of it together.
#define ZODIAC_PAD_TEXT(x) #x
#define ZODIAC_PAD_STRING(x) ZODIAC_PAD_TEXT(x)
[[gnu::used,gnu::noinline]] void zodiac_core_placement_pad()
{
asm volatile(".fill " ZODIAC_PAD_STRING(ZODIAC_CORE_PLACEMENT_PAD) ",1,0x90");
}
#endif

// String's friend for the script find (find_simple's preconditions checked first) and the script byte_at.
struct StringBindingAccess {
// The script byte_at, registered asCALL_CDECL_OBJLAST (the string last), as the add-on registers its opIndex: a frameless leaf in
// range, and a forced tail call to byte_out_of_range otherwise.
// - The convention: asBC_Thiscall1 calls a THISCALL method through a member-function pointer (composite and base offsets, the
//   virtual bit), an OBJLAST function as a plain call. Measured in Task 5c's placement study (8 Windows runners, 6 copies of each
//   build, 2 processes per copy): string_byte_ascii read 1.013-1.047 against the add-on as a THISCALL member, 0.979-0.987 as OBJLAST
//   and 0.982-0.991 as OBJFIRST, on the EPYC 7763, 9V45 and Xeon 8573C alike (length(), on the general call path, did not move).
// - The tail call: without musttail, full LTO sees that the cold path always returns zero_byte, calls it as a plain call and, on
//   Windows, gives the whole function a stack frame (sub/add rsp on the in-range path too, seen in the release disassembly).
//   Neither is noexcept: clang cannot make a musttail call out of a noexcept function (its terminate scope is a cleanup); nothing in
//   them can throw.
// - It starts on a 64-byte boundary, as copy_heap and release do (string.hpp), so its own start does not shift with unrelated code
//   under LTO; CI's probe step prints its release disassembly.
[[gnu::noinline]] static const std::uint8_t& byte_out_of_range(int index,const String& self)
{
return self.script_byte_out_of_range(index);
}
[[gnu::aligned(64)]] static const std::uint8_t& byte_at(int index,const String& self)
{
if(static_cast<std::uint32_t>(index)<self.size_bytes())return *reinterpret_cast<const std::uint8_t*>(self.data()+index);
[[clang::musttail]] return byte_out_of_range(index,self);
}
// The script s[i] (get_opIndex): on text known to be ASCII with i in range, the one-byte string built in place; everything else
// (text not yet scanned, multibyte text, a bad index) takes char_at_any, out of line, which raises the script exception. Before (Task
// 5c), every s[i] went through guarded's exception frame, std::expected<String> and the general char_at (length, the character
// index, the copying constructor); string_index_ascii read 1.04-1.05 against the add-on's substr on the EPYC 7763.
[[gnu::noinline]] static String char_at_any(const String& self,int index) noexcept
{
try{
return value_or_raise(self.char_at(index),self);
}
catch(...){
raise_current_exception();
}
return String();
}
static String char_at(const String& self,int index) noexcept
{
const int code=self.known_ascii_code_at(index);
if(code>=0)return String(String::OneAsciiByte{static_cast<char>(code)});
return char_at_any(self,index);
}
// Text with one byte per character and a valid start goes straight to find_simple, without an exception frame (it
// never allocates); everything else (multibyte text, which may build its index, or a bad start) takes find_any.
static int find(const String& self,const String& needle,int start) noexcept
{
const int length=self.length();
if(length==self.byte_length()&&static_cast<unsigned>(start)<=static_cast<unsigned>(length))return self.find_simple(needle.data(),static_cast<std::size_t>(needle.byte_length()),start);
return find_any(self,needle,start);
}
};

namespace {
int find(const String& self,const String& needle,int start) noexcept
{
return StringBindingAccess::find(self,needle,start);
}
int rfind(const String& self,const String& needle,int start)
{
return value_or_raise(self.rfind(needle.bytes(),start),self);
}
bool contains(const String& self,const String& needle) noexcept
{
return self.contains(needle.bytes());
}
bool starts_with(const String& self,const String& prefix) noexcept
{
return self.starts_with(prefix.bytes());
}
bool ends_with(const String& self,const String& suffix) noexcept
{
return self.ends_with(suffix.bytes());
}
int count(const String& self,const String& needle) noexcept
{
return self.count(needle.bytes());
}
String substr(const String& self,int start,int count)
{
return value_or_raise(self.substr(start,count),self);
}
String left(const String& self,int count)
{
return value_or_raise(self.left(count),self);
}
String right(const String& self,int count)
{
return value_or_raise(self.right(count),self);
}
ScriptArray* split(const String& self,const String& delimiter,bool keep_empty)
{
asIScriptContext* context=asGetActiveContext();
asITypeInfo* type=context?static_cast<asITypeInfo*>(context->GetEngine()->GetUserData(string_array_slot)):nullptr;
if(!type)return nullptr;
std::vector<String> parts=self.split(delimiter.bytes(),keep_empty);
if(parts.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::bad_alloc();
const int size=static_cast<int>(parts.size());
// The array raises the script exception when it cannot allocate its elements.
ScriptArray* array=ScriptArray::create(type,size);
if(!array)return nullptr;
for(int index=0;index<size;++index)*static_cast<String*>(array->at(index))=std::move(parts[static_cast<std::size_t>(index)]);
return array;
}
String join(const ScriptArray& parts,const String& separator)
{
const int size=parts.length();
if(size==0)return String();
std::size_t total=static_cast<std::size_t>(separator.byte_length())*static_cast<std::size_t>(size-1);
for(int index=0;index<size;++index)total+=static_cast<std::size_t>(static_cast<const String*>(parts.at(index))->byte_length());
if(total>static_cast<std::size_t>(String::max_byte_length))throw std::bad_alloc();
String out;
out.reserve(static_cast<int>(total));
for(int index=0;index<size;++index){
if(index)out.append(separator);
out.append(*static_cast<const String*>(parts.at(index)));
}
return out;
}
// array<T>::join(const T &in separator): registered on the template, so it only does anything for array<string>; for other
// element types a string separator does not even compile.
String join_method(const ScriptArray& parts,const void* separator)
{
if(!parts.holds_strings())throw std::runtime_error("join needs an array of strings");
return join(parts,*static_cast<const String*>(separator));
}
String replace(const String& self,const String& old_text,const String& new_text,bool all)
{
return self.replace(old_text.bytes(),new_text.bytes(),all);
}
String trim(const String& self)
{
return self.trim();
}
String trim_left(const String& self)
{
return self.trim_left();
}
String trim_right(const String& self)
{
return self.trim_right();
}
String lower(const String& self)
{
return self.lower();
}
String upper(const String& self)
{
return self.upper();
}
String lower_in(const String& self,const String& locale)
{
return self.lower(locale.bytes());
}
String upper_in(const String& self,const String& locale)
{
return self.upper(locale.bytes());
}
String repeat(const String& self,int count)
{
return value_or_raise(self.repeat(count),self);
}
String reverse(const String& self)
{
return self.reverse();
}
String insert(const String& self,int index,const String& text)
{
return value_or_raise(self.insert(index,text.bytes()),self);
}
String erase(const String& self,int start,int count)
{
return value_or_raise(self.erase(start,count),self);
}
String normalize(const String& self)
{
return self.normalize();
}
double to_number(const String& self)
{
return value_or_raise(self.to_number(),self);
}
std::int64_t to_int(const String& self)
{
return value_or_raise(self.to_int(),self);
}

// format(): one argument of the variadic ?&in list.
void append_argument(String& out,asIScriptEngine& engine,int type_id,const void* address,int string_type)
{
if(type_id==string_type){
out.append(*static_cast<const String*>(address));
return;
}
switch(type_id){
case asTYPEID_BOOL:out.append(load<bool>(address));return;
case asTYPEID_INT8:out.append(load<std::int8_t>(address));return;
case asTYPEID_INT16:out.append(load<std::int16_t>(address));return;
case asTYPEID_INT32:out.append(load<std::int32_t>(address));return;
case asTYPEID_INT64:out.append(load<std::int64_t>(address));return;
case asTYPEID_UINT8:out.append(load<std::uint8_t>(address));return;
case asTYPEID_UINT16:out.append(load<std::uint16_t>(address));return;
case asTYPEID_UINT32:out.append(load<std::uint32_t>(address));return;
case asTYPEID_UINT64:out.append(load<std::uint64_t>(address));return;
case asTYPEID_FLOAT:out.append(load<float>(address));return;
case asTYPEID_DOUBLE:out.append(load<double>(address));return;
default:break;
}
if((type_id&asTYPEID_OBJHANDLE)&&!*static_cast<void* const*>(address)){
out.append("null");
return;
}
const asITypeInfo* type=engine.GetTypeInfoById(type_id);
if(type&&(type->GetFlags()&asOBJ_ENUM)){
append_argument(out,engine,type->GetUnderlyingTypeId(),address,string_type);
return;
}
const char* name=engine.GetTypeDeclaration(type_id&~asTYPEID_OBJHANDLE,true);
out.append(name?name:"?");
}
void format(asIScriptGeneric* gen) noexcept
{
String* result=::new(gen->GetAddressOfReturnLocation()) String();
try{
const String& pattern=*static_cast<const String*>(gen->GetArgObject(0));
const int arguments=gen->GetArgCount();
asIScriptEngine& engine=*gen->GetEngine();
const int string_type=engine.GetStringFactory();
const std::string_view text=pattern.bytes();
String out;
// A guess of 8 bytes per argument, computed in size_t (a pattern near max_byte_length would overflow an int) and clamped,
// since it is only a hint: a longer result still grows, one past the limit still throws.
const std::size_t guess=text.size()+8*static_cast<std::size_t>(arguments>1?arguments-1:0);
out.reserve(static_cast<int>(std::min(guess,static_cast<std::size_t>(String::max_byte_length))));
int next=1;
std::size_t run=0;
for(std::size_t at=text.find_first_of("{}");at!=std::string_view::npos;at=text.find_first_of("{}",at)){
const char brace=text[at];
if(at+1<text.size()&&text[at+1]==brace){
out.append(text.substr(run,at+1-run));
at+=2;
run=at;
}else if(brace=='{'&&at+1<text.size()&&text[at+1]=='}'&&next<arguments){
out.append(text.substr(run,at-run));
append_argument(out,engine,gen->GetArgTypeId(static_cast<asUINT>(next)),gen->GetArgAddress(static_cast<asUINT>(next)),string_type);
++next;
at+=2;
run=at;
}else{
++at;
}
}
out.append(text.substr(run));
*result=std::move(out);
}
catch(...){
raise_current_exception();
}
}

void check(int result,std::string_view declaration)
{
if(result>=0)return;
std::string message="cannot register \"";
message.append(declaration).append("\" (angelscript error ").append(std::to_string(result)).append(")");
throw std::runtime_error(message);
}
class Registrar {
public:
explicit Registrar(asIScriptEngine* engine) noexcept:engine_(engine) {}
void behaviour(asEBehaviours behaviour,const std::string& declaration,const asSFuncPtr& function,asDWORD convention)
{
check(engine_->RegisterObjectBehaviour("string",behaviour,declaration.c_str(),function,convention),declaration);
}
void method(const std::string& declaration,const asSFuncPtr& function,asDWORD convention=asCALL_CDECL_OBJFIRST)
{
check(engine_->RegisterObjectMethod("string",declaration.c_str(),function,convention),declaration);
}
void global(const std::string& declaration,const asSFuncPtr& function,asDWORD convention=asCALL_CDECL)
{
check(engine_->RegisterGlobalFunction(declaration.c_str(),function,convention),declaration);
}
// Construction, assignment and concatenation from one number or bool type.
template<class T>
void value(const char* name)
{
const std::string type(name);
behaviour(asBEHAVE_CONSTRUCT,"void f("+type+") explicit",asFUNCTION(construct_from<T>),asCALL_CDECL_OBJLAST);
method("string &opAssign("+type+")",asFUNCTION(guarded<&assign_value<T>>::call));
method("string &opAddAssign("+type+")",asFUNCTION(guarded<&add_assign_value<T>>::call));
method("string opAdd("+type+") const",asFUNCTION(guarded<&add_value<T>>::call));
method("string opAdd_r("+type+") const",asFUNCTION(guarded<&add_value_reversed<T>>::call));
}
private:
asIScriptEngine* engine_;
};
}

void register_string(asIScriptEngine* engine)
{
if(!engine->GetTypeInfoByName("array"))throw std::runtime_error("cannot register string: register the array type first");
check(engine->RegisterObjectType("string",sizeof(String),asOBJ_VALUE|asOBJ_APP_CLASS_CDAK),"string");
check(engine->RegisterStringFactory("string",&string_factory()),"string factory");
// A template method has to be registered before any instance exists, and split's declaration below creates array<string>.
constexpr const char* join_declaration="string join(const T&in separator) const";
check(engine->RegisterObjectMethod("array<T>",join_declaration,asFUNCTION(guarded<&join_method>::call),asCALL_CDECL_OBJFIRST),join_declaration);
Registrar r(engine);
r.behaviour(asBEHAVE_CONSTRUCT,"void f()",asFUNCTION(construct),asCALL_CDECL_OBJLAST);
r.behaviour(asBEHAVE_CONSTRUCT,"void f(const string &in)",asFUNCTION(copy_construct),asCALL_CDECL_OBJLAST);
r.behaviour(asBEHAVE_DESTRUCT,"void f()",asFUNCTION(destruct),asCALL_CDECL_OBJLAST);
r.method("string &opAssign(const string &in)",asFUNCTION(guarded<&assign>::call));
r.method("string &opAddAssign(const string &in)",asFUNCTION(guarded<&add_assign>::call));
r.method("string opAdd(const string &in) const",asFUNCTION(guarded<&add>::call));
r.method("bool opEquals(const string &in) const",asFUNCTION(equals));
r.method("int opCmp(const string &in) const",asFUNCTION(compare));
// s[i] is read-only (spec 8.1): a get-only index accessor, so an assignment to s[i] is a compile error rather than a write
// to a temporary. The engine's default accessor mode (3) only takes functions declared with "property" as accessors.
r.method("string get_opIndex(int) const property",asFUNCTION(StringBindingAccess::char_at));
r.value<std::int8_t>("int8");
r.value<std::int16_t>("int16");
r.value<std::int32_t>("int");
r.value<std::int64_t>("int64");
r.value<std::uint8_t>("uint8");
r.value<std::uint16_t>("uint16");
r.value<std::uint32_t>("uint");
r.value<std::uint64_t>("uint64");
r.value<float>("float");
r.value<double>("double");
r.value<bool>("bool");
// THISCALL, not the add-on's CDECL_OBJLAST: OBJLAST was 2-3 % slower on the EPYC 7763, 9V74 and both Xeons and 0.6 % faster only on
// the EPYC 9V45 (placement-study 45606248890, Task 5c fix round 2).
r.method("int length() const",asMETHOD(String,length),asCALL_THISCALL);
r.method("int byte_length() const",asMETHOD(String,byte_length),asCALL_THISCALL);
r.method("bool empty() const",asMETHOD(String,empty),asCALL_THISCALL);
r.method("const uint &code_at(int) const",asFUNCTION(code_at));
r.method("const uint8 &byte_at(int) const",asFUNCTION(StringBindingAccess::byte_at),asCALL_CDECL_OBJLAST);
r.method("int find(const string &in needle, int start = 0) const",asFUNCTION(find));
r.method("int rfind(const string &in needle, int start = -1) const",asFUNCTION(guarded<&rfind>::call));
r.method("bool contains(const string &in) const",asFUNCTION(contains));
r.method("bool starts_with(const string &in) const",asFUNCTION(starts_with));
r.method("bool ends_with(const string &in) const",asFUNCTION(ends_with));
r.method("int count(const string &in) const",asFUNCTION(count));
r.method("string substr(int start, int count = -1) const",asFUNCTION(guarded<&substr>::call));
r.method("string left(int) const",asFUNCTION(guarded<&left>::call));
r.method("string right(int) const",asFUNCTION(guarded<&right>::call));
r.method("string replace(const string &in old_text, const string &in new_text, bool all = true) const",asFUNCTION(guarded<&replace>::call));
r.method("string trim() const",asFUNCTION(guarded<&trim>::call));
r.method("string trim_left() const",asFUNCTION(guarded<&trim_left>::call));
r.method("string trim_right() const",asFUNCTION(guarded<&trim_right>::call));
r.method("string lower() const",asFUNCTION(guarded<&lower>::call));
r.method("string upper() const",asFUNCTION(guarded<&upper>::call));
r.method("string lower(const string &in locale) const",asFUNCTION(guarded<&lower_in>::call));
r.method("string upper(const string &in locale) const",asFUNCTION(guarded<&upper_in>::call));
r.method("string repeat(int) const",asFUNCTION(guarded<&repeat>::call));
r.method("string reverse() const",asFUNCTION(guarded<&reverse>::call));
r.method("string insert(int index, const string &in text) const",asFUNCTION(guarded<&insert>::call));
r.method("string erase(int start, int count = -1) const",asFUNCTION(guarded<&erase>::call));
r.method("string normalize() const",asFUNCTION(guarded<&normalize>::call));
r.method("double to_number() const",asFUNCTION(guarded<&to_number>::call));
r.method("int64 to_int() const",asFUNCTION(guarded<&to_int>::call));
r.method("bool is_number() const",asMETHOD(String,is_number),asCALL_THISCALL);
r.method("bool is_digits() const",asMETHOD(String,is_digits),asCALL_THISCALL);
r.method("uint64 hash() const",asMETHOD(String,hash),asCALL_THISCALL);
r.method("array<string>@ split(const string &in delimiter, bool keep_empty = true) const",asFUNCTION(guarded<&split>::call));
r.global("string join(const array<string> &in parts, const string &in separator)",asFUNCTION(guarded<&join>::call));
r.global("string format(const string &in format, const ?&in ...)",asFUNCTION(static_cast<asGENFUNC_t>(&format)),asCALL_GENERIC);
asITypeInfo* array_type=engine->GetTypeInfoByDecl("array<string>");
if(!array_type)throw std::runtime_error("cannot register string: array<string> is not available");
engine->SetUserData(array_type,string_array_slot);
}

bool is_script_string_type(const asITypeInfo* type) noexcept
{
if(!type)return false;
asIStringFactory* factory=nullptr;
const int type_id=type->GetEngine()->GetStringFactory(nullptr,&factory);
return factory==&string_factory()&&type_id==type->GetTypeId();
}
}
