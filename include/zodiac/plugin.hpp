/*
Header-only C++ convenience layer over the Zodiac plugin ABI (plugin.h). Everything here compiles down to the C table,
so plugins written with it keep the stable ABI.

zodiac::plugin::function<&fn>(host, "declaration") registers a plain C++ function in one line. The script declaration
must match fn's signature. Supported types:
- parameters: int32_t, int64_t, float, double, bool, std::string_view (valid for the duration of the call)
- return: void, int32_t, int64_t, float, double, bool, std::string, std::vector<std::string> (script array<string>@)
Any C++ exception escaping fn becomes a catchable script exception carrying e.what().

zodiac::plugin::VfsFile opens game files through the host's VFS (plugin.h, VFS rules): VfsFile::open(host, "sounds/a.ogg")
gives a VfsResult<VfsFile> (value and a ZODIAC_E_* code), and the file reads into a span, seeks and closes itself. It checks
the host's struct_size itself, so an older host gives ZODIAC_E_UNAVAILABLE instead of a call through a missing entry.
Open a VfsFile through the host the current call came from (the host given to the module's init, or the one a registered
function was bound to), and use it only there: the one shared implementation the next paragraph relies on makes a bound function
safe with any host, but not a file handle. Each host numbers its own handles, and one from another host may alias a handle of
this one (plugin.h). Each open and exists with a std::string_view copies the path into a 512-byte stack buffer to add its NUL
(one allocation only for a longer path), so it costs that much stack.

The wrapper reaches the host table through a per-function pointer set at registration. Registration runs on the main
thread before scripts execute, so calls only read it. Registering the same function with several hosts makes it use the
last one, which is safe because every Zodiac host in a process shares one implementation.
*/
#ifndef ZODIAC_PLUGIN_HPP
#define ZODIAC_PLUGIN_HPP

#include <zodiac/plugin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zodiac::plugin {
namespace detail {
template<class T> inline constexpr bool unsupported=false;
template<class T> struct argument{
static_assert(unsupported<T>,"zodiac::plugin::function: unsupported parameter type (use int32_t, int64_t, float, double, bool or std::string_view)");
};
template<> struct argument<int32_t>{
static int32_t read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept{return host->arg_int32(call,index);}
};
template<> struct argument<int64_t>{
static int64_t read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept{return host->arg_int64(call,index);}
};
template<> struct argument<float>{
static float read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept{return host->arg_float(call,index);}
};
template<> struct argument<double>{
static double read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept{return host->arg_double(call,index);}
};
template<> struct argument<bool>{
static bool read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept{return host->arg_bool(call,index)!=0;}
};
template<> struct argument<std::string_view>{
static std::string_view read(const zodiac_host* host,zodiac_call* call,uint32_t index) noexcept
{
size_t length=0;
const char* data=host->arg_string(call,index,&length);
return data?std::string_view(data,length):std::string_view();
}
};
template<class T> struct result{
static_assert(unsupported<T>,"zodiac::plugin::function: unsupported return type (use void, int32_t, int64_t, float, double, bool, std::string or std::vector<std::string>)");
};
template<> struct result<int32_t>{
static void write(const zodiac_host* host,zodiac_call* call,int32_t value){host->ret_int32(call,value);}
};
template<> struct result<int64_t>{
static void write(const zodiac_host* host,zodiac_call* call,int64_t value){host->ret_int64(call,value);}
};
template<> struct result<float>{
static void write(const zodiac_host* host,zodiac_call* call,float value){host->ret_float(call,value);}
};
template<> struct result<double>{
static void write(const zodiac_host* host,zodiac_call* call,double value){host->ret_double(call,value);}
};
template<> struct result<bool>{
static void write(const zodiac_host* host,zodiac_call* call,bool value){host->ret_bool(call,value?1:0);}
};
template<> struct result<std::string>{
static void write(const zodiac_host* host,zodiac_call* call,const std::string& value){host->ret_string(call,value.data(),value.size());}
};
template<> struct result<std::vector<std::string>>{
static void write(const zodiac_host* host,zodiac_call* call,const std::vector<std::string>& values)
{
zodiac_array* array=host->ret_array(call,"string",static_cast<uint32_t>(values.size()));
if(!array)return;
for(const std::string& value:values)host->array_push_string(array,value.data(),value.size());
}
};
template<class F> struct signature;
template<class R,class... A> struct signature<R(*)(A...)>{
using return_type=R;
using arguments=void(std::remove_cvref_t<A>...);
};
template<class R,class... A> struct signature<R(*)(A...) noexcept>:signature<R(*)(A...)>{};
template<auto Fn> inline const zodiac_host* bound_host=nullptr;
template<auto Fn,class R,class... A,size_t... I> inline void invoke(const zodiac_host* host,zodiac_call* call,void(*)(A...),std::index_sequence<I...>)
{
if constexpr(std::is_void_v<R>)Fn(argument<A>::read(host,call,static_cast<uint32_t>(I))...);
else result<std::remove_cvref_t<R>>::write(host,call,Fn(argument<A>::read(host,call,static_cast<uint32_t>(I))...));
}
template<class F> struct arity;
template<class... A> struct arity<void(A...)>:std::integral_constant<size_t,sizeof...(A)>{};
template<auto Fn> void wrapper(zodiac_call* call) noexcept
{
using traits=signature<decltype(Fn)>;
const zodiac_host* host=bound_host<Fn>;
try{
invoke<Fn,typename traits::return_type>(host,call,static_cast<typename traits::arguments*>(nullptr),std::make_index_sequence<arity<typename traits::arguments>::value>());
}
catch(const std::exception& error){
host->set_exception(call,error.what());
}
catch(...){
host->set_exception(call,"unknown C++ exception");
}
}
}
// Registers Fn under the script declaration. Returns ZODIAC_OK or a negative ZODIAC_E_* code.
template<auto Fn> int function(zodiac_host* host,const char* declaration)
{
static_assert(std::is_pointer_v<decltype(Fn)>&&std::is_function_v<std::remove_pointer_t<decltype(Fn)>>,"zodiac::plugin::function expects a pointer to a free function");
if(!host)return ZODIAC_E_INVALID_ARGUMENT;
detail::bound_host<Fn> = host;
return host->register_function(host,declaration,&detail::wrapper<Fn>,nullptr);
}

