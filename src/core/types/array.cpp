/*
   AngelCode Scripting Library
   Copyright (c) 2003-2026 Andreas Jonsson

   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any
   damages arising from the use of this software.

   Permission is granted to anyone to use this software for any
   purpose, including commercial applications, and to alter it and
   redistribute it freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you
      must not claim that you wrote the original software. If you use
      this software in a product, an acknowledgment in the product
      documentation would be appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and
      must not be misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
      distribution.

   The original version of this library can be located at:
   http://www.angelcode.com/angelscript/

   Andreas Jonsson
   andreas@angelcode.com
*/
// ALTERED SOURCE: element storage, copies, callbacks, garbage-collector support and registration of zodiac::ScriptArray,
// forked from AngelScript 2.39.0's add_on/scriptarray/scriptarray.cpp (see array.hpp for the changes and the contract). The
// native fast paths are in array_fast_paths.cpp.
#include <core/types/array.hpp>
#include <core/types/script_guard.hpp>
#include <core/types/string.hpp>
#include <core/types/string_binding.hpp>
#if defined(ZODIAC_TEST_HOOKS)
#include <chrono>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif
#endif
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

namespace zodiac {
// Per template instance, found once (the add-on's SArrayCache): the element type's opCmp and opEquals for the callback
// paths, its opHndlAssign when it is an asOBJ_ASHANDLE type, and whether it is Zodiac's string.
struct ArrayCache {
asIScriptFunction* compare=nullptr;
asIScriptFunction* equals=nullptr;
asIScriptFunction* handle_assign=nullptr;
int compare_result=0;
int equals_result=0;
bool strings=false;
};

namespace {
// Type user data slot of the cache (the add-on used 1000; Zodiac's own slots spell their owner).
constexpr asPWORD array_cache_slot=0x5A415252;
constexpr std::uint32_t max_elements=INT_MAX;
constexpr const char* index_error="index out of range";
constexpr const char* count_error="negative count";
constexpr const char* memory_error="out of memory";
constexpr const char* changed_error="the array changed while it was sorted";
constexpr const char* searched_error="the array changed while it was searched";

[[gnu::cold,gnu::noinline]] void raise(const char* text) noexcept
{
raise_script_exception(text);
}
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
// Copies one element of 1, 2, 4 or 8 bytes with a fixed-size move.
void copy_element(void* target,const void* source,std::uint32_t size) noexcept
{
switch(size){
case 1:std::memcpy(target,source,1);break;
case 2:std::memcpy(target,source,2);break;
case 4:std::memcpy(target,source,4);break;
default:std::memcpy(target,source,8);break;
}
}
template<class T>
void reverse_as(std::byte* data,std::uint32_t count) noexcept
{
std::reverse(reinterpret_cast<T*>(data),reinterpret_cast<T*>(data)+count);
}
ScriptArray::Kind primitive_kind(int type_id,std::uint32_t size) noexcept
{
using Kind=ScriptArray::Kind;
switch(type_id){
case asTYPEID_BOOL:return Kind::boolean;
case asTYPEID_INT8:return Kind::int8;
case asTYPEID_INT16:return Kind::int16;
case asTYPEID_INT32:return Kind::int32;
case asTYPEID_INT64:return Kind::int64;
case asTYPEID_UINT8:return Kind::uint8;
case asTYPEID_UINT16:return Kind::uint16;
case asTYPEID_UINT32:return Kind::uint32;
case asTYPEID_UINT64:return Kind::uint64;
case asTYPEID_FLOAT:return Kind::float32;
case asTYPEID_DOUBLE:return Kind::float64;
default:
// Enums are signed integers of their underlying size.
return size==1?Kind::int8:size==2?Kind::int16:size==8?Kind::int64:Kind::int32;
}
}

void cleanup_cache(asITypeInfo* type)
{
delete static_cast<ArrayCache*>(type->GetUserData(array_cache_slot));
}
// The add-on's Precache: opCmp and opEquals are looked up once per instance because the search is slow.
void fill_cache(ArrayCache& cache,asITypeInfo* type) noexcept
{
const int sub_type_id=type->GetSubTypeId();
// The methods of a handle-to-const element must be const too.
const bool must_be_const=(sub_type_id&asTYPEID_HANDLETOCONST)!=0;
asITypeInfo* sub_type=type->GetEngine()->GetTypeInfoById(sub_type_id);
if(!sub_type)return;
for(asUINT index=0;index<sub_type->GetMethodCount();++index){
asIScriptFunction* function=sub_type->GetMethodByIndex(index);
if(function->GetParamCount()!=1||(must_be_const&&!function->IsReadOnly()))continue;
asDWORD flags=0;
const int return_type_id=function->GetReturnTypeId(&flags);
// The method must not return a reference; opCmp returns int and opEquals bool.
if(flags!=asTM_NONE)continue;
const bool is_compare=return_type_id==asTYPEID_INT32&&std::strcmp(function->GetName(),"opCmp")==0;
const bool is_equals=return_type_id==asTYPEID_BOOL&&std::strcmp(function->GetName(),"opEquals")==0;
if(!is_compare&&!is_equals)continue;
// The parameter is a reference to the element type or a handle to it.
int parameter_type_id=0;
function->GetParam(0,&parameter_type_id,&flags);
if((parameter_type_id&~(asTYPEID_OBJHANDLE|asTYPEID_HANDLETOCONST))!=(sub_type_id&~(asTYPEID_OBJHANDLE|asTYPEID_HANDLETOCONST)))continue;
if(flags&asTM_INREF){
if((parameter_type_id&asTYPEID_OBJHANDLE)||(must_be_const&&!(flags&asTM_CONST)))continue;
}else if(parameter_type_id&asTYPEID_OBJHANDLE){
if(must_be_const&&!(parameter_type_id&asTYPEID_HANDLETOCONST))continue;
}else{
continue;
}
asIScriptFunction*& slot=is_compare?cache.compare:cache.equals;
int& result=is_compare?cache.compare_result:cache.equals_result;
if(slot||result){
slot=nullptr;
result=asMULTIPLE_FUNCTIONS;
}else{
slot=function;
}
}
if(!cache.equals&&!cache.equals_result)cache.equals_result=asNO_FUNCTION;
if(!cache.compare&&!cache.compare_result)cache.compare_result=asNO_FUNCTION;
const bool handles=(sub_type_id&asTYPEID_OBJHANDLE)!=0;
cache.strings=!handles&&is_script_string_type(sub_type);
if(!handles&&(sub_type->GetFlags()&asOBJ_ASHANDLE)){
try{
const std::string name=sub_type->GetName();
cache.handle_assign=sub_type->GetMethodByDecl((name+"& opHndlAssign(const "+name+"&in)").c_str());
}
catch(...){
}
}
}
ArrayCache* precache(asITypeInfo* type) noexcept
{
if(auto* cache=static_cast<ArrayCache*>(type->GetUserData(array_cache_slot)))return cache;
// Created once even when several threads get here together; the user data is set only when the cache is complete.
asAcquireExclusiveLock();
auto* cache=static_cast<ArrayCache*>(type->GetUserData(array_cache_slot));
if(!cache){
cache=new(std::nothrow) ArrayCache();
if(cache){
fill_cache(*cache,type);
type->SetUserData(cache,array_cache_slot);
}
}
asReleaseExclusiveLock();
if(!cache)raise(memory_error);
return cache;
}

// Calls script functions from inside a native method: on the active context, nested with PushState, when it belongs to the
// same engine (the add-on's way), otherwise on a context from the engine's pool. The first call that does not finish stops
// the work and is passed on when this object goes away: an exception is raised again on the calling context with the same
// text, an abort of a nested call aborts the caller.
class ScriptCalls {
public:
explicit ScriptCalls(asIScriptEngine* engine) noexcept:engine_(engine)
{
asIScriptContext* active=asGetActiveContext();
if(active&&active->GetEngine()==engine&&active->PushState()>=0){
context_=active;
nested_=true;
}else{
context_=engine->RequestContext();
if(!context_)fail(memory_error);
}
}
~ScriptCalls()
{
if(context_){
if(nested_)context_->PopState();
else engine_->ReturnContext(context_);
}
if(aborted_){
if(nested_&&context_)context_->Abort();
}else if(failed_){
raise(error_.empty()&&lost_text_?memory_error:error_.c_str());
}
}
ScriptCalls(const ScriptCalls&)=delete;
ScriptCalls& operator=(const ScriptCalls&)=delete;
bool ok() const noexcept {return !failed_&&!aborted_;}
asIScriptContext* context() const noexcept {return context_;}
bool prepare(asIScriptFunction* function) noexcept
{
if(!ok())return false;
if(context_->Prepare(function)>=0)return true;
fail("the script function cannot be called");
return false;
}
bool execute() noexcept
{
const int result=context_->Execute();
if(result==asEXECUTION_FINISHED)return true;
if(result==asEXECUTION_ABORTED){
aborted_=true;
}else if(result==asEXECUTION_EXCEPTION){
const char* text=context_->GetExceptionString();
fail(text?text:"");
}else{
fail("the script function did not finish");
}
return false;
}
private:
void fail(const char* text) noexcept
{
if(!ok())return;
failed_=true;
try{
error_=text;
}
catch(...){
lost_text_=true;
}
}
asIScriptEngine* engine_;
asIScriptContext* context_=nullptr;
bool nested_=false;
bool failed_=false;
bool aborted_=false;
bool lost_text_=false;
std::string error_;
};

// Marks an array busy for the duration of a callback sort, find or equality: while busy_ holds a text, every operation that
// would destroy elements raises it and changes nothing, so a script call cannot free an element still being compared.
class BusyGuard {
public:
BusyGuard(const char*& busy,const char* text) noexcept:busy_(busy),previous_(busy)
{
busy=text;
}
~BusyGuard()
{
busy_=previous_;
}
BusyGuard(const BusyGuard&)=delete;
BusyGuard& operator=(const BusyGuard&)=delete;
private:
const char*& busy_;
const char* previous_;
};

// 1 when the objects are equal, 0 when not, -1 when the type has neither opEquals nor opCmp or a call failed. Like the
// add-on: the same object (or two null handles) is equal and a null handle equals nothing else; otherwise opEquals, or else
// opCmp()==0, decides. Handle elements are counted during the call, so script code dropping them cannot free them.
int objects_equal(ScriptCalls& calls,const ArrayCache& cache,void* a,void* b,asITypeInfo* held) noexcept
{
if(a==b)return 1;
if(!a||!b)return 0;
asIScriptFunction* function=cache.equals?cache.equals:cache.compare;
if(!function||!calls.prepare(function))return -1;
asIScriptEngine* engine=calls.context()->GetEngine();
if(held){
engine->AddRefScriptObject(a,held);
engine->AddRefScriptObject(b,held);
}
asIScriptContext* context=calls.context();
context->SetObject(a);
context->SetArgObject(0,b);
int result=-1;
if(calls.execute()){
if(cache.equals)result=context->GetReturnByte()!=0?1:0;
else result=static_cast<int>(context->GetReturnDWord())==0?1:0;
}
if(held){
engine->ReleaseScriptObject(a,held);
engine->ReleaseScriptObject(b,held);
}
return result;
}

// Orders the slot copies of a callback sort. The array is busy meanwhile, so the compare cannot destroy elements; every call
// checks that it did not insert any either. After the first failure or change every comparison is false, which ends
// std::stable_sort quickly with the copies still a permutation.
struct CallLess {
using Slot=std::uint64_t;
ScriptCalls* calls;
asIScriptFunction* less;
asIScriptFunction* compare;
const std::uint32_t* count;
std::uint32_t expected_count;
bool descending;
bool indirect;
bool* stopped;
bool* changed;
bool operator()(const Slot& left,const Slot& right) const noexcept
{
if(*stopped)return false;
const Slot& a=descending?right:left;
const Slot& b=descending?left:right;
bool result=false;
if(less){
if(!calls->prepare(less))return stop();
asIScriptContext* context=calls->context();
// The less funcdef takes const T &in: an object itself, or the address of a handle or primitive value.
context->SetArgAddress(0,indirect?load_pointer(&a):const_cast<Slot*>(&a));
context->SetArgAddress(1,indirect?load_pointer(&b):const_cast<Slot*>(&b));
if(!calls->execute())return stop();
result=context->GetReturnByte()!=0;
}else{
void* x=load_pointer(&a);
void* y=load_pointer(&b);
// Null handles sort first (last when descending), as in the add-on.
if(!x||!y)return !x&&y;
if(!calls->prepare(compare))return stop();
asIScriptContext* context=calls->context();
context->SetObject(x);
context->SetArgObject(0,y);
if(!calls->execute())return stop();
result=static_cast<int>(context->GetReturnDWord())<0;
}
if(*count!=expected_count){
*changed=true;
return stop();
}
return result;
}
bool stop() const noexcept
{
*stopped=true;
return false;
}
};

int for_begin(const ScriptArray*) noexcept
{
return 0;
}
bool for_end(int iterator,const ScriptArray* array) noexcept
{
return !array||iterator>=array->length();
}
int for_next(int iterator,const ScriptArray*) noexcept
{
return iterator+1;
}
int for_value1(int iterator,const ScriptArray*) noexcept
{
return iterator;
}

void write_type_error(asITypeInfo* type,asITypeInfo* sub_type,const char* problem) noexcept
{
char message[512];
std::snprintf(message,sizeof message,"the element type \"%s\" has no %s",sub_type?sub_type->GetEngine()->GetTypeDeclaration(sub_type->GetTypeId()):"?",problem);
type->GetEngine()->WriteMessage("array",0,0,asMSGTYPE_ERROR,message);
}
// The add-on's template callback: an element type needs a default constructor or factory, and an instance is garbage
// collected only when its elements can form reference cycles.
bool template_callback(asITypeInfo* type,bool& no_gc)
{
const int type_id=type->GetSubTypeId();
if(type_id==asTYPEID_VOID)return false;
asIScriptEngine* engine=type->GetEngine();
if((type_id&asTYPEID_MASK_OBJECT)&&!(type_id&asTYPEID_OBJHANDLE)){
asITypeInfo* sub_type=engine->GetTypeInfoById(type_id);
const asQWORD flags=sub_type->GetFlags();
if((flags&asOBJ_VALUE)&&!(flags&asOBJ_POD)){
bool found=false;
for(asUINT index=0;index<sub_type->GetBehaviourCount()&&!found;++index){
asEBehaviours behaviour;
asIScriptFunction* function=sub_type->GetBehaviourByIndex(index,&behaviour);
found=behaviour==asBEHAVE_CONSTRUCT&&function->GetParamCount()==0;
}
if(!found){
write_type_error(type,sub_type,"default constructor");
return false;
}
}else if(flags&asOBJ_REF){
// With value assignment of reference types disabled no element can be created.
bool found=false;
if(!engine->GetEngineProperty(asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE)){
for(asUINT index=0;index<sub_type->GetFactoryCount()&&!found;++index)found=sub_type->GetFactoryByIndex(index)->GetParamCount()==0;
}
if(!found){
write_type_error(type,sub_type,"default factory");
return false;
}
}
if(!(flags&asOBJ_GC))no_gc=true;
}else if(!(type_id&asTYPEID_OBJHANDLE)){
// Primitives cannot form cycles.
no_gc=true;
}else{
// A handle can only form a cycle through a garbage-collected type, or a script class something may derive from.
const asQWORD flags=engine->GetTypeInfoById(type_id)->GetFlags();
if(!(flags&asOBJ_GC)&&(!(flags&asOBJ_SCRIPT_OBJECT)||(flags&asOBJ_NOINHERIT)))no_gc=true;
}
return true;
}

void check(int result,const char* declaration)
{
if(result>=0)return;
throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
}

ScriptArray::ScriptArray(asITypeInfo* type) noexcept:sub_type_id_(type->GetSubTypeId()),type_(type)
{
type_->AddRef();
if(sub_type_id_&asTYPEID_MASK_OBJECT){
element_size_=sizeof(void*);
indirect_=!(sub_type_id_&asTYPEID_OBJHANDLE);
kind_=indirect_?Kind::object:Kind::handle;
cache_=precache(type);
if(indirect_&&cache_&&cache_->strings)kind_=Kind::string;
}else{
element_size_=static_cast<std::uint32_t>(engine()->GetSizeOfPrimitiveType(sub_type_id_));
// An enum compares as its underlying type, which may be unsigned.
int value_type_id=sub_type_id_;
if(value_type_id>asTYPEID_DOUBLE){
if(asITypeInfo* enum_type=engine()->GetTypeInfoById(value_type_id))value_type_id=enum_type->GetUnderlyingTypeId();
}
kind_=primitive_kind(value_type_id,element_size_);
}
}

#if defined(ZODIAC_TEST_HOOKS)
testing::StringArraySteps& testing::string_array_steps() noexcept
{
static StringArraySteps steps{};
return steps;
}
namespace {
std::uint64_t step_clock() noexcept
{
#if defined(__x86_64__)
return __rdtsc();
#else
return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
// Adds the cycles since mark to field and moves mark on.
void step(std::uint64_t& mark,std::uint64_t testing::StringArraySteps::*field) noexcept
{
const std::uint64_t now=step_clock();
testing::string_array_steps().*field+=now-mark;
mark=now;
}
}
#define ZODIAC_STEP_MARK std::uint64_t step_mark=step_clock()
#define ZODIAC_STEP(field) step(step_mark,&testing::StringArraySteps::field)
#define ZODIAC_STEP_IF(condition,field) do{if(condition)ZODIAC_STEP(field);}while(0)
#else
#define ZODIAC_STEP_MARK do{}while(0)
#define ZODIAC_STEP(field) do{}while(0)
#define ZODIAC_STEP_IF(condition,field) do{}while(0)
#endif

ScriptArray::ScriptArray(asITypeInfo* type,ArrayCache* strings) noexcept:kind_(Kind::string),indirect_(true),element_size_(sizeof(void*)),sub_type_id_(type->GetSubTypeId()),type_(type),cache_(strings)
{
type_->AddRef();
}

ArrayCache* ScriptArray::cache_for(asITypeInfo* type) noexcept
{
return precache(type);
}

ScriptArray::~ScriptArray()
{
#if defined(ZODIAC_TEST_HOOKS)
const bool timed=timed_;
#endif
ZODIAC_STEP_MARK;
if(kind_==Kind::string){
// Zodiac's strings directly: what ReleaseScriptObject does for this value type (its destructor, then the engine's free,
// which is asFreeMem's), without the engine's generic calls per element.
for(std::uint32_t index=0;index<count_;++index){
if(void* pointer=load_pointer(slot(index))){
static_cast<String*>(pointer)->~String();
ZODIAC_STEP_IF(timed,string_destroy);
asFreeMem(pointer);
ZODIAC_STEP_IF(timed,string_free);
}
}
}else if(has_objects()){
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
for(std::uint32_t index=0;index<count_;++index){
if(void* pointer=load_pointer(slot(index)))owner->ReleaseScriptObject(pointer,element_type);
}
}
std::free(data_);
ZODIAC_STEP_IF(timed,slots_free);
type_->Release();
ZODIAC_STEP_IF(timed,type_release);
}

ScriptArray* ScriptArray::allocate(asITypeInfo* type) noexcept
{
void* memory=std::malloc(sizeof(ScriptArray));
if(!memory){
raise(memory_error);
return nullptr;
}
return new(memory) ScriptArray(type);
}

void ScriptArray::notify_gc() noexcept
{
if(type_->GetFlags()&asOBJ_GC)engine()->NotifyGarbageCollectorOfNewObject(this,type_);
}

ScriptArray* ScriptArray::create(asITypeInfo* type) noexcept
{
ScriptArray* array=allocate(type);
if(array)array->notify_gc();
return array;
}

ScriptArray* ScriptArray::create(asITypeInfo* type,int length) noexcept
{
if(length<0){
raise(count_error);
return nullptr;
}
ScriptArray* array=allocate(type);
if(!array)return nullptr;
array->resize(length);
if(array->length()!=length){
array->release();
return nullptr;
}
array->notify_gc();
return array;
}

ScriptArray* ScriptArray::create_strings(asITypeInfo* type,int capacity,ArrayCache* cache) noexcept
{
if(capacity<0){
raise(count_error);
return nullptr;
}
ScriptArray* array;
ZODIAC_STEP_MARK;
if(cache&&cache->strings){
void* memory=std::malloc(sizeof(ScriptArray));
if(!memory){
raise(memory_error);
return nullptr;
}
ZODIAC_STEP(array_malloc);
array=new(memory) ScriptArray(type,cache);
ZODIAC_STEP(type_add_ref);
}else{
array=allocate(type);
if(!array)return nullptr;
if(array->kind_!=Kind::string){
raise("create_strings needs array<string>");
array->release();
return nullptr;
}
}
if(!array->set_capacity(static_cast<std::uint32_t>(capacity))){
array->release();
return nullptr;
}
ZODIAC_STEP(slots_alloc);
#if defined(ZODIAC_TEST_HOOKS)
array->timed_=true;
++testing::string_array_steps().arrays;
#endif
array->notify_gc();
return array;
}

bool ScriptArray::append_string(const String& text) noexcept
{
if(count_>=capacity_){
raise(memory_error);
return false;
}
ZODIAC_STEP_MARK;
void* memory=asAllocMem(sizeof(String));
if(!memory){
raise(memory_error);
return false;
}
ZODIAC_STEP(string_alloc);
try{
new(memory) String(text);
ZODIAC_STEP(string_copy);
#if defined(ZODIAC_TEST_HOOKS)
++testing::string_array_steps().strings;
#endif
}
catch(...){
asFreeMem(memory);
raise(memory_error);
return false;
}
store_pointer(slot(count_),memory);
++count_;
return true;
}

ScriptArray* ScriptArray::create(asITypeInfo* type,int length,const void* value) noexcept
{
if(length<0){
raise(count_error);
return nullptr;
}
ScriptArray* array=allocate(type);
if(!array)return nullptr;
const auto count=static_cast<std::uint32_t>(length);
if(!array->set_capacity(count)){
array->release();
return nullptr;
}
for(std::uint32_t index=0;index<count;++index){
if(!array->store_copy(array->slot(index),value)){
array->release();
return nullptr;
}
++array->count_;
}
array->notify_gc();
return array;
}

ScriptArray* ScriptArray::create_from_list(asITypeInfo* type,void* list) noexcept
{
asUINT length;
std::memcpy(&length,list,sizeof length);
std::byte* source=static_cast<std::byte*>(list)+sizeof length;
ScriptArray* array=allocate(type);
if(!array)return nullptr;
if(!array->set_capacity(length)){
array->release();
return nullptr;
}
const int sub_type_id=array->sub_type_id_;
if(!(sub_type_id&asTYPEID_MASK_OBJECT)||(sub_type_id&asTYPEID_OBJHANDLE)||(array->sub_type()->GetFlags()&asOBJ_REF)){
// Primitives are copied. Handles and reference-type objects are taken over from the list, which the engine then finds
// cleared and releases nothing from.
const std::size_t bytes=static_cast<std::size_t>(length)*array->element_size_;
if(bytes){
std::memcpy(array->data_,source,bytes);
if(sub_type_id&asTYPEID_MASK_OBJECT)std::memset(source,0,bytes);
}
array->count_=length;
}else{
// Value types lie in the list one after the other; each element is made as a copy.
const std::size_t size=array->sub_type()->GetSize();
for(asUINT index=0;index<length;++index){
if(!array->store_copy(array->slot(index),source+index*size)){
array->release();
return nullptr;
}
++array->count_;
}
}
array->notify_gc();
return array;
}

void ScriptArray::add_ref() const noexcept
{
// Clear the GC flag, then count.
gc_flag_=false;
asAtomicInc(ref_count_);
}

void ScriptArray::release() const noexcept
{
gc_flag_=false;
if(asAtomicDec(ref_count_)==0){
ScriptArray* self=const_cast<ScriptArray*>(this);
#if defined(ZODIAC_TEST_HOOKS)
const bool timed=timed_;
#endif
self->~ScriptArray();
ZODIAC_STEP_MARK;
std::free(self);
ZODIAC_STEP_IF(timed,array_free);
}
}

int ScriptArray::array_type_id() const noexcept
{
return type_->GetTypeId();
}

const void* ScriptArray::element_address(std::uint32_t index) const noexcept
{
std::byte* place=slot(index);
return indirect_?load_pointer(place):place;
}

void* ScriptArray::at(int index) noexcept
{
return const_cast<void*>(static_cast<const ScriptArray*>(this)->at(index));
}

const void* ScriptArray::at(int index) const noexcept
{
if(static_cast<std::uint32_t>(index)>=count_)[[unlikely]]{
raise(index_error);
return nullptr;
}
return element_address(static_cast<std::uint32_t>(index));
}

bool ScriptArray::set_capacity(std::uint32_t capacity) noexcept
{
if(capacity==capacity_)return true;
if(capacity>max_elements){
raise(memory_error);
return false;
}
if(capacity==0){
std::free(data_);
data_=nullptr;
capacity_=0;
return true;
}
// Elements are primitives or pointers, so moving the block moves them.
void* memory=std::realloc(data_,static_cast<std::size_t>(capacity)*element_size_);
if(!memory){
raise(memory_error);
return false;
}
data_=static_cast<std::byte*>(memory);
capacity_=capacity;
return true;
}

bool ScriptArray::grow(std::uint32_t needed) noexcept
{
if(needed<=capacity_)return true;
if(needed>max_elements){
raise(memory_error);
return false;
}
// Doubling keeps appends amortized O(1); when the doubled block cannot be had, the exact size is tried.
const std::uint32_t doubled=capacity_<max_elements/2?std::max<std::uint32_t>(capacity_*2,8):max_elements;
const std::uint32_t wanted=std::max(needed,doubled);
if(void* memory=std::realloc(data_,static_cast<std::size_t>(wanted)*element_size_)){
data_=static_cast<std::byte*>(memory);
capacity_=wanted;
return true;
}
return set_capacity(needed);
}

bool ScriptArray::make_room(std::uint32_t at,std::uint32_t count) noexcept
{
if(count>max_elements-count_){
raise(memory_error);
return false;
}
if(!grow(count_+count))return false;
if(at<count_)std::memmove(slot(at+count),slot(at),static_cast<std::size_t>(count_-at)*element_size_);
count_+=count;
return true;
}

bool ScriptArray::store_copy(std::byte* target,const void* value) noexcept
{
if(!has_objects()){
copy_element(target,value,element_size_);
return true;
}
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
void* pointer;
if(kind_==Kind::handle){
pointer=load_pointer(value);
if(pointer)owner->AddRefScriptObject(pointer,element_type);
}else if(cache_&&cache_->handle_assign){
pointer=owner->CreateScriptObject(element_type);
if(!pointer)return false;
if(!assign_object(pointer,value)){
owner->ReleaseScriptObject(pointer,element_type);
return false;
}
}else{
// One copy construction instead of the add-on's default construction plus assignment.
pointer=owner->CreateScriptObjectCopy(const_cast<void*>(value),element_type);
if(!pointer)return false;
}
store_pointer(target,pointer);
return true;
}

void ScriptArray::destroy_slot(std::byte* place) noexcept
{
if(!has_objects())return;
if(void* pointer=load_pointer(place))engine()->ReleaseScriptObject(pointer,sub_type());
}

bool ScriptArray::assign_object(void* target,const void* value) noexcept
{
if(cache_&&cache_->handle_assign){
// asOBJ_ASHANDLE types are copied by their opHndlAssign.
ScriptCalls calls(engine());
if(!calls.prepare(cache_->handle_assign))return false;
calls.context()->SetObject(target);
calls.context()->SetArgAddress(0,const_cast<void*>(value));
return calls.execute();
}
return engine()->AssignScriptObject(target,const_cast<void*>(value),sub_type())>=0;
}

void ScriptArray::erase_slots(std::uint32_t first,std::uint32_t count) noexcept
{
if(!count)return;
if(busy_){
raise(busy_);
return;
}
const std::size_t tail=static_cast<std::size_t>(count_-first-count)*element_size_;
if(!has_objects()){
if(tail)std::memmove(slot(first),slot(first+count),tail);
count_-=count;
return;
}
// Removed objects are released once the array is consistent again: their destructors may run script code that uses it,
// or even release the array, so nothing of it is touched after the first release.
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
constexpr std::uint32_t local_size=16;
void* local[local_size];
void** removed=count<=local_size?local:static_cast<void**>(std::malloc(static_cast<std::size_t>(count)*sizeof(void*)));
if(!removed){
// Without memory for the list the slots are cleared and released in place, the add-on's order.
for(std::uint32_t index=0;index<count;++index){
void* pointer=load_pointer(slot(first+index));
store_pointer(slot(first+index),nullptr);
if(pointer)owner->ReleaseScriptObject(pointer,element_type);
}
if(first+count<=count_){
const std::size_t rest=static_cast<std::size_t>(count_-first-count)*element_size_;
if(rest)std::memmove(slot(first),slot(first+count),rest);
count_-=count;
}
return;
}
std::memcpy(removed,slot(first),static_cast<std::size_t>(count)*sizeof(void*));
if(tail)std::memmove(slot(first),slot(first+count),tail);
count_-=count;
for(std::uint32_t index=0;index<count;++index){
if(removed[index])owner->ReleaseScriptObject(removed[index],element_type);
}
if(removed!=local)std::free(removed);
}

void ScriptArray::reserve(int capacity) noexcept
{
if(capacity<0){
raise(count_error);
return;
}
if(static_cast<std::uint32_t>(capacity)>capacity_)set_capacity(static_cast<std::uint32_t>(capacity));
}

bool ScriptArray::try_reserve(int capacity) noexcept
{
if(capacity<=static_cast<int>(capacity_))return true;
void* memory=std::realloc(data_,static_cast<std::size_t>(capacity)*element_size_);
if(!memory)return false;
data_=static_cast<std::byte*>(memory);
capacity_=static_cast<std::uint32_t>(capacity);
return true;
}

void ScriptArray::resize(int length) noexcept
{
if(length<0){
raise(count_error);
return;
}
const auto target=static_cast<std::uint32_t>(length);
if(target<count_){
erase_slots(target,count_-target);
return;
}
if(target==count_)return;
if(kind_<Kind::string||kind_==Kind::handle){
// Primitives start as zero and handles as null.
const std::uint32_t first=count_;
if(make_room(first,target-first))std::memset(slot(first),0,static_cast<std::size_t>(target-first)*element_size_);
return;
}
if(!grow(target))return;
// One object at a time, each added once it exists: constructors may run script code that uses the array.
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
while(count_<target){
void* pointer=owner->CreateScriptObject(element_type);
if(!pointer)return;
if(!make_room(count_,1)){
owner->ReleaseScriptObject(pointer,element_type);
return;
}
store_pointer(slot(count_-1),pointer);
}
}

void ScriptArray::clear() noexcept
{
erase_slots(0,count_);
}

ScriptArray& ScriptArray::assign(const ScriptArray& other) noexcept
{
// Like the add-on: only arrays of the same type are copied.
if(&other==this||other.type_!=type_)return *this;
if(busy_){
raise(busy_);
return *this;
}
if(!has_objects()){
const std::uint32_t count=other.count_;
if(count>capacity_&&!set_capacity(count))return *this;
if(count)std::memcpy(data_,other.data_,static_cast<std::size_t>(count)*element_size_);
count_=count;
return *this;
}
resize(static_cast<int>(other.count_));
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
// Bounds are read again on every element: releases and assignments may run script code that changes either array.
for(std::uint32_t index=0;index<count_&&index<other.count_;++index){
if(kind_==Kind::handle){
void* incoming=load_pointer(other.slot(index));
if(incoming)owner->AddRefScriptObject(incoming,element_type);
void* old=load_pointer(slot(index));
store_pointer(slot(index),incoming);
// The old handle is released after the new one is counted, in case both are the same object.
if(old)owner->ReleaseScriptObject(old,element_type);
}else if(!assign_object(load_pointer(slot(index)),load_pointer(other.slot(index)))){
break;
}
}
return *this;
}

bool ScriptArray::equals(const ScriptArray& other) const noexcept
{
if(other.type_!=type_||other.count_!=count_)return false;
if(native())return native_equals(other);
if(!count_)return true;
// Like find: elements without opEquals or opCmp cannot be compared.
if(!check_compare(true))return false;
const BusyGuard busy(busy_,searched_error);
const BusyGuard other_busy(other.busy_,searched_error);
asITypeInfo* held=kind_==Kind::handle?sub_type():nullptr;
ScriptCalls calls(engine());
for(std::uint32_t index=0;index<count_&&index<other.count_;++index){
if(objects_equal(calls,*cache_,load_pointer(slot(index)),load_pointer(other.slot(index)),held)!=1)return false;
}
return count_==other.count_;
}

void ScriptArray::insert_at(int index,const void* value) noexcept
{
if(index<0||static_cast<std::uint32_t>(index)>count_){
raise(index_error);
return;
}
// The new element is made before the array moves: value may be one of its own elements.
alignas(8) std::byte element[8];
if(!store_copy(element,value))return;
const auto at=std::min(static_cast<std::uint32_t>(index),count_);
if(!make_room(at,1)){
destroy_slot(element);
return;
}
copy_element(slot(at),element,element_size_);
}

void ScriptArray::insert_at(int index,const ScriptArray& values) noexcept
{
if(index<0||static_cast<std::uint32_t>(index)>count_){
raise(index_error);
return;
}
if(values.type_!=type_){
raise("the arrays have different element types");
return;
}
const std::uint32_t count=values.count_;
if(!count)return;
// Copies first (values may be this array), then one move into place.
auto* copies=static_cast<std::byte*>(std::malloc(static_cast<std::size_t>(count)*element_size_));
if(!copies){
raise(memory_error);
return;
}
std::uint32_t made=0;
bool failed=false;
for(;made<count&&made<values.count_;++made){
if(!store_copy(copies+static_cast<std::size_t>(made)*element_size_,values.element_address(made))){
failed=true;
break;
}
}
if(!failed&&static_cast<std::uint32_t>(index)>count_){
raise(index_error);
failed=true;
}
if(!failed&&make_room(static_cast<std::uint32_t>(index),made)){
std::memcpy(slot(static_cast<std::uint32_t>(index)),copies,static_cast<std::size_t>(made)*element_size_);
}else{
for(std::uint32_t at=0;at<made;++at)destroy_slot(copies+static_cast<std::size_t>(at)*element_size_);
}
std::free(copies);
}

void ScriptArray::insert_last(const void* value) noexcept
{
if(!has_objects()&&count_<capacity_)[[likely]]{
copy_element(slot(count_),value,element_size_);
++count_;
return;
}
insert_at(static_cast<int>(count_),value);
}

void ScriptArray::remove_at(int index) noexcept
{
if(static_cast<std::uint32_t>(index)>=count_){
raise(index_error);
return;
}
erase_slots(static_cast<std::uint32_t>(index),1);
}

void ScriptArray::remove_last() noexcept
{
if(!count_){
raise(index_error);
return;
}
erase_slots(count_-1,1);
}

void ScriptArray::remove_range(int start,int count) noexcept
{
if(start<0||static_cast<std::uint32_t>(start)>count_){
raise(index_error);
return;
}
if(count<-1){
raise(count_error);
return;
}
const std::uint32_t first=static_cast<std::uint32_t>(start);
const std::uint32_t rest=count_-first;
erase_slots(first,count==-1?rest:std::min(static_cast<std::uint32_t>(count),rest));
}

bool ScriptArray::check_compare(bool equality) const noexcept
{
if(cache_&&(cache_->compare||(equality&&cache_->equals)))return true;
const bool ambiguous=cache_&&(cache_->compare_result==asMULTIPLE_FUNCTIONS||(equality&&cache_->equals_result==asMULTIPLE_FUNCTIONS));
char message[512];
std::snprintf(message,sizeof message,"type \"%s\" has %s %s",sub_type()->GetName(),ambiguous?"more than one":"no",equality?"opEquals or opCmp method":"opCmp method");
raise(message);
return false;
}

int ScriptArray::find(const void* value,int start) const noexcept
{
if(start<0||static_cast<std::uint32_t>(start)>count_){
raise(index_error);
return -1;
}
if(native())return native_find(value,static_cast<std::uint32_t>(start));
if(!check_compare(true))return -1;
// A handle parameter is the address of the handle.
void* needle=kind_==Kind::handle?load_pointer(value):const_cast<void*>(value);
const BusyGuard busy(busy_,searched_error);
asITypeInfo* held=kind_==Kind::handle?sub_type():nullptr;
ScriptCalls calls(engine());
for(std::uint32_t index=static_cast<std::uint32_t>(start);index<count_;++index){
const int result=objects_equal(calls,*cache_,load_pointer(slot(index)),needle,held);
if(result==1)return static_cast<int>(index);
if(result<0)return -1;
}
return -1;
}

int ScriptArray::find_by_ref(const void* value,int start) const noexcept
{
if(start<0||static_cast<std::uint32_t>(start)>count_){
raise(index_error);
return -1;
}
if(kind_==Kind::handle){
const void* needle=load_pointer(value);
for(std::uint32_t index=static_cast<std::uint32_t>(start);index<count_;++index){
if(load_pointer(slot(index))==needle)return static_cast<int>(index);
}
}else{
for(std::uint32_t index=static_cast<std::uint32_t>(start);index<count_;++index){
if(element_address(index)==value)return static_cast<int>(index);
}
}
return -1;
}

bool ScriptArray::contains(const void* value) const noexcept
{
return find(value,0)>=0;
}

void ScriptArray::sort_ascending() noexcept
{
if(native()){
native_sort(false);
return;
}
if(check_compare(false))sort_with_calls(nullptr,false);
}

void ScriptArray::sort_descending() noexcept
{
if(native()){
native_sort(true);
return;
}
if(check_compare(false))sort_with_calls(nullptr,true);
}

void ScriptArray::sort(asIScriptFunction* less) noexcept
{
if(!less){
raise("the compare function is null");
return;
}
sort_with_calls(less,false);
}

bool ScriptArray::sort_with_calls(asIScriptFunction* less,bool descending) noexcept
{
const std::uint32_t count=count_;
if(count<2)return true;
// The sort runs on copies of the slots, so the array stays whole for the garbage collector and for the compare, which
// may read it. The array is busy meanwhile (no element can be destroyed) and handles are counted for the duration, so a
// compare that replaces handles cannot free them either.
using Slot=CallLess::Slot;
auto* order=static_cast<Slot*>(std::malloc(static_cast<std::size_t>(count)*sizeof(Slot)));
if(!order){
raise(memory_error);
return false;
}
for(std::uint32_t index=0;index<count;++index){
order[index]=0;
copy_element(&order[index],slot(index),element_size_);
}
asIScriptEngine* owner=engine();
asITypeInfo* element_type=sub_type();
const bool handles=kind_==Kind::handle;
if(handles){
for(std::uint32_t index=0;index<count;++index){
if(void* pointer=load_pointer(&order[index]))owner->AddRefScriptObject(pointer,element_type);
}
}
bool stopped=false;
bool changed=false;
{
const BusyGuard busy(busy_,changed_error);
ScriptCalls calls(owner);
const CallLess compare{&calls,less,cache_?cache_->compare:nullptr,&count_,count,descending,indirect_,&stopped,&changed};
if(calls.ok())std::stable_sort(order,order+count,compare);
else stopped=true;
if(!stopped&&count_!=count){
stopped=true;
changed=true;
}
if(!stopped){
// Written back in one pass with no call in between; the handles the array held move into order and are released below.
for(std::uint32_t index=0;index<count;++index){
Slot old=0;
copy_element(&old,slot(index),element_size_);
copy_element(slot(index),&order[index],element_size_);
order[index]=old;
}
}
}
if(handles){
for(std::uint32_t index=0;index<count;++index){
if(void* pointer=load_pointer(&order[index]))owner->ReleaseScriptObject(pointer,element_type);
}
}
std::free(order);
// Raised after the calls have ended, so the exception lands on the caller's context.
if(changed)raise(changed_error);
return !stopped;
}

void ScriptArray::reverse() noexcept
{
if(count_<2)return;
switch(element_size_){
case 1:reverse_as<std::uint8_t>(data_,count_);break;
case 2:reverse_as<std::uint16_t>(data_,count_);break;
case 4:reverse_as<std::uint32_t>(data_,count_);break;
default:reverse_as<std::uint64_t>(data_,count_);break;
}
}

ScriptArray* ScriptArray::slice(int start,int count) const noexcept
{
if(start<0||static_cast<std::uint32_t>(start)>count_){
raise(index_error);
return nullptr;
}
if(count<-1){
raise(count_error);
return nullptr;
}
const auto first=static_cast<std::uint32_t>(start);
const std::uint32_t rest=count_-first;
const std::uint32_t size=count==-1?rest:std::min(static_cast<std::uint32_t>(count),rest);
ScriptArray* result=allocate(type_);
if(!result)return nullptr;
if(!result->set_capacity(size)){
result->release();
return nullptr;
}
if(!has_objects()){
if(size)std::memcpy(result->data_,slot(first),static_cast<std::size_t>(size)*element_size_);
result->count_=size;
}else{
for(std::uint32_t index=0;index<size&&first+index<count_;++index){
if(!result->store_copy(result->slot(index),element_address(first+index))){
result->release();
return nullptr;
}
++result->count_;
}
}
result->notify_gc();
return result;
}

int ScriptArray::ref_count() const noexcept
{
return ref_count_;
}

void ScriptArray::set_gc_flag() noexcept
{
gc_flag_=true;
}

bool ScriptArray::gc_flag() const noexcept
{
return gc_flag_;
}

void ScriptArray::enum_references(asIScriptEngine* engine) noexcept
{
if(!(sub_type_id_&asTYPEID_MASK_OBJECT))return;
asITypeInfo* element_type=engine->GetTypeInfoById(sub_type_id_);
const asQWORD flags=element_type->GetFlags();
if(flags&asOBJ_REF){
// Every referenced instance is reported.
for(std::uint32_t index=0;index<count_;++index){
if(void* pointer=load_pointer(slot(index)))engine->GCEnumCallback(pointer);
}
}else if((flags&asOBJ_VALUE)&&(flags&asOBJ_GC)){
// Garbage-collected value types report their own references.
for(std::uint32_t index=0;index<count_;++index){
if(void* pointer=load_pointer(slot(index)))engine->ForwardGCEnumReferences(pointer,element_type);
}
}
}

void ScriptArray::release_all_handles(asIScriptEngine*) noexcept
{
clear();
}

void register_array(asIScriptEngine* engine)
{
engine->SetTypeInfoUserDataCleanupCallback(cleanup_cache,array_cache_slot);
check(engine->RegisterObjectType("array<class T>",0,asOBJ_REF|asOBJ_GC|asOBJ_TEMPLATE),"array<class T>");
auto behaviour=[engine](asEBehaviours kind,const char* declaration,const asSFuncPtr& function,asDWORD convention){
check(engine->RegisterObjectBehaviour("array<T>",kind,declaration,function,convention),declaration);
};
auto method=[engine](const char* declaration,const asSFuncPtr& function,asDWORD convention){
check(engine->RegisterObjectMethod("array<T>",declaration,function,convention),declaration);
};
behaviour(asBEHAVE_TEMPLATE_CALLBACK,"bool f(int&in, bool&out)",asFUNCTION(template_callback),asCALL_CDECL);
// Templates receive their instance type as a hidden first parameter.
behaviour(asBEHAVE_FACTORY,"array<T>@ f(int&in)",asFUNCTIONPR(ScriptArray::create,(asITypeInfo*),ScriptArray*),asCALL_CDECL);
behaviour(asBEHAVE_FACTORY,"array<T>@ f(int&in, int length) explicit",asFUNCTIONPR(ScriptArray::create,(asITypeInfo*,int),ScriptArray*),asCALL_CDECL);
behaviour(asBEHAVE_FACTORY,"array<T>@ f(int&in, int length, const T &in value)",asFUNCTIONPR(ScriptArray::create,(asITypeInfo*,int,const void*),ScriptArray*),asCALL_CDECL);
behaviour(asBEHAVE_LIST_FACTORY,"array<T>@ f(int&in type, int&in list) {repeat T}",asFUNCTION(ScriptArray::create_from_list),asCALL_CDECL);
behaviour(asBEHAVE_ADDREF,"void f()",asMETHOD(ScriptArray,add_ref),asCALL_THISCALL);
behaviour(asBEHAVE_RELEASE,"void f()",asMETHOD(ScriptArray,release),asCALL_THISCALL);
behaviour(asBEHAVE_GETREFCOUNT,"int f()",asMETHOD(ScriptArray,ref_count),asCALL_THISCALL);
behaviour(asBEHAVE_SETGCFLAG,"void f()",asMETHOD(ScriptArray,set_gc_flag),asCALL_THISCALL);
behaviour(asBEHAVE_GETGCFLAG,"bool f()",asMETHOD(ScriptArray,gc_flag),asCALL_THISCALL);
behaviour(asBEHAVE_ENUMREFS,"void f(int&in)",asMETHOD(ScriptArray,enum_references),asCALL_THISCALL);
behaviour(asBEHAVE_RELEASEREFS,"void f(int&in)",asMETHOD(ScriptArray,release_all_handles),asCALL_THISCALL);
method("T &opIndex(int index)",asMETHODPR(ScriptArray,at,(int),void*),asCALL_THISCALL);
method("const T &opIndex(int index) const",asMETHODPR(ScriptArray,at,(int) const,const void*),asCALL_THISCALL);
method("int opForBegin() const",asFUNCTION(for_begin),asCALL_CDECL_OBJLAST);
method("bool opForEnd(int) const",asFUNCTION(for_end),asCALL_CDECL_OBJLAST);
method("int opForNext(int) const",asFUNCTION(for_next),asCALL_CDECL_OBJLAST);
method("const T &opForValue0(int index) const",asMETHODPR(ScriptArray,at,(int) const,const void*),asCALL_THISCALL);
method("int opForValue1(int index) const",asFUNCTION(for_value1),asCALL_CDECL_OBJLAST);
method("array<T> &opAssign(const array<T>&in)",asMETHOD(ScriptArray,assign),asCALL_THISCALL);
method("bool opEquals(const array<T>&in) const",asMETHOD(ScriptArray,equals),asCALL_THISCALL);
method("int length() const",asMETHOD(ScriptArray,length),asCALL_THISCALL);
method("bool empty() const",asMETHOD(ScriptArray,empty),asCALL_THISCALL);
method("void resize(int length)",asMETHOD(ScriptArray,resize),asCALL_THISCALL);
method("void reserve(int length)",asMETHOD(ScriptArray,reserve),asCALL_THISCALL);
method("void clear()",asMETHOD(ScriptArray,clear),asCALL_THISCALL);
method("void insert_at(int index, const T&in value)",asMETHODPR(ScriptArray,insert_at,(int,const void*),void),asCALL_THISCALL);
method("void insert_at(int index, const array<T>& values)",asMETHODPR(ScriptArray,insert_at,(int,const ScriptArray&),void),asCALL_THISCALL);
method("void insert_last(const T&in value)",asMETHOD(ScriptArray,insert_last),asCALL_THISCALL);
method("void remove_at(int index)",asMETHOD(ScriptArray,remove_at),asCALL_THISCALL);
method("void remove_last()",asMETHOD(ScriptArray,remove_last),asCALL_THISCALL);
method("void remove_range(int start, int count = -1)",asMETHOD(ScriptArray,remove_range),asCALL_THISCALL);
// if_handle_then_const: for handle elements the value is a read-only object.
method("int find(const T&in if_handle_then_const value, int start = 0) const",asMETHOD(ScriptArray,find),asCALL_THISCALL);
method("int find_by_ref(const T&in if_handle_then_const value, int start = 0) const",asMETHOD(ScriptArray,find_by_ref),asCALL_THISCALL);
method("bool contains(const T&in if_handle_then_const value) const",asMETHOD(ScriptArray,contains),asCALL_THISCALL);
method("void sort()",asMETHOD(ScriptArray,sort_ascending),asCALL_THISCALL);
method("void sort_descending()",asMETHOD(ScriptArray,sort_descending),asCALL_THISCALL);
check(engine->RegisterFuncdef("bool array<T>::less(const T&in if_handle_then_const a, const T&in if_handle_then_const b)"),"array<T>::less");
method("void sort(const less &in compare)",asMETHODPR(ScriptArray,sort,(asIScriptFunction*),void),asCALL_THISCALL);
method("void reverse()",asMETHOD(ScriptArray,reverse),asCALL_THISCALL);
method("array<T>@ slice(int start, int count = -1) const",asMETHOD(ScriptArray,slice),asCALL_THISCALL);
check(engine->RegisterDefaultArrayType("array<T>"),"default array type");
}
}
