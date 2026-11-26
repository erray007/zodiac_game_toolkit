// The platform file layer under the VFS (platform/file.hpp): replace_file over an existing and an open target, long Windows paths
// (\\?\), positional reads from several threads on one File, and the per-user data folder's fallbacks.
#include <core/path_utf8.hpp>
#include <core/vfs/folder_source.hpp>
#include <platform/file.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace {
namespace fs=std::filesystem;
using zodiac::platform::File;
using zodiac::platform::FileAccess;
// A fresh folder under the system temp folder, removed (long paths included) on destruction.
class TempDir{
public:
TempDir()
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/zodiac::path_from_utf8("zodiac platform file \xC5\x9F "+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
fs::create_directories(dir_);
}
~TempDir(){zodiac::vfs::detail::remove_tree(dir_);}
TempDir(const TempDir&)=delete;
TempDir& operator=(const TempDir&)=delete;
const fs::path& dir() const{return dir_;}
private:
fs::path dir_;
};
void write_disk(const fs::path& path,std::string_view text)
{
std::ofstream out(path,std::ios::binary);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
}
std::string read_disk(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
std::string read_all(const File& file)
{
std::string text(static_cast<std::size_t>(file.size()),'\0');
const std::int64_t got=file.read_at(0,std::as_writable_bytes(std::span(text)));
REQUIRE(got>=0);
text.resize(static_cast<std::size_t>(got));
return text;
}
// Sets (or, with nullopt, removes) an environment variable for the test's duration, restoring the old value.
class ScopedEnv{
public:
ScopedEnv(const char* name,const std::optional<fs::path>& value):name_(name)
{
#ifdef _WIN32
// The wide value: the narrow one is in the ANSI code page, which cannot hold every folder name.
const std::wstring wide(name_.begin(),name_.end());
if(const wchar_t* old=_wgetenv(wide.c_str()))old_=fs::path(old);
#else
if(const char* old=std::getenv(name))old_=fs::path(old);
#endif
set(value);
}
~ScopedEnv(){set(old_);}
ScopedEnv(const ScopedEnv&)=delete;
ScopedEnv& operator=(const ScopedEnv&)=delete;
private:
void set(const std::optional<fs::path>& value)
{
#ifdef _WIN32
const std::wstring wide(name_.begin(),name_.end());
_wputenv_s(wide.c_str(),value?value->c_str():L"");
#else
if(value)setenv(name_.c_str(),value->c_str(),1);
else unsetenv(name_.c_str());
#endif
}
std::string name_;
std::optional<fs::path> old_;
};
}

TEST_CASE("replace_file replaces an existing target at once")
{
const TempDir temp;
const fs::path from=temp.dir()/"new.bin";
const fs::path to=temp.dir()/"pack.bin";
write_disk(from,"new bytes");
write_disk(to,"old bytes, longer");
std::error_code error;
REQUIRE(zodiac::platform::replace_file(from,to,error));
CHECK_FALSE(error);
CHECK(read_disk(to)=="new bytes");
CHECK_FALSE(fs::exists(from));
// A missing source fails and says why.
CHECK_FALSE(zodiac::platform::replace_file(from,to,error));
CHECK(zodiac::platform::error_kind(error)==zodiac::platform::ErrorKind::not_found);
}

TEST_CASE("rename_no_replace moves to a free name and never over an existing one")
{
const TempDir temp;
const fs::path from=temp.dir()/"pack.key.tmp";
const fs::path to=temp.dir()/"pack.key";
write_disk(from,"new key");
write_disk(to,"the game's key");
std::error_code error;
CHECK_FALSE(zodiac::platform::rename_no_replace(from,to,error));
CHECK(zodiac::platform::error_kind(error)==zodiac::platform::ErrorKind::exists);
CHECK(read_disk(to)=="the game's key");
CHECK(read_disk(from)=="new key");
fs::remove(to);
REQUIRE(zodiac::platform::rename_no_replace(from,to,error));
CHECK_FALSE(error);
CHECK(read_disk(to)=="new key");
CHECK_FALSE(fs::exists(from));
CHECK_FALSE(zodiac::platform::rename_no_replace(from,to,error));
CHECK(zodiac::platform::error_kind(error)==zodiac::platform::ErrorKind::not_found);
}