// The outcome of a VfsFile call: value when code is ZODIAC_OK, else code is a negative ZODIAC_E_* code (plugin.h, VFS rules)
// and value is T().
template<class T> struct VfsResult{
T value{};
int code=ZODIAC_OK;
bool ok() const noexcept{return code==ZODIAC_OK;}
explicit operator bool() const noexcept{return ok();}
};
namespace detail {
// A NUL-terminated copy of a path for the C entries: on the stack when it is short, otherwise one allocation. A path holding a
// NUL byte would name another file, so it is refused (ZODIAC_E_BAD_PATH); a failed allocation is ZODIAC_E_IO.
class vfs_path{
public:
explicit vfs_path(std::string_view path) noexcept
{
if(path.find('\0')!=std::string_view::npos){
code_=ZODIAC_E_BAD_PATH;
return;
}
char* to=local_;
if(path.size()>=sizeof local_){
heap_=new(std::nothrow) char[path.size()+1];
if(!heap_){
code_=ZODIAC_E_IO;
return;
}
to=heap_;
}
if(!path.empty())std::memcpy(to,path.data(),path.size());
to[path.size()]='\0';
text_=to;
}
~vfs_path()
{
delete[] heap_;
}
vfs_path(const vfs_path&)=delete;
vfs_path& operator=(const vfs_path&)=delete;
const char* c_str() const noexcept{return text_;}
int code() const noexcept{return code_;}
private:
char local_[512];
char* heap_=nullptr;
const char* text_=nullptr;
int code_=ZODIAC_OK;
};
}
// A game file opened through the host's VFS (plugin.h, VFS rules), closed when it is destroyed. Move-only; every call is noexcept
// and compiles down to the C entries. A host whose table lacks the VFS entries (ZODIAC_HOST_HAS) gives ZODIAC_E_UNAVAILABLE from
// open and exists, so a module using VfsFile still loads in an older host. One VfsFile belongs to one thread at a time, and to the
// host it was opened through: open it through the host the call came from (the header comment above). The std::string_view
// overloads of open and exists put a 512-byte buffer on the stack (vfs_path).
class VfsFile{
public:
VfsFile() noexcept=default;
VfsFile(VfsFile&& other) noexcept:host_(other.host_),file_(other.file_)
{
other.host_=nullptr;
other.file_=nullptr;
}
VfsFile& operator=(VfsFile&& other) noexcept
{
if(this!=&other){
close();
host_=other.host_;
file_=other.file_;
other.host_=nullptr;
other.file_=nullptr;
}
return *this;
}
VfsFile(const VfsFile&)=delete;
VfsFile& operator=(const VfsFile&)=delete;
~VfsFile()
{
close();
}
// True when host's table holds the VFS entries.
static bool available(const zodiac_host* host) noexcept
{
return host&&ZODIAC_HOST_HAS(host,vfs_exists);
}
static VfsResult<VfsFile> open(zodiac_host* host,const char* path) noexcept
{
VfsResult<VfsFile> result;
if(!available(host)){
result.code=host?ZODIAC_E_UNAVAILABLE:ZODIAC_E_INVALID_ARGUMENT;
return result;
}
zodiac_vfs_file* file=nullptr;
result.code=host->vfs_open(host,path,&file);
if(result.code==ZODIAC_OK){
result.value.host_=host;
result.value.file_=file;
}
return result;
}
static VfsResult<VfsFile> open(zodiac_host* host,std::string_view path) noexcept
{
const detail::vfs_path text(path);
if(text.code()!=ZODIAC_OK)return {VfsFile(),text.code()};
return open(host,text.c_str());
}
// 1 (true) when path names a file, false when it names nothing or a folder.
static VfsResult<bool> exists(zodiac_host* host,const char* path) noexcept
{
if(!available(host))return {false,host?ZODIAC_E_UNAVAILABLE:ZODIAC_E_INVALID_ARGUMENT};
const int found=host->vfs_exists(host,path);
if(found<0)return {false,found};
return {found==1,ZODIAC_OK};
}
static VfsResult<bool> exists(zodiac_host* host,std::string_view path) noexcept
{
const detail::vfs_path text(path);
if(text.code()!=ZODIAC_OK)return {false,text.code()};
return exists(host,text.c_str());
}
bool is_open() const noexcept{return file_!=nullptr;}
zodiac_vfs_file* handle() const noexcept{return file_;}
// Up to size bytes at the position, which moves on: the bytes read (fewer only at the end of the file, 0 there).
VfsResult<size_t> read(void* buffer,size_t size) noexcept
{
if(!file_)return {0,ZODIAC_E_INVALID_ARGUMENT};
const int64_t got=host_->vfs_read(host_,file_,buffer,size);
if(got<0)return {0,static_cast<int>(got)};
return {static_cast<size_t>(got),ZODIAC_OK};
}
VfsResult<size_t> read(std::span<std::byte> buffer) noexcept{return read(buffer.data(),buffer.size());}
VfsResult<size_t> read(std::span<char> buffer) noexcept{return read(buffer.data(),buffer.size());}
VfsResult<size_t> read(std::span<unsigned char> buffer) noexcept{return read(buffer.data(),buffer.size());}
// The new position; origin is ZODIAC_SEEK_SET, ZODIAC_SEEK_CUR or ZODIAC_SEEK_END, and the position must stay in 0..size.
VfsResult<uint64_t> seek(int64_t offset,int origin=ZODIAC_SEEK_SET) noexcept
{
if(!file_)return {0,ZODIAC_E_INVALID_ARGUMENT};
const int64_t at=host_->vfs_seek(host_,file_,offset,origin);
if(at<0)return {0,static_cast<int>(at)};
return {static_cast<uint64_t>(at),ZODIAC_OK};
}
VfsResult<uint64_t> position() noexcept{return seek(0,ZODIAC_SEEK_CUR);}
VfsResult<uint64_t> size() const noexcept
{
if(!file_)return {0,ZODIAC_E_INVALID_ARGUMENT};
const int64_t bytes=host_->vfs_size(host_,file_);
if(bytes<0)return {0,static_cast<int>(bytes)};
return {static_cast<uint64_t>(bytes),ZODIAC_OK};
}
void close() noexcept
{
if(file_)host_->vfs_close(host_,file_);
host_=nullptr;
file_=nullptr;
}
private:
zodiac_host* host_=nullptr;
zodiac_vfs_file* file_=nullptr;
};
}

#endif
