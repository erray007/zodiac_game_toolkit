// Fixture module for the plugin VFS tests (tests/test_plugin_vfs.cpp): it reads game files through the host's VFS entries
// (plugin.h, VFS rules) as a plugin such as an audio module does, on the script's thread and on threads of its own.
// - string vfs_read_all(const string &in path): the file's bytes, read in 64 KiB blocks (one pack chunk each). A failure raises
//   the script exception "<entry> failed: <code> after <bytes> bytes" ("vfs_open failed: -6 after 0 bytes").
// - int vfs_threads(const string &in path,int threads): reads path once here, then on threads threads at once, each through a
//   handle of its own: even threads forward in blocks of their own size, odd ones backwards with a seek before every block.
//   0 when every thread read exactly those bytes, else how many did not; the first read's negative code when it fails.
// - string vfs_read_hpp(const string &in path): the file's bytes through zodiac::plugin::VfsFile (plugin.hpp): opened by
//   string_view, moved, read backwards into spans with seeks, checked at the end and after close. A failure raises
//   "VfsFile::<call> failed: <code>".
// - int vfs_exists_hpp(const string &in path): VfsFile::exists: 1, 0 or the negative code.
// register_module fails with ZODIAC_E_UNAVAILABLE (logging why) in a host without the VFS entries, as plugin.h tells modules that
// need them to; vfs_read_all and vfs_threads also check struct_size at every call, raising "the host has no VFS entries".
// CMake sets ZODIAC_FIXTURE_NAME. ZODIAC_FIXTURE_OLD_ABI builds it as a module made before the VFS entries: its info has the
// ABI v1 size (64 bytes, without link_modes), and it registers only int vfs_reader_old(int), which never uses them.
#include <zodiac/plugin.h>
#include <zodiac/plugin.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
zodiac_host* g_host=nullptr;
#ifdef ZODIAC_FIXTURE_OLD_ABI
void vfs_reader_old(zodiac_call* call)
{
g_host->ret_int32(call,g_host->arg_int32(call,0)+1);
}
int register_module(zodiac_host* host)
{
g_host=host;
return host->register_function(host,"int vfs_reader_old(int)",&vfs_reader_old,nullptr);
}
const zodiac_module_info g_info={1u,static_cast<uint32_t>(offsetof(zodiac_module_info,link_modes)),ZODIAC_FIXTURE_NAME,"0.0.1","Zlib",nullptr,0,&register_module,nullptr,0};
#else
constexpr std::size_t kBlock=65536;
// Raises the script exception for a missing entry; true when the host has every VFS entry.
bool has_vfs(zodiac_call* call)
{
if(ZODIAC_HOST_HAS(g_host,vfs_exists))return true;
g_host->set_exception(call,"the host has no VFS entries");
return false;
}
struct Handle{
zodiac_vfs_file* file=nullptr;
Handle()=default;
Handle(const Handle&)=delete;
Handle& operator=(const Handle&)=delete;
~Handle()
{
g_host->vfs_close(g_host,file);
}
};
// What failed while reading a whole file: the entry, its code and the bytes delivered before it.
struct Failed{
const char* entry=nullptr;
int64_t code=0;
std::size_t delivered=0;
};
// Reads all of path into out in blocks of block bytes: forward, or backwards with a seek before each block.
Failed read_whole(const std::string& path,std::size_t block,bool backwards,std::string& out)
{
Handle handle;
if(const int opened=g_host->vfs_open(g_host,path.c_str(),&handle.file);opened!=ZODIAC_OK)return {"vfs_open",opened,0};
const int64_t size=g_host->vfs_size(g_host,handle.file);
if(size<0)return {"vfs_size",size,0};
out.assign(static_cast<std::size_t>(size),'\0');
std::size_t done=0;
if(!backwards){
for(;;){
const std::size_t room=out.size()-done;
const int64_t got=g_host->vfs_read(g_host,handle.file,out.data()+done,room<block?room:block);
if(got<0)return {"vfs_read",got,done};
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
// Past the end there is nothing more.
char extra[16];
if(done!=out.size()||g_host->vfs_read(g_host,handle.file,extra,sizeof extra)!=0)return {"vfs_read",ZODIAC_E_IO,done};
return {};
}
std::size_t end=out.size();
while(end>0){
const std::size_t start=end>block?end-block:0;
if(const int64_t at=g_host->vfs_seek(g_host,handle.file,static_cast<int64_t>(start),ZODIAC_SEEK_SET);at!=static_cast<int64_t>(start))return {"vfs_seek",at<0?at:ZODIAC_E_IO,done};
const int64_t got=g_host->vfs_read(g_host,handle.file,out.data()+start,end-start);
if(got<0)return {"vfs_read",got,done};
if(got!=static_cast<int64_t>(end-start))return {"vfs_read",ZODIAC_E_IO,done};
done+=end-start;
end=start;
}
return {};
}
void vfs_read_all(zodiac_call* call)
{
if(!has_vfs(call))return;
try{
size_t length=0;
const char* data=g_host->arg_string(call,0,&length);
const std::string path(data?data:"",length);
std::string bytes;
const Failed failed=read_whole(path,kBlock,false,bytes);
if(failed.entry){
const std::string message=std::string(failed.entry)+" failed: "+std::to_string(failed.code)+" after "+std::to_string(failed.delivered)+" bytes";
g_host->set_exception(call,message.c_str());
return;
}
g_host->ret_string(call,bytes.data(),bytes.size());
}
catch(...){
g_host->set_exception(call,"vfs_read_all: out of memory");
}
}
// Joins every thread it holds, also when making one failed.
struct Workers{
std::vector<std::thread> threads;
~Workers()
{
for(std::thread& thread:threads)if(thread.joinable())thread.join();
}
};
void vfs_threads(zodiac_call* call)
{
if(!has_vfs(call))return;
try{
size_t length=0;
const char* data=g_host->arg_string(call,0,&length);
const std::string path(data?data:"",length);
const int32_t count=g_host->arg_int32(call,1);
std::string expected;
if(const Failed failed=read_whole(path,kBlock,false,expected);failed.entry){
g_host->ret_int32(call,static_cast<int32_t>(failed.code));
return;
}
std::atomic<int32_t> differing{0};
{
Workers workers;
workers.threads.reserve(count>0?static_cast<std::size_t>(count):0);
for(int32_t i=0;i<count;++i){
workers.threads.emplace_back([&path,&expected,&differing,i]{
try{
std::string got;
// Block sizes that cross the 64 KiB chunks at different places in every thread.
const std::size_t block=4093*static_cast<std::size_t>(i+1)+static_cast<std::size_t>(i%3);
const bool backwards=i%2==1;
if(g_host->vfs_exists(g_host,path.c_str())!=1||read_whole(path,block,backwards,got).entry||got!=expected)differing.fetch_add(1);
}
catch(...){
differing.fetch_add(1);
}
});
}
}
g_host->ret_int32(call,differing.load());
}
catch(...){
g_host->set_exception(call,"vfs_threads: cannot start the threads");
}
}
void fail(zodiac_call* call,const char* entry,int code)
{
const std::string message=std::string(entry)+" failed: "+std::to_string(code);
g_host->set_exception(call,message.c_str());
}
std::string_view path_argument(zodiac_call* call)
{
size_t length=0;
const char* data=g_host->arg_string(call,0,&length);
return data?std::string_view(data,length):std::string_view();
}
void vfs_read_hpp(zodiac_call* call)
{
using zodiac::plugin::VfsFile;
try{
auto opened=VfsFile::open(g_host,path_argument(call));
if(!opened){
fail(call,"VfsFile::open",opened.code);
return;
}
VfsFile file=std::move(opened.value);
if(opened.value.is_open()||!file.is_open()){
g_host->set_exception(call,"a moved VfsFile did not move");
return;
}
const auto size=file.size();
if(!size){
fail(call,"VfsFile::size",size.code);
return;
}
std::string bytes(static_cast<std::size_t>(size.value),'\0');
// Backwards in spans of 10000 bytes, with a seek before each.
std::size_t end=bytes.size();
while(end>0){
const std::size_t start=end>10000?end-10000:0;
const auto at=file.seek(static_cast<int64_t>(start));
if(!at||at.value!=start){
fail(call,"VfsFile::seek",at?ZODIAC_E_IO:at.code);
return;
}
const auto got=file.read(std::span<char>(bytes.data()+start,end-start));
if(!got||got.value!=end-start){
fail(call,"VfsFile::read",got?ZODIAC_E_IO:got.code);
return;
}
end=start;
}
std::byte extra[4];
const auto tail=file.seek(0,ZODIAC_SEEK_END);
const auto none=file.read(std::span<std::byte>(extra));
if(!tail||tail.value!=bytes.size()||!none||none.value!=0||file.position().value!=bytes.size()){
g_host->set_exception(call,"VfsFile read past the end");
return;
}
file.close();
if(file.is_open()||file.size().code!=ZODIAC_E_INVALID_ARGUMENT||file.read(extra,sizeof extra).code!=ZODIAC_E_INVALID_ARGUMENT){
g_host->set_exception(call,"a closed VfsFile still answers");
return;
}
g_host->ret_string(call,bytes.data(),bytes.size());
}
catch(...){
g_host->set_exception(call,"vfs_read_hpp: out of memory");
}
}
void vfs_exists_hpp(zodiac_call* call)
{
const auto found=zodiac::plugin::VfsFile::exists(g_host,path_argument(call));
g_host->ret_int32(call,found?(found.value?1:0):found.code);
}
int register_module(zodiac_host* host)
{
g_host=host;
// It cannot work without the VFS: refuse to register in a host whose table lacks the entries (plugin.h, VFS rules).
if(!ZODIAC_HOST_HAS(host,vfs_exists)){
host->log(host,ZODIAC_LOG_ERROR,ZODIAC_FIXTURE_NAME " needs a Zodiac whose plugin table has the VFS entries");
return ZODIAC_E_UNAVAILABLE;
}
if(const int result=host->register_function(host,"string vfs_read_all(const string &in path)",&vfs_read_all,nullptr);result!=ZODIAC_OK)return result;
if(const int result=host->register_function(host,"int vfs_threads(const string &in path,int threads)",&vfs_threads,nullptr);result!=ZODIAC_OK)return result;
if(const int result=host->register_function(host,"string vfs_read_hpp(const string &in path)",&vfs_read_hpp,nullptr);result!=ZODIAC_OK)return result;
return host->register_function(host,"int vfs_exists_hpp(const string &in path)",&vfs_exists_hpp,nullptr);
}
const zodiac_module_info g_info={ZODIAC_ABI_VERSION,sizeof(zodiac_module_info),ZODIAC_FIXTURE_NAME,"0.0.1","Zlib",nullptr,0,&register_module,nullptr,0};
#endif
}

ZODIAC_DEFINE_MODULE(g_info)