TEST_CASE("create_secret makes a new file only its owner, SYSTEM and Administrators can open")
{
const TempDir temp;
const fs::path path=temp.dir()/"secret.key";
{
auto file=File::open(path,FileAccess::create_secret);
REQUIRE(file);
REQUIRE(file->write_at(0,std::as_bytes(std::span(std::string_view("key")))));
}
CHECK(read_disk(path)=="key");
// Like create_new, never over an existing file.
CHECK_FALSE(File::open(path,FileAccess::create_secret));
#ifdef _WIN32
PSECURITY_DESCRIPTOR descriptor=nullptr;
PACL dacl=nullptr;
REQUIRE(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor)==ERROR_SUCCESS);
SECURITY_DESCRIPTOR_CONTROL control=0;
DWORD revision=0;
REQUIRE(GetSecurityDescriptorControl(descriptor,&control,&revision));
// Protected: nothing inherited from the folder (whose entries grant Users or Authenticated Users access).
CHECK((control&SE_DACL_PROTECTED)!=0);
REQUIRE(dacl!=nullptr);
const auto sid_of=[](WELL_KNOWN_SID_TYPE type){
std::vector<BYTE> sid(SECURITY_MAX_SID_SIZE);
DWORD size=static_cast<DWORD>(sid.size());
REQUIRE(CreateWellKnownSid(type,nullptr,sid.data(),&size));
return sid;
};
const std::vector<BYTE> users=sid_of(WinBuiltinUsersSid);
const std::vector<BYTE> authenticated=sid_of(WinAuthenticatedUserSid);
const std::vector<BYTE> everyone=sid_of(WinWorldSid);
CHECK(dacl->AceCount==3);
for(DWORD index=0;index<dacl->AceCount;++index){
void* ace=nullptr;
REQUIRE(GetAce(dacl,index,&ace));
const auto* header=static_cast<const ACE_HEADER*>(ace);
CHECK(header->AceType==ACCESS_ALLOWED_ACE_TYPE);
PSID sid=&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
CHECK_FALSE(EqualSid(sid,const_cast<BYTE*>(users.data())));
CHECK_FALSE(EqualSid(sid,const_cast<BYTE*>(authenticated.data())));
CHECK_FALSE(EqualSid(sid,const_cast<BYTE*>(everyone.data())));
}
LocalFree(descriptor);
// A hard link is the same file, so it has the same descriptor.
const fs::path link=temp.dir()/"link.key";
fs::create_hard_link(path,link);
PSECURITY_DESCRIPTOR linked=nullptr;
PACL linked_dacl=nullptr;
REQUIRE(GetNamedSecurityInfoW(link.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&linked_dacl,nullptr,&linked)==ERROR_SUCCESS);
CHECK(linked_dacl->AceCount==3);
LocalFree(linked);
#else
struct stat info{};
REQUIRE(::stat(path.c_str(),&info)==0);
CHECK((info.st_mode&0777)==0600);
#endif
}

TEST_CASE("replace_file works while the target is open for reading, which keeps the old bytes")
{
// Task 8's compact() replaces a pack that file objects still read.
const TempDir temp;
const fs::path from=temp.dir()/"compacted.bin";
const fs::path to=temp.dir()/"game.zpk";
write_disk(to,"the old pack");
write_disk(from,"the new pack, compacted");
auto reader=File::open(to,FileAccess::read);
REQUIRE(reader);
std::error_code error;
REQUIRE(zodiac::platform::replace_file(from,to,error));
CHECK_FALSE(error);
CHECK(read_all(*reader)=="the old pack");
auto fresh=File::open(to,FileAccess::read);
REQUIRE(fresh);
CHECK(read_all(*fresh)=="the new pack, compacted");
reader->close();
CHECK(read_disk(to)=="the new pack, compacted");
}

TEST_CASE("paths of 248 characters and more work in every call")
{
const TempDir temp;
fs::path deep=temp.dir();
const std::string segment(60,'d');
for(int i=0;i<5;++i){
deep/=zodiac::path_from_utf8(segment+std::to_string(i));
const std::error_code made=zodiac::platform::make_directory(deep);
REQUIRE_FALSE(made);
}
REQUIRE(deep.native().size()>=300);
const fs::path file=deep/"long.bin";
{
auto out=File::open(file,FileAccess::create_truncate);
REQUIRE(out);
const std::string_view text="far away";
REQUIRE(out->write_at(0,std::as_bytes(std::span(text))));
REQUIRE(out->sync());
}
const auto node=zodiac::platform::stat_path(file);
REQUIRE(node);
CHECK(node->kind==zodiac::platform::NodeKind::file);
CHECK(node->size==8);
auto in=File::open(file,FileAccess::read);
REQUIRE(in);
CHECK(read_all(*in)=="far away");
in->close();
std::vector<zodiac::platform::DirectoryEntry> entries;
REQUIRE_FALSE(zodiac::platform::list_directory(deep,entries));
REQUIRE(entries.size()==1);
CHECK(entries[0].name=="long.bin");
const fs::path moved=deep/"moved.bin";
std::error_code error;
REQUIRE(zodiac::platform::replace_file(file,moved,error));
REQUIRE_FALSE(zodiac::platform::remove_file(moved));
const fs::path top=temp.dir()/zodiac::path_from_utf8(segment+"0");
REQUIRE_FALSE(zodiac::vfs::detail::remove_tree(top));
const auto gone=zodiac::platform::stat_path(top);
REQUIRE_FALSE(gone);
CHECK(zodiac::platform::error_kind(gone.error())==zodiac::platform::ErrorKind::not_found);
}

TEST_CASE("read_at is safe from several threads on one File")
{
const TempDir temp;
const fs::path path=temp.dir()/"pattern.bin";
constexpr std::size_t size=1<<20;
std::string bytes(size,'\0');
for(std::size_t i=0;i<size;++i)bytes[i]=static_cast<char>((i*7+i/251)&0xFF);
write_disk(path,bytes);
const auto file=File::open(path,FileAccess::read);
REQUIRE(file);
std::atomic<int> failures{0};
std::vector<std::thread> threads;
for(int t=0;t<8;++t){
threads.emplace_back([&,t]{
std::string buffer(4096,'\0');
std::uint64_t state=0x9E3779B97F4A7C15u*static_cast<std::uint64_t>(t+1);
for(int i=0;i<2000;++i){
state^=state<<13;
state^=state>>7;
state^=state<<17;
const std::size_t offset=static_cast<std::size_t>(state%(size-100));
const std::int64_t got=file->read_at(offset,std::as_writable_bytes(std::span(buffer)));
const std::size_t expected=std::min<std::size_t>(buffer.size(),size-offset);
if(got!=static_cast<std::int64_t>(expected)||std::string_view(buffer).substr(0,expected)!=std::string_view(bytes).substr(offset,expected))failures.fetch_add(1);
}
});
}
for(std::thread& thread:threads)thread.join();
CHECK(failures.load()==0);
// At and past the end: 0, never an error.
std::byte one[1];
CHECK(file->read_at(size,one)==0);
CHECK(file->read_at(size+12345,one)==0);
}

TEST_CASE("user_data_folder falls back when the variable is unset or relative")
{
const TempDir temp;
#ifdef _WIN32
const char* name="APPDATA";
#else
const char* name="XDG_DATA_HOME";
#endif
{
const ScopedEnv set(name,temp.dir());
CHECK(zodiac::platform::user_data_folder()==temp.dir().lexically_normal());
}
#ifdef _WIN32
// The roaming application data folder, from the shell, without the variable.
for(const std::optional<fs::path>& value:{std::optional<fs::path>(),std::optional<fs::path>(fs::path(L"relative\\folder"))}){
const ScopedEnv unset(name,value);
const fs::path folder=zodiac::platform::user_data_folder();
REQUIRE_FALSE(folder.empty());
CHECK(folder.is_absolute());
std::error_code ec;
CHECK(fs::is_directory(folder,ec));
}
#else
const ScopedEnv home("HOME",temp.dir());
for(const std::optional<fs::path>& value:{std::optional<fs::path>(),std::optional<fs::path>(fs::path("relative/folder"))}){
const ScopedEnv unset(name,value);
CHECK(zodiac::platform::user_data_folder()==(temp.dir()/".local"/"share").lexically_normal());
}
{
const ScopedEnv unset(name,std::nullopt);
const ScopedEnv no_home("HOME",std::nullopt);
CHECK(zodiac::platform::user_data_folder().empty());
}
#endif
}
