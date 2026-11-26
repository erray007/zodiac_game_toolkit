// Packs mounted into the VFS (src/core/pack/pack_source.hpp, the mount rules of src/core/vfs/vfs.hpp, mount_pack and unmount_pack of
// src/core/vfs/files.hpp; spec 8.3 and 9 of the library-and-packs design):
// - newest-first lookup in front of the folder, mount points below a mount's root, case-insensitive names and "..";
// - write routing: a read-only pack's exact text, a writable pack taking new and existing names, an append seeded from the folder
//   below it, and paths no pack covers landing in the folder;
// - deletion markers hiding older packs and the folder, and a newer pack showing the name again; listings merged by fold;
// - unmount while a read is open, concurrent reads through a mount, and Review Focus 4: a pack changed while it is mounted and
//   read, a second pack object on one file, and compact under an open file object.
// - the 2 GiB limit of a file written through a pack mount, also for an append seeded from an older mount's (sparse) file;
// - lookups that allocate nothing: usual and Turkish names on the stack fold, a name past it with one allocation.
// Packs are made with 64 KiB, 1 pass Argon2id or raw keys for speed.
#include "script_fixture.hpp"
#include "sparse_file.hpp"
#include <core/pack/pack_format.hpp>
#include <core/pack/pack_source.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/vfs.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// Every allocation of this executable's code through the replaceable operator new, counted per thread, so a test can show
// that a lookup allocates nothing.
namespace {
thread_local std::uint64_t allocations=0;
// The largest single request since a test last reset it, so a test can show that no buffer of a file's size was made.
thread_local std::size_t largest_allocation=0;
}
void* operator new(std::size_t size)
{
++allocations;
if(size>largest_allocation)largest_allocation=size;
if(void* memory=std::malloc(size?size:1))return memory;
throw std::bad_alloc();
}
void* operator new[](std::size_t size)
{
return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
std::free(memory);
}
void operator delete[](void* memory) noexcept
{
std::free(memory);
}
void operator delete(void* memory,std::size_t) noexcept
{
std::free(memory);
}
void operator delete[](void* memory,std::size_t) noexcept
{
std::free(memory);
}

namespace pack=zodiac::pack;
namespace crypto=zodiac::crypto;
namespace fs=std::filesystem;
using zodiac::vfs::EntryKind;
using zodiac::vfs::FileError;
using zodiac::vfs::FolderSource;
using zodiac::vfs::ListKind;
using zodiac::vfs::Vfs;
using zodiac::vfs::WriteMode;
using zodiac_test::ScriptFixture;

namespace {
const pack::PackOptions kFast{64,1,16};

struct TempDir{
fs::path path;
TempDir():path(zodiac_test::unique_temp_folder())
{
fs::create_directories(path);
}
~TempDir()
{
std::error_code error;
fs::remove_all(path,error);
}
TempDir(const TempDir&)=delete;
TempDir& operator=(const TempDir&)=delete;
};

crypto::Key test_key(std::uint8_t seed)
{
crypto::Key key;
for(std::size_t i=0;i<key.size();++i)key[i]=static_cast<std::uint8_t>(seed+i*7);
return key;
}
std::span<const std::byte> span_of(std::string_view text)
{
return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
// size pseudo-random bytes of a seed; printable ones (for a script string compared as text) with text.
std::string data_of(std::size_t size,std::uint64_t seed,bool text=false)
{
std::mt19937_64 random(seed);
std::string bytes(size,'\0');
for(char& c:bytes)c=text?static_cast<char>('a'+random()%26):static_cast<char>(random()>>56);
return bytes;
}
std::string file_bytes(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
REQUIRE(in.good());
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void put_bytes(const fs::path& path,std::string_view bytes)
{
fs::create_directories(path.parent_path());
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
REQUIRE(out.good());
}
std::shared_ptr<pack::PackStorage> make(const fs::path& path,const pack::KeySource& key)
{
fs::create_directories(path.parent_path());
auto storage=pack::PackStorage::open(path,key,true,kFast);
if(!storage)INFO(storage.error().failure.message);
REQUIRE(storage);
return std::move(*storage);
}
std::shared_ptr<pack::PackStorage> reopen(const fs::path& path,const pack::KeySource& key)
{
auto storage=pack::PackStorage::open(path,key,false);
REQUIRE(storage);
return std::move(*storage);
}
void add(pack::PackStorage& storage,std::string_view name,std::string_view bytes)
{
REQUIRE(storage.add(name,span_of(bytes),0));
}
// A pack at path holding the given entries, flushed.
std::shared_ptr<pack::PackStorage> pack_of(const fs::path& path,std::uint8_t seed,const std::vector<std::pair<std::string,std::string>>& entries)
{
auto storage=make(path,test_key(seed));
for(const auto& [name,bytes]:entries)add(*storage,name,bytes);
REQUIRE(storage->flush());
return storage;
}
// Every byte of reader, or the FileError of the first failed read.
std::expected<std::string,FileError> read_all(zodiac::vfs::Reader& reader)
{
std::string bytes(static_cast<std::size_t>(reader.size()),'\0');
std::size_t done=0;
while(done<bytes.size()){
const std::int64_t got=reader.read_at(done,{reinterpret_cast<std::byte*>(bytes.data()+done),bytes.size()-done});
if(got<0)return std::unexpected(zodiac::vfs::read_error(got));
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
bytes.resize(done);
return bytes;
}
// The bytes of path through vfs, or the FileError of the open or a read.
std::expected<std::string,FileError> vfs_read(Vfs& vfs,std::string_view path)
{
const auto reader=vfs.open_read(path);
if(!reader)return std::unexpected(reader.error().code);
return read_all(**reader);
}
std::string read_ok(Vfs& vfs,std::string_view path)
{
INFO(path);
const auto bytes=vfs_read(vfs,path);
REQUIRE(bytes);
return *bytes;
}
std::string joined(const std::vector<std::string>& names)
{
std::string out;
for(const std::string& name:names){
if(!out.empty())out.push_back(',');
out.append(name);
}
return out;
}
std::string listed(Vfs& vfs,std::string_view path,ListKind kind=ListKind::files)
{
const auto names=vfs.list(path,kind);
REQUIRE(names);
return joined(*names);
}
// A Vfs with folder root (writable) at "/" and folder data (writable) at "data:/".
struct Mounted{
TempDir dir;
Vfs vfs;
Mounted()
{
REQUIRE(vfs.mount("/",std::make_shared<FolderSource>(root(),false,true),true,nullptr));
REQUIRE(vfs.mount("data:/",std::make_shared<FolderSource>(data(),false,true),true,nullptr));
}
fs::path root() const{return dir.path/"root";}
fs::path data() const{return dir.path/"data";}
fs::path packs() const{return dir.path/"packs";}
};
// Owners for mounts made from C++.
int owner_a=0;
int owner_b=0;
int owner_c=0;

std::string text_of(ScriptFixture& fixture,std::string_view body)
{
const std::string source="string main(){"+std::string(body)+"}";
INFO(source);
const int result=fixture.run(source.c_str(),"string main()");
INFO(fixture.exception_string());
REQUIRE(result==asEXECUTION_FINISHED);
return fixture.returned_string();
}
// A module whose global variables live across several calls.
struct Steps{
ScriptFixture& fixture;
asIScriptModule* module=nullptr;
Steps(ScriptFixture& owner,const std::string& source):fixture(owner)
{
module=fixture.engine->GetModule("steps",asGM_ALWAYS_CREATE);
REQUIRE(module!=nullptr);
REQUIRE(module->AddScriptSection("steps",source.c_str())>=0);
INFO(fixture.messages);
REQUIRE(module->Build()>=0);
}
~Steps()
{
module->Discard();
}
Steps(const Steps&)=delete;
Steps& operator=(const Steps&)=delete;
std::string call(const char* declaration)
{
asIScriptFunction* function=module->GetFunctionByDecl(declaration);
REQUIRE(function!=nullptr);
asIScriptContext* context=fixture.engine->CreateContext();
REQUIRE(context!=nullptr);
REQUIRE(context->Prepare(function)>=0);
const int result=context->Execute();
const char* exception=context->GetExceptionString();
INFO(declaration<<": "<<(exception?exception:""));
std::string text;
if(result==asEXECUTION_FINISHED)text=static_cast<const zodiac::String*>(context->GetAddressOfReturnValue())->bytes();
context->Release();
REQUIRE(result==asEXECUTION_FINISHED);
return text;
}
};
}

TEST_CASE("packs at one point are searched newest first, all in front of the folder, and listings merge by fold")
{
Mounted m;
put_bytes(m.root()/"a.txt","folder");
put_bytes(m.root()/"b.txt","folder b");
put_bytes(m.root()/"only.txt","folder only");
auto first=pack_of(m.packs()/"1.zpk",1,{{"a.txt","one"},{"b.txt","one b"},{"one.txt","1"}});
auto second=pack_of(m.packs()/"2.zpk",2,{{"a.txt","two"},{"B.TXT","two b"},{"sub/c.txt","c"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(first,false),false,&owner_a));
CHECK(read_ok(m.vfs,"a.txt")=="one");
REQUIRE(m.vfs.mount("/",pack::make_pack_source(second,false),false,&owner_b));
CHECK(read_ok(m.vfs,"a.txt")=="two");
CHECK(read_ok(m.vfs,"A.TXT")=="two");
CHECK(read_ok(m.vfs,"b.txt")=="two b");
CHECK(read_ok(m.vfs,"one.txt")=="1");
CHECK(read_ok(m.vfs,"only.txt")=="folder only");
CHECK(read_ok(m.vfs,"SUB/C.txt")=="c");
const auto info=m.vfs.stat("a.txt");
REQUIRE(info);
CHECK(info->kind==EntryKind::file);
CHECK(info->size==3);
const auto folder=m.vfs.stat("Sub");
REQUIRE(folder);
CHECK(folder->kind==EntryKind::directory);
CHECK(m.vfs.open_read("sub").error().code==FileError::is_directory);
// One name per fold across the sources, spelled as the newest has it.
CHECK(listed(m.vfs,"/")=="a.txt,B.TXT,one.txt,only.txt");
CHECK(listed(m.vfs,"/",ListKind::directories)=="sub");
CHECK(listed(m.vfs,"sub")=="c.txt");
CHECK(m.vfs.unmount(&owner_b)==1);
CHECK(read_ok(m.vfs,"a.txt")=="one");
CHECK(listed(m.vfs,"/")=="a.txt,b.txt,one.txt,only.txt");
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(read_ok(m.vfs,"a.txt")=="folder");
CHECK(m.vfs.open_read("one.txt").error().code==FileError::not_found);
}

TEST_CASE("a pack mounted at data:/maps serves its names below the mount point, case-insensitively, and .. leaves it")
{
Mounted m;
put_bytes(m.data()/"x","folder x");
auto maps=pack_of(m.packs()/"maps.zpk",3,{{"x","pack x"},{"deep/y","pack y"}});
REQUIRE(m.vfs.mount("data:/maps",pack::make_pack_source(maps,false),false,&owner_a));
CHECK(read_ok(m.vfs,"data:/maps/x")=="pack x");
CHECK(read_ok(m.vfs,"data:/MAPS/X")=="pack x");
CHECK(read_ok(m.vfs,"DATA:/Maps/Deep/Y")=="pack y");
CHECK(read_ok(m.vfs,"data:/maps/../x")=="folder x");
CHECK(read_ok(m.vfs,"data:/x")=="folder x");
CHECK(m.vfs.stat("data:/maps")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"data:/",ListKind::directories)=="maps");
CHECK(listed(m.vfs,"data:/maps")=="x");
CHECK(listed(m.vfs,"data:/maps",ListKind::directories)=="deep");
CHECK(m.vfs.open_read("maps/x").error().code==FileError::not_found);
}

TEST_CASE("a write to a name only a read-only pack has fails with the exact text, and other paths land in the folder")
{
Mounted m;
auto shipped=pack_of(m.packs()/"ro.zpk",4,{{"r.txt","shipped"},{"defaults/settings.ini","x=1"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(shipped,false),false,&owner_a));
const std::string text="\"R.txt\" is in a read-only pack; save files in data:/ instead";
const auto opened=m.vfs.open_write("R.txt",WriteMode::truncate);
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::read_only_pack);
CHECK(opened.error().message==text);
const auto written=m.vfs.write_file("R.txt",span_of("mine"));
REQUIRE_FALSE(written);
CHECK(zodiac::vfs::write_failure_text("R.txt",written.error())==text);
CHECK(m.vfs.remove("r.txt").error().code==FileError::read_only_pack);
CHECK(m.vfs.write_file("defaults/settings.ini",span_of("x=2")).error().message=="\"defaults/settings.ini\" is in a read-only pack; save files in data:/ instead");
CHECK(read_ok(m.vfs,"r.txt")=="shipped");
REQUIRE(m.vfs.write_file("u.txt",span_of("unrelated")));
CHECK(file_bytes(m.root()/"u.txt")=="unrelated");
REQUIRE(m.vfs.write_file("data:/save.txt",span_of("saved")));
CHECK(file_bytes(m.data()/"save.txt")=="saved");
}

TEST_CASE("a writable pack mount receives new and existing names, which persist after flush and reopen")
{
Mounted m;
put_bytes(m.root()/"log.txt","ab");
put_bytes(m.root()/"under.txt","folder");
const fs::path file=m.packs()/"rw.zpk";
{
auto storage=pack_of(file,5,{{"old.txt","old bytes"},{"keep.txt","kept"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
}
REQUIRE(m.vfs.write_file("new.txt",span_of("brand new")));
REQUIRE(m.vfs.write_file("Old.txt",span_of("replaced")));
{
auto target=m.vfs.open_write("dir/stream.bin",WriteMode::truncate);
REQUIRE(target);
REQUIRE((*target)->write_at(0,span_of("0123456789")));
REQUIRE((*target)->write_at(20,span_of("tail")));
CHECK((*target)->size()==24);
REQUIRE((*target)->truncate(22));
// Nothing is stored before commit.
CHECK(m.vfs.open_read("dir/stream.bin").error().code==FileError::not_found);
REQUIRE((*target)->commit());
}
{
// "a" on a name only the folder below has starts from the folder's bytes; the folder keeps its file.
auto target=m.vfs.open_write("log.txt",WriteMode::append);
REQUIRE(target);
CHECK((*target)->size()==2);
REQUIRE((*target)->write_at(2,span_of("cd")));
REQUIRE((*target)->commit());
REQUIRE((*target)->sync());
}
{
auto target=m.vfs.open_write("keep.txt",WriteMode::update);
REQUIRE(target);
REQUIRE((*target)->write_at(0,span_of("K")));
REQUIRE((*target)->commit());
}
{
// "r+" on a name only the folder below has starts from the folder's bytes too.
auto target=m.vfs.open_write("Under.txt",WriteMode::update);
REQUIRE(target);
CHECK((*target)->size()==6);
REQUIRE((*target)->write_at(0,span_of("F")));
REQUIRE((*target)->commit());
}
CHECK(m.vfs.open_write("none.txt",WriteMode::update).error().code==FileError::not_found);
CHECK(read_ok(m.vfs,"new.txt")=="brand new");
CHECK(read_ok(m.vfs,"old.txt")=="replaced");
CHECK(read_ok(m.vfs,"log.txt")=="abcd");
CHECK(read_ok(m.vfs,"keep.txt")=="Kept");
CHECK(read_ok(m.vfs,"under.txt")=="Folder");
CHECK(file_bytes(m.root()/"under.txt")=="folder");
CHECK(file_bytes(m.root()/"log.txt")=="ab");
CHECK_FALSE(fs::exists(m.root()/"new.txt"));
// A rename inside the pack moves the entry; a recursive delete of a pack folder removes its files.
REQUIRE(m.vfs.move("new.txt","moved/new.txt"));
CHECK(m.vfs.open_read("new.txt").error().code==FileError::not_found);
CHECK(read_ok(m.vfs,"moved/new.txt")=="brand new");
REQUIRE(m.vfs.write_file("gone/a.txt",span_of("a")));
REQUIRE(m.vfs.write_file("gone/b/c.txt",span_of("c")));
CHECK(m.vfs.delete_directory("gone",false).error().code==FileError::not_empty);
REQUIRE(m.vfs.delete_directory("gone",true));
CHECK(m.vfs.stat("gone").error().code==FileError::not_found);
// Paths the pack does not cover still land in the folder.
REQUIRE(m.vfs.write_file("data:/u.txt",span_of("unrelated")));
CHECK(file_bytes(m.data()/"u.txt")=="unrelated");
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(m.vfs.open_read("new.txt").error().code==FileError::not_found);
// The mount held the last reference: the storage flushed and closed when it went.
const auto storage=reopen(file,test_key(5));
CHECK(storage->list()==std::vector<std::string>{"Under.txt","dir/stream.bin","keep.txt","log.txt","moved/new.txt","old.txt"});
Vfs fresh;
REQUIRE(fresh.mount("/",pack::make_pack_source(storage,false),false,&owner_a));
CHECK(read_ok(fresh,"old.txt")=="replaced");
CHECK(read_ok(fresh,"Moved/New.txt")=="brand new");
CHECK(read_ok(fresh,"log.txt")=="abcd");
CHECK(read_ok(fresh,"dir/stream.bin")==std::string("0123456789\0\0\0\0\0\0\0\0\0\0ta",22));
}

TEST_CASE("a newer read-only pack over a writable one refuses writes to the names it provides")
{
Mounted m;
auto base=pack_of(m.packs()/"base.zpk",6,{{"s.txt","base"}});
auto patch=pack_of(m.packs()/"patch.zpk",7,{{"s.txt","patched"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(base,true),true,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(patch,false),false,&owner_b));
CHECK(m.vfs.write_file("s.txt",span_of("lost")).error().code==FileError::read_only_pack);
REQUIRE(m.vfs.write_file("t.txt",span_of("into base")));
CHECK(base->stat("t.txt")->kind==EntryKind::file);
CHECK(read_ok(m.vfs,"s.txt")=="patched");
}

TEST_CASE("a deletion marker hides the older packs' and the folder's file, and a newer pack shows it again")
{
Mounted m;
put_bytes(m.root()/"m.txt","folder m");
put_bytes(m.root()/"f.txt","folder f");
put_bytes(m.root()/"kept.txt","folder kept");
auto first=pack_of(m.packs()/"1.zpk",8,{{"m.txt","one"},{"sub/x.txt","x"}});
auto patch=make(m.packs()/"patch.zpk",test_key(9));
REQUIRE(patch->add_deletion_marker("M.TXT"));
REQUIRE(patch->add_deletion_marker("f.txt"));
REQUIRE(patch->add_deletion_marker("sub/x.txt"));
REQUIRE(patch->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(first,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(patch,false),false,&owner_b));
CHECK(m.vfs.stat("m.txt").error().code==FileError::not_found);
CHECK(m.vfs.stat("F.txt").error().code==FileError::not_found);
const auto opened=m.vfs.open_read("m.txt");
REQUIRE_FALSE(opened);
CHECK(zodiac::vfs::read_failure_text("m.txt",opened.error())=="cannot read \"m.txt\": file not found");
CHECK(m.vfs.open_read("sub/x.txt").error().code==FileError::not_found);
CHECK(listed(m.vfs,"/")=="kept.txt");
// Every file of sub is deleted, so sub is no folder any more.
CHECK(m.vfs.list("sub",ListKind::files).error().code==FileError::not_found);
CHECK(m.vfs.stat("sub").error().code==FileError::not_found);
CHECK(m.vfs.remove("f.txt").error().code==FileError::not_found);
CHECK(file_bytes(m.root()/"f.txt")=="folder f");
CHECK(m.vfs.copy("m.txt","data:/c.txt").error().failure.code==FileError::not_found);
// A write below a read-only marker would succeed and never be read back: it is refused like one below a read-only file.
const auto hidden=m.vfs.write_file("f.txt",span_of("new f"));
REQUIRE_FALSE(hidden);
CHECK(hidden.error().message=="\"f.txt\" is in a read-only pack; save files in data:/ instead");
CHECK(m.vfs.open_write("F.TXT",WriteMode::append).error().code==FileError::read_only_pack);
CHECK(file_bytes(m.root()/"f.txt")=="folder f");
auto third=pack_of(m.packs()/"3.zpk",10,{{"m.txt","three"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(third,false),false,&owner_c));
CHECK(read_ok(m.vfs,"m.txt")=="three");
CHECK(m.vfs.stat("f.txt").error().code==FileError::not_found);
CHECK(listed(m.vfs,"/")=="kept.txt,m.txt");
CHECK(m.vfs.unmount(&owner_b)==1);
CHECK(read_ok(m.vfs,"f.txt")=="folder f");
CHECK(read_ok(m.vfs,"sub/x.txt")=="x");
}

TEST_CASE("a file object reads on after unmount_pack and close, and the storage goes when the file closes")
{
ScriptFixture fixture;
const std::string big=data_of(200000,11,true);
{
auto storage=make(fixture.root()/"s.zpk",std::string("k"));
add(*storage,"big.txt",big);
REQUIRE(storage->flush());
}
Steps steps(fixture,
"pack p; file f;"
"string open(){ if(!p.open(\"s.zpk\", \"k\"))return p.error; if(!mount_pack(p))return p.error; if(!f.open(\"big.txt\", \"rb\"))return f.error; return f.read(70000); }"
"string drop(){ unmount_pack(p); string s=file_exists(\"big.txt\")+\",\"; p.close(); return s+f.read(); }"
"string close(){ f.close(); file g; return g.open(\"big.txt\", \"rb\")+\",\"+g.error; }");
CHECK(steps.call("string open()")==big.substr(0,70000));
std::weak_ptr<pack::PackStorage> weak;
{
auto shared=pack::PackStorage::open(fixture.root()/"s.zpk",std::string("k"),false);
REQUIRE(shared);
weak=*shared;
}
const std::string rest=steps.call("string drop()");
CHECK(rest=="false,"+big.substr(70000));
// Only the file object holds it now.
CHECK_FALSE(weak.expired());
CHECK(steps.call("string close()")=="false,cannot read \"big.txt\": file not found");
CHECK(weak.expired());
}

TEST_CASE("concurrent random reads through a mount give the source's bytes while readers open and close")
{
TempDir dir;
const std::vector<std::pair<std::string,std::string>> entries{{"sounds/a.ogg",data_of(3*65536+7,21)},{"sounds/b.ogg",data_of(1000003,22)},{"c.bin",data_of(65536,23)}};
auto storage=pack_of(dir.path/"c.zpk",12,entries);
Vfs vfs;
REQUIRE(vfs.mount("/",pack::make_pack_source(storage,false),false,&owner_a));
storage.reset();
const auto started=std::chrono::steady_clock::now();
std::atomic<std::uint64_t> wrong{0};
std::atomic<std::uint64_t> failed{0};
std::atomic<bool> done{false};
std::atomic<std::uint64_t> opened{0};
std::thread opener([&]{
std::mt19937_64 random(99);
while(!done.load()){
const auto& [name,bytes]=entries[random()%entries.size()];
const auto reader=vfs.open_read(name);
if(!reader||(*reader)->size()!=bytes.size()){
failed.fetch_add(1);
continue;
}
std::byte first{};
if((*reader)->read_at(0,{&first,1})!=1||first!=static_cast<std::byte>(bytes[0]))wrong.fetch_add(1);
opened.fetch_add(1);
}
});
std::vector<std::thread> workers;
for(int t=0;t<8;++t){
workers.emplace_back([&,t]{
std::mt19937_64 random(static_cast<std::uint64_t>(t)*7919+1);
std::vector<std::shared_ptr<zodiac::vfs::Reader>> readers;
for(const auto& [name,bytes]:entries){
auto reader=vfs.open_read(name);
if(!reader){
failed.fetch_add(1);
return;
}
readers.push_back(std::move(*reader));
}
std::vector<std::byte> buffer(200000);
for(int i=0;i<2000;++i){
const std::size_t which=random()%entries.size();
const std::string& bytes=entries[which].second;
const std::uint64_t offset=random()%bytes.size();
const std::size_t length=1+static_cast<std::size_t>(random()%200000);
const std::size_t expected=static_cast<std::size_t>(std::min<std::uint64_t>(length,bytes.size()-offset));
const std::int64_t got=readers[which]->read_at(offset,{buffer.data(),length});
if(got!=static_cast<std::int64_t>(expected)||std::memcmp(buffer.data(),bytes.data()+offset,expected)!=0)wrong.fetch_add(1);
}
});
}
for(std::thread& worker:workers)worker.join();
done.store(true);
opener.join();
const auto elapsed=std::chrono::steady_clock::now()-started;
MESSAGE("16000 reads and "<<opened.load()<<" opens took "<<std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()<<" ms");
CHECK(wrong.load()==0);
CHECK(failed.load()==0);
CHECK(opened.load()>0);
CHECK(elapsed<std::chrono::seconds(30));
}

TEST_CASE("entries added to a mounted pack while threads read through the mount are visible at once, and every read is right")
{
TempDir dir;
const std::vector<std::pair<std::string,std::string>> entries{{"a",data_of(300000,31)},{"b",data_of(65537,32)},{"c",data_of(5,33)}};
auto storage=pack_of(dir.path/"live.zpk",13,entries);
Vfs vfs;
REQUIRE(vfs.mount("/",pack::make_pack_source(storage,false),false,&owner_a));
std::atomic<std::uint64_t> wrong{0};
std::atomic<std::uint64_t> reads{0};
std::atomic<bool> done{false};
std::vector<std::thread> readers;
for(int t=0;t<4;++t){
readers.emplace_back([&,t]{
std::mt19937_64 random(static_cast<std::uint64_t>(t)+500);
std::vector<std::byte> buffer(100000);
while(!done.load()){
const auto& [name,bytes]=entries[random()%entries.size()];
const auto reader=vfs.open_read(name);
if(!reader){
wrong.fetch_add(1);
continue;
}
const std::uint64_t offset=random()%bytes.size();
const std::size_t length=1+static_cast<std::size_t>(random()%buffer.size());
const std::size_t expected=static_cast<std::size_t>(std::min<std::uint64_t>(length,bytes.size()-offset));
const std::int64_t got=(*reader)->read_at(offset,{buffer.data(),length});
if(got!=static_cast<std::int64_t>(expected)||std::memcmp(buffer.data(),bytes.data()+offset,expected)!=0)wrong.fetch_add(1);
reads.fetch_add(1);
}
});
}
int invisible=0;
int flush_failures=0;
for(int i=0;i<200;++i){
const std::string name="new/"+std::to_string(i)+".bin";
const std::string bytes=data_of(1000+static_cast<std::size_t>(i)*997,1000+static_cast<std::uint64_t>(i));
if(!storage->add(name,span_of(bytes),0)){
++invisible;
continue;
}
const auto seen=vfs_read(vfs,name);
if(!seen||*seen!=bytes)++invisible;
if(i%50==49&&!storage->flush())++flush_failures;
}
done.store(true);
for(std::thread& reader:readers)reader.join();
CHECK(invisible==0);
CHECK(flush_failures==0);
CHECK(wrong.load()==0);
CHECK(reads.load()>0);
CHECK(listed(vfs,"new").size()>0);
CHECK(vfs.list("new",ListKind::files)->size()==200);
}

TEST_CASE("a second pack object shares the storage, and closing it leaves the first one's mount serving files")
{
ScriptFixture fixture;
{
auto storage=make(fixture.root()/"two.zpk",std::string("k"));
add(*storage,"base.txt","base");
REQUIRE(storage->flush());
}
CHECK(text_of(fixture,
"pack p; if(!p.open(\"two.zpk\", \"k\"))return p.error;"
"pack q; if(!q.open(\"TWO.zpk\", \"k\"))return q.error;"
"if(!q.add(\"n.txt\", \"new\"))return q.error;"
"string s=p.exists(\"n.txt\")+\",\";"
"if(!mount_pack(p))return p.error;"
"q.close();"
"s+=file_exists(\"n.txt\")+\",\"+read_file(\"N.TXT\")+\",\"+read_file(\"base.txt\")+\",\";"
"p.close();"
"s+=file_exists(\"n.txt\")+\",\"+file_exists(\"base.txt\")+\",\";"
"pack r; if(!r.open(\"two.zpk\", \"k\"))return r.error;"
"return s+r.read(\"n.txt\")+\",\"+r.count;")=="true,true,new,base,false,false,new,2");
}

TEST_CASE("compact under a file object reading a mounted entry: the reader finishes, and a new open reads the compacted file")
{
ScriptFixture fixture;
const std::string big=data_of(300000,41,true);
{
auto storage=make(fixture.root()/"cmp.zpk",std::string("k"));
add(*storage,"waste.txt",data_of(100000,42));
add(*storage,"big.txt",big);
REQUIRE(storage->remove("waste.txt"));
REQUIRE(storage->flush());
}
Steps steps(fixture,
"pack p; file f;"
"string first(){ if(!p.open(\"cmp.zpk\", \"k\"))return p.error; if(!mount_pack(p, \"/assets\"))return p.error; if(!f.open(\"assets/big.txt\", \"rb\"))return f.error; return f.read(100000); }"
"string compact(){ string before=p.wasted_bytes>0?\"wasted\":\"clean\"; if(!p.compact())return p.error; return before+\",\"+p.wasted_bytes; }"
"string rest(){ return f.read(); }"
"string again(){ return read_file(\"assets/big.txt\"); }");
CHECK(steps.call("string first()")==big.substr(0,100000));
CHECK(steps.call("string compact()")=="wasted,0");
CHECK(steps.call("string rest()")==big.substr(100000));
CHECK(steps.call("string again()")==big);
// The compacted file holds big.txt alone: shorter than the old one by at least the removed entry.
CHECK(fs::file_size(fixture.root()/"cmp.zpk")<big.size()+60000);
}

TEST_CASE("mount_pack and unmount_pack from script: failures have exact texts, and writes follow the mount's writability")
{
ScriptFixture fixture;
{
auto storage=make(fixture.root()/"g.zpk",std::string("k"));
add(*storage,"shipped.txt","shipped");
REQUIRE(storage->flush());
}
CHECK(text_of(fixture,
"pack p; string s=mount_pack(p)+\",\"+p.error+\"|\";"
"if(!p.open(\"g.zpk\", \"k\"))return p.error;"
"s+=mount_pack(p, \"../up\")+\",\"+p.error+\"|\";"
"s+=mount_pack(p, \"data:/a/b\")+\",\"+p.error+\"|\";"
"s+=mount_pack(p, \"data:/A/B/\")+\",\"+p.error+\"|\";"
"s+=read_file(\"data:/a/b/shipped.txt\")+\",\";"
"s+=mount_pack(p)+\",\"+p.error+\"|\";"
"s+=write_file(\"shipped.txt\", \"mine\")+\",\"+last_file_error()+\"|\";"
"unmount_pack(p);"
"s+=file_exists(\"shipped.txt\")+\",\"+file_exists(\"data:/a/b/shipped.txt\")+\"|\";"
"if(!mount_pack(p, \"/\", true))return p.error;"
"file f; if(!f.open(\"saved.txt\", \"w\"))return f.error; f.write(\"saved\"); f.close();"
"s+=p.exists(\"saved.txt\")+\",\"+p.read(\"saved.txt\")+\",\"+write_file(\"shipped.txt\", \"mine\")+\",\"+p.read(\"shipped.txt\");"
"return s;")==
"false,pack is not open|false,\"../up\" leaves its mount|true,|false,already mounted there|shipped,true,|false,\"shipped.txt\" is in a read-only pack; save files in data:/ instead|false,false|true,saved,true,mine");
CHECK_FALSE(fs::exists(fixture.root()/"saved.txt"));
}

TEST_CASE("mount_pack refuses a writable mount of a pack it cannot change, throws on a null handle, and delete_file leaves a marker")
{
ScriptFixture fixture;
REQUIRE(fixture.vfs.mount_folder("ro",fixture.folder/"ro",false));
{
auto storage=make(fixture.folder/"ro"/"x.zpk",std::string("k"));
add(*storage,"in.txt","inside");
REQUIRE(storage->flush());
}
{
auto storage=make(fixture.root()/"w.zpk",std::string("k"));
add(*storage,"s.txt","pack s");
REQUIRE(storage->flush());
}
put_bytes(fixture.root()/"s.txt","folder s");
CHECK(text_of(fixture,
"pack p; if(!p.open(\"ro/x.zpk\", \"k\"))return p.error;"
"string s=mount_pack(p, \"/\", true)+\",\"+p.error+\"|\"+mount_pack(p)+\",\"+read_file(\"in.txt\")+\"|\";"
"pack@ none; try{ mount_pack(none); }catch{ s+=exception_info(); } try{ unmount_pack(none); }catch{ s+=\",\"+exception_info(); }"
"pack w; if(!w.open(\"w.zpk\", \"k\"))return w.error; if(!mount_pack(w, \"/\", true))return w.error;"
"s+=\"|\"+read_file(\"s.txt\")+\",\"+delete_file(\"s.txt\")+\",\"+file_exists(\"s.txt\")+\",\"+w.exists(\"s.txt\");"
"s+=\",\"+write_file(\"s.txt\", \"again\")+\",\"+read_file(\"s.txt\");"
"return s;")==
"false,cannot write \"ro/x.zpk\": access denied|true,inside|Null pointer access,Null pointer access|pack s,true,false,false,true,again");
CHECK(file_bytes(fixture.root()/"s.txt")=="folder s");
}

TEST_CASE("a pack never holds a file and a folder of one fold, and its folders follow renames and folder removals")
{
Mounted m;
auto storage=pack_of(m.packs()/"fold.zpk",50,{{"a.txt","a"},{"d/x.txt","x"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
CHECK(m.vfs.write_file("a.txt/x",span_of("no")).error().code==FileError::not_a_directory);
CHECK(m.vfs.open_write("A.TXT/x/y",WriteMode::truncate).error().code==FileError::not_a_directory);
CHECK(m.vfs.write_file("D",span_of("no")).error().code==FileError::is_directory);
CHECK(m.vfs.create_directory("a.txt/sub",false).error().code==FileError::not_a_directory);
CHECK(m.vfs.create_directory("a.txt",false).error().code==FileError::not_a_directory);
REQUIRE(m.vfs.create_directory("d/new",false));
// PackStorage itself refuses them too, so the pack object's add and rename cannot make one either.
CHECK(storage->add("a.txt/b",span_of("no"),0).error()==FileError::not_a_directory);
CHECK(storage->add("d",span_of("no"),0).error()==FileError::is_directory);
CHECK(storage->rename("a.txt","D").error().code==FileError::is_directory);
CHECK(storage->rename("d/x.txt","a.txt/x.txt").error().code==FileError::not_a_directory);
CHECK(storage->list()==std::vector<std::string>{"a.txt","d/x.txt"});
REQUIRE(m.vfs.move("d/x.txt","e/x.txt"));
CHECK(m.vfs.stat("d").error().code==FileError::not_found);
CHECK(m.vfs.stat("E")->kind==EntryKind::directory);
REQUIRE(m.vfs.write_file("e/f/g.txt",span_of("g")));
CHECK(m.vfs.stat("e/F")->kind==EntryKind::directory);
REQUIRE(m.vfs.delete_directory("e",true));
CHECK(m.vfs.stat("e").error().code==FileError::not_found);
CHECK(m.vfs.stat("e/f").error().code==FileError::not_found);
CHECK(storage->stat("e/f/g.txt").error()==FileError::not_found);
// The names are free again, for a file this time.
REQUIRE(m.vfs.write_file("e",span_of("now a file")));
CHECK(read_ok(m.vfs,"E")=="now a file");
}

TEST_CASE("a file written through a pack mount holds at most 2 GiB, refused before anything is allocated")
{
Mounted m;
auto storage=pack_of(m.packs()/"cap.zpk",56,{});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
auto target=m.vfs.open_write("big.bin",WriteMode::truncate);
REQUIRE(target);
zodiac::vfs::WriteTarget& out=**target;
const std::uint64_t before=allocations;
const auto far=out.write_at(std::uint64_t{1}<<40,span_of("x"));
const auto edge=out.write_at(pack::kMaxPackWriteBytes-1,span_of("xy"));
const auto grown=out.truncate(pack::kMaxPackWriteBytes+1);
const std::uint64_t allocated=allocations-before;
REQUIRE_FALSE(far);
CHECK(far.error()==FileError::disk_full);
REQUIRE_FALSE(edge);
CHECK(edge.error()==FileError::disk_full);
REQUIRE_FALSE(grown);
CHECK(grown.error()==FileError::disk_full);
CHECK(allocated==0);
CHECK(out.size()==0);
CHECK(zodiac::vfs::write_failure_text("big.bin",zodiac::vfs::Failure{far.error(),{}})=="cannot write \"big.bin\": disk full");
REQUIRE(out.write_at(0,span_of("ok")));
REQUIRE(out.commit());
CHECK(read_ok(m.vfs,"big.bin")=="ok");
}

TEST_CASE("an append or update of an older mount's file over 2 GiB through a pack mount is refused before anything is allocated")
{
Mounted m;
// One byte more than a pack's file may hold, as a hole on disk.
zodiac_test::make_sparse(m.root()/"huge.log",pack::kMaxPackWriteBytes,"!");
const fs::path file=m.packs()/"seed.zpk";
auto storage=pack_of(file,59,{});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
for(const WriteMode mode:{WriteMode::append,WriteMode::update}){
CAPTURE(static_cast<int>(mode));
largest_allocation=0;
const auto opened=m.vfs.open_write("huge.log",mode);
const std::size_t largest=largest_allocation;
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::disk_full);
CHECK(zodiac::vfs::write_failure_text("huge.log",opened.error())=="cannot write \"huge.log\": disk full");
// The lookup's own small allocations only: no buffer for the file's bytes was asked for.
CHECK(largest<(std::size_t{1}<<20));
}
// Nothing was made in the pack, and the folder's file is as it was.
CHECK(storage->stat("huge.log").error()==FileError::not_found);
CHECK(fs::file_size(m.root()/"huge.log")==pack::kMaxPackWriteBytes+1);
// A file of the pack's size limit or less still starts the pack's copy.
put_bytes(m.root()/"small.log","line 1\n");
auto small=m.vfs.open_write("small.log",WriteMode::append);
REQUIRE(small);
REQUIRE((*small)->write_at((*small)->size(),span_of("line 2\n")));
REQUIRE((*small)->commit());
CHECK(read_ok(m.vfs,"small.log")=="line 1\nline 2\n");
CHECK(file_bytes(m.root()/"small.log")=="line 1\n");
}

TEST_CASE("remove leaves a deletion marker when an older mount still has the name, which a write replaces and a reopen keeps")
{
Mounted m;
put_bytes(m.root()/"f.txt","folder f");
const fs::path file=m.packs()/"rm.zpk";
{
auto storage=pack_of(file,51,{{"f.txt","pack f"},{"only.txt","only"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
REQUIRE(m.vfs.remove("F.txt"));
CHECK(m.vfs.stat("f.txt").error().code==FileError::not_found);
CHECK(m.vfs.open_read("f.txt").error().code==FileError::not_found);
CHECK(listed(m.vfs,"/")=="only.txt");
CHECK(file_bytes(m.root()/"f.txt")=="folder f");
CHECK(storage->stat("f.txt")->kind==EntryKind::deleted);
CHECK(m.vfs.remove("f.txt").error().code==FileError::not_found);
// Nothing older has only.txt: no marker.
REQUIRE(m.vfs.remove("only.txt"));
CHECK(storage->stat("only.txt").error()==FileError::not_found);
REQUIRE(m.vfs.write_file("f.txt",span_of("again")));
CHECK(read_ok(m.vfs,"f.txt")=="again");
CHECK(storage->stat("f.txt")->kind==EntryKind::file);
REQUIRE(m.vfs.remove("f.txt"));
}
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(read_ok(m.vfs,"f.txt")=="folder f");
const auto storage=reopen(file,test_key(51));
CHECK(storage->stat("f.txt")->kind==EntryKind::deleted);
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
CHECK(m.vfs.stat("f.txt").error().code==FileError::not_found);
}

TEST_CASE("an append seeded from an older mount that fails part way leaves no file behind")
{
Mounted m;
const fs::path file=m.packs()/"bad.zpk";
const std::string bytes=data_of(5000,53);
{
auto storage=pack_of(file,52,{{"x.txt",bytes}});
}
// One byte of x.txt's only chunk, which starts right after the empty index and footer the pack began with.
std::string damaged=file_bytes(file);
damaged[pack::kHeaderSize+pack::kMinIndexLength+pack::kFooterSize+100]^=1;
put_bytes(file,damaged);
auto storage=reopen(file,test_key(52));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,false),false,&owner_a));
TempDir newer;
REQUIRE(m.vfs.mount("/",std::make_shared<FolderSource>(newer.path,false),true,&owner_b));
const auto opened=m.vfs.open_write("x.txt",WriteMode::append);
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::damaged);
CHECK_FALSE(fs::exists(newer.path/"x.txt"));
}

TEST_CASE("a listing takes each fold from its newest mount whatever its kind")
{
Mounted m;
put_bytes(m.root()/"maps","a file in the folder");
put_bytes(m.root()/"tiles"/"t.txt","t");
auto storage=pack_of(m.packs()/"kinds.zpk",57,{{"Maps/a.txt","a"},{"TILES","a file in the pack"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,false),false,&owner_a));
CHECK(listed(m.vfs,"/")=="TILES");
CHECK(listed(m.vfs,"/",ListKind::directories)=="Maps");
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(listed(m.vfs,"/")=="maps");
CHECK(listed(m.vfs,"/",ListKind::directories)=="tiles");
}

// D1 (vfs.hpp, Resolution): a marker hides files only. A patch deletes a folder file by file, and a pack's folder every file of which
// a newer marker hides is no folder at all; a host folder's folders, and a pack folder with one shown file, stay.
TEST_CASE("a patch that deletes every file of a pack folder deletes the folder, and a host folder or one shown file keeps it")
{
Mounted m;
auto base=pack_of(m.packs()/"base.zpk",61,{{"maps/a.txt","a"},{"maps/b.txt","b"},{"maps/sub/c.txt","c"},{"levels/l1.txt","l1"},{"levels/l2.txt","l2"},{"top.txt","t"}});
auto patch=make(m.packs()/"patch.zpk",test_key(62));
REQUIRE(patch->add_deletion_marker("maps/a.txt"));
REQUIRE(patch->add_deletion_marker("MAPS/B.txt"));
REQUIRE(patch->add_deletion_marker("maps/sub/c.txt"));
REQUIRE(patch->add_deletion_marker("levels/l1.txt"));
REQUIRE(patch->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(base,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(patch,false),false,&owner_b));
// The whole folder, its subfolder included, is gone by stat, open_read, list (in the parent and as the folder) and delete_directory.
CHECK(m.vfs.stat("maps").error().code==FileError::not_found);
CHECK(m.vfs.stat("Maps/sub").error().code==FileError::not_found);
CHECK(m.vfs.stat("maps/").error().code==FileError::not_found);
CHECK(m.vfs.open_read("maps").error().code==FileError::not_found);
CHECK(m.vfs.open_read("maps/sub/c.txt").error().code==FileError::not_found);
CHECK(m.vfs.list("maps",ListKind::files).error().code==FileError::not_found);
CHECK(m.vfs.list("maps/sub",ListKind::directories).error().code==FileError::not_found);
CHECK(m.vfs.delete_directory("maps",true).error().code==FileError::not_found);
CHECK(listed(m.vfs,"/",ListKind::directories)=="levels");
CHECK(listed(m.vfs,"/")=="top.txt");
// One file of levels is still shown, so levels stays, with only that file.
CHECK(m.vfs.stat("levels")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"levels")=="l2.txt");
// A host folder of that name exists on its own: the game folder's maps/ is found below the deleted pack folder, with its own files.
REQUIRE(m.vfs.create_directory("maps",false));
REQUIRE(m.vfs.write_file("maps/readme.txt",span_of("plain")));
CHECK(file_bytes(m.root()/"maps"/"readme.txt")=="plain");
CHECK(m.vfs.stat("maps")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"maps")=="readme.txt");
CHECK(listed(m.vfs,"/",ListKind::directories)=="levels,maps");
CHECK(m.vfs.stat("maps/sub").error().code==FileError::not_found);
CHECK(read_ok(m.vfs,"maps/readme.txt")=="plain");
// A newer pack that brings one file back brings its folders back.
auto restore=pack_of(m.packs()/"restore.zpk",63,{{"maps/sub/c.txt","c again"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(restore,false),false,&owner_c));
CHECK(m.vfs.stat("maps/sub")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"maps",ListKind::directories)=="sub");
CHECK(read_ok(m.vfs,"maps/sub/c.txt")=="c again");
CHECK(m.vfs.unmount(&owner_c)==1);
CHECK(m.vfs.unmount(&owner_b)==1);
CHECK(listed(m.vfs,"maps")=="a.txt,b.txt,readme.txt");
}

TEST_CASE("a deletion marker hides the older files of its name, never a folder of that fold")
{
Mounted m;
// Oldest first: the game folder's folder tiles/, a pack's file TILES in front of it, then a patch's marker of that file.
put_bytes(m.root()/"tiles"/"t.txt","t");
auto files=pack_of(m.packs()/"files.zpk",64,{{"TILES","the file"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(files,false),false,&owner_a));
CHECK(m.vfs.stat("tiles")->kind==EntryKind::file);
auto patch=make(m.packs()/"patch.zpk",test_key(65));
REQUIRE(patch->add_deletion_marker("tiles"));
REQUIRE(patch->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(patch,false),false,&owner_b));
// The file is hidden; the older folder of that fold is found, with its files, by every lookup and by list.
CHECK(m.vfs.stat("tiles")->kind==EntryKind::directory);
CHECK(m.vfs.open_read("tiles").error().code==FileError::is_directory);
CHECK(read_ok(m.vfs,"tiles/t.txt")=="t");
CHECK(listed(m.vfs,"/").empty());
CHECK(listed(m.vfs,"/",ListKind::directories)=="tiles");
CHECK(listed(m.vfs,"tiles")=="t.txt");
CHECK(m.vfs.remove("tiles").error().code==FileError::is_directory);
CHECK(m.vfs.unmount(&owner_b)==1);
CHECK(m.vfs.unmount(&owner_a)==1);
// A pack holding a marker and a folder of one fold has that folder.
auto both=make(m.packs()/"both.zpk",test_key(66));
REQUIRE(both->add_deletion_marker("maps"));
add(*both,"maps/new.txt","new");
REQUIRE(both->flush());
CHECK(both->stat("maps")->kind==EntryKind::directory);
CHECK(both->place("maps").value()==EntryKind::directory);
CHECK(both->count_below("").files==1);
CHECK(both->count_below("").markers==1);
CHECK(both->count_below("maps").files==1);
CHECK(both->count_below("maps").markers==0);
REQUIRE(m.vfs.mount("/",pack::make_pack_source(both,false),false,&owner_a));
CHECK(m.vfs.stat("maps")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"/",ListKind::directories)=="maps,tiles");
CHECK(listed(m.vfs,"maps")=="new.txt");
CHECK(m.vfs.unmount(&owner_a)==1);
// Removing a writable pack's file over an older folder of its fold leaves no marker: the folder shows again.
auto writable=pack_of(m.packs()/"w.zpk",67,{{"Tiles","w"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(writable,true),true,&owner_a));
CHECK(m.vfs.stat("tiles")->kind==EntryKind::file);
REQUIRE(m.vfs.remove("tiles"));
CHECK(writable->stat("tiles").error()==FileError::not_found);
CHECK(m.vfs.stat("tiles")->kind==EntryKind::directory);
CHECK(read_ok(m.vfs,"tiles/t.txt")=="t");
}

namespace {
// A source that counts what folder_shown asks of the one it forwards to: stat calls, marker enumerations and the names they give.
class CountingSource final:public zodiac::vfs::Source{
public:
explicit CountingSource(std::shared_ptr<zodiac::vfs::Source> inner):inner_(std::move(inner)){}
std::uint64_t stats=0;
std::uint64_t enumerations=0;
std::uint64_t enumerated=0;
std::expected<zodiac::vfs::EntryInfo,FileError> stat(std::string_view relative) override{++stats;return inner_->stat(relative);}
std::expected<std::shared_ptr<zodiac::vfs::Reader>,FileError> open_read(std::string_view relative) override{return inner_->open_read(relative);}
std::expected<std::unique_ptr<zodiac::vfs::WriteTarget>,FileError> open_write(std::string_view relative,WriteMode mode) override{return inner_->open_write(relative,mode);}
std::expected<void,FileError> list(std::string_view relative,std::vector<zodiac::vfs::EntryInfo>& out) override{return inner_->list(relative,out);}
std::expected<void,FileError> remove(std::string_view relative,bool leave_marker) override{return inner_->remove(relative,leave_marker);}
std::expected<void,FileError> make_directory(std::string_view relative) override{return inner_->make_directory(relative);}
std::expected<void,FileError> remove_directory(std::string_view relative,bool recursive) override{return inner_->remove_directory(relative,recursive);}
bool read_only() const noexcept override{return inner_->read_only();}
bool implied_folders() const noexcept override{return inner_->implied_folders();}
zodiac::vfs::EntriesBelow count_below(std::string_view relative) override{return inner_->count_below(relative);}
void markers_below(std::string_view relative,std::vector<std::string>& out) override
{
++enumerations;
const std::size_t before=out.size();
inner_->markers_below(relative,out);
enumerated+=out.size()-before;
}
private:
std::shared_ptr<zodiac::vfs::Source> inner_;
};
}

TEST_CASE("a pack folder with more files than the newer markers below it is shown from the counts alone")
{
Mounted m;
auto base=make(m.packs()/"big.zpk",test_key(71));
for(int i=0;i<3000;++i)add(*base,"big/f"+std::to_string(i)+".txt","x");
add(*base,"small/a.txt","a");
add(*base,"small/b.txt","b");
REQUIRE(base->flush());
auto patch=make(m.packs()/"few.zpk",test_key(72));
for(int i=0;i<3;++i)REQUIRE(patch->add_deletion_marker("big/f"+std::to_string(i)+".txt"));
// Many markers elsewhere, and both files of small/.
for(int i=0;i<500;++i)REQUIRE(patch->add_deletion_marker("other/o"+std::to_string(i)+".txt"));
REQUIRE(patch->add_deletion_marker("small/a.txt"));
REQUIRE(patch->add_deletion_marker("Small/B.TXT"));
REQUIRE(patch->flush());
auto older=std::make_shared<CountingSource>(pack::make_pack_source(base,false));
auto newer=std::make_shared<CountingSource>(pack::make_pack_source(patch,false));
REQUIRE(m.vfs.mount("/",older,false,&owner_a));
REQUIRE(m.vfs.mount("/",newer,false,&owner_b));
// 3000 files against 3 markers: shown without one name enumerated or one file looked up beyond the folder's own stat.
CHECK(m.vfs.stat("big")->kind==EntryKind::directory);
CHECK(newer->enumerations==0);
CHECK(older->stats==1);
// small/ has 2 files against 2 markers: only small/'s markers are visited (not other/'s 500), and the folder is gone.
CHECK(m.vfs.stat("small").error().code==FileError::not_found);
CHECK(newer->enumerations==1);
CHECK(newer->enumerated==2);
// The folder's stat, then for each of the 2 names: is it a file here, and the lookup that decides it (as stat would).
CHECK(older->stats==1+1+2*2);
CHECK(listed(m.vfs,"/",ListKind::directories)=="big");
}

TEST_CASE("a recursive delete in a writable pack leaves markers for the older files below the folder, kept after a reopen")
{
Mounted m;
// The game folder's levels/ (a host folder), the release's levels/, and a writable pack over both holding levels/ too.
put_bytes(m.root()/"levels"/"host.txt","host");
auto release=pack_of(m.packs()/"release.zpk",73,{{"levels/l1.txt","1"},{"levels/deep/l2.txt","2"},{"keep.txt","k"}});
const fs::path save=m.packs()/"save.zpk";
{
auto writable=pack_of(save,74,{{"levels/mine.txt","mine"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(release,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(writable,true),true,&owner_b));
REQUIRE(m.vfs.delete_directory("levels",true));
const auto check=[&]{
for(const char* gone:{"levels/l1.txt","levels/deep/l2.txt","levels/mine.txt","levels/host.txt"}){
INFO(gone);
CHECK(m.vfs.stat(gone).error().code==FileError::not_found);
}
CHECK(m.vfs.stat("levels/deep").error().code==FileError::not_found);
// The host folder itself stays, empty: a host folder's folders exist on their own.
CHECK(m.vfs.stat("levels")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"levels").empty());
CHECK(listed(m.vfs,"levels",ListKind::directories).empty());
CHECK(read_ok(m.vfs,"keep.txt")=="k");
CHECK(file_bytes(m.root()/"levels"/"host.txt")=="host");
};
check();
CHECK(writable->stat("levels/l1.txt")->kind==EntryKind::deleted);
CHECK(writable->stat("levels/deep/l2.txt")->kind==EntryKind::deleted);
CHECK(writable->stat("levels/host.txt")->kind==EntryKind::deleted);
CHECK(writable->stat("levels/mine.txt").error()==FileError::not_found);
REQUIRE(writable->close());
CHECK(m.vfs.unmount(&owner_b)==1);
auto reopened=reopen(save,test_key(74));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(reopened,true),true,&owner_b));
check();
}
// Without a host folder of that name, the release's folder goes whole.
Mounted fresh;
auto again=pack_of(fresh.packs()/"release.zpk",75,{{"levels/l1.txt","1"}});
auto writable=pack_of(fresh.packs()/"save.zpk",76,{{"levels/mine.txt","mine"}});
REQUIRE(fresh.vfs.mount("/",pack::make_pack_source(again,false),false,&owner_a));
REQUIRE(fresh.vfs.mount("/",pack::make_pack_source(writable,true),true,&owner_b));
REQUIRE(fresh.vfs.delete_directory("levels",true));
CHECK(fresh.vfs.stat("levels").error().code==FileError::not_found);
CHECK(listed(fresh.vfs,"/",ListKind::directories).empty());
}

TEST_CASE("remove_folder_leaving removes a folder and writes its markers as one change, or changes nothing")
{
TempDir dir;
auto storage=pack_of(dir.path/"leave.zpk",77,{{"levels/a.txt","a"},{"levels/deep/b.txt","b"},{"keep.txt","k"}});
REQUIRE(storage->add_deletion_marker("levels/old.txt"));
// A name outside the folder, or one no entry can have, is refused before anything changes.
for(const std::vector<std::string>& bad:{std::vector<std::string>{"levels/x.txt","keep.txt"},std::vector<std::string>{"levels/con"},std::vector<std::string>{"levels\\x.txt"}}){
CHECK(storage->remove_folder_leaving("levels",bad).error()==FileError::bad_path);
CHECK(storage->list()==std::vector<std::string>{"keep.txt","levels/a.txt","levels/deep/b.txt"});
CHECK(storage->count_below("levels").markers==1);
}
CHECK(storage->remove_folder_leaving("keep.txt",{}).error()==FileError::not_a_directory);
CHECK(storage->remove_folder_leaving("none",{}).error()==FileError::not_found);
// a.txt's entry becomes its marker in place, x.txt and deep/y.txt get new ones (their folders made), old.txt's marker stays.
const auto removed=storage->remove_folder_leaving("levels",{"levels/a.txt","levels/x.txt","Levels/X.TXT","levels/deep/y.txt","levels/old.txt"});
REQUIRE(removed);
CHECK(*removed==2);
CHECK(storage->list()==std::vector<std::string>{"keep.txt"});
for(const char* name:{"levels/a.txt","levels/x.txt","levels/deep/y.txt","levels/old.txt"}){
INFO(name);
CHECK(storage->stat(name)->kind==EntryKind::deleted);
}
CHECK(storage->stat("levels/deep/b.txt").error()==FileError::not_found);
CHECK(storage->count_below("levels").files==0);
CHECK(storage->count_below("levels").markers==4);
CHECK(storage->count_below("levels/deep").markers==1);
std::vector<std::string> marked;
storage->markers_below("levels",marked);
std::ranges::sort(marked);
CHECK(marked==std::vector<std::string>{"levels/a.txt","levels/deep/y.txt","levels/old.txt","levels/x.txt"});
// A file written after the change replaces its marker, as any add does: the write survives, and so it does after a reopen.
add(*storage,"levels/a.txt","written later");
CHECK(storage->stat("levels/a.txt")->kind==EntryKind::file);
REQUIRE(storage->close());
const auto reopened=reopen(dir.path/"leave.zpk",test_key(77));
CHECK(reopened->list()==std::vector<std::string>{"keep.txt","levels/a.txt"});
CHECK(reopened->stat("levels/x.txt")->kind==EntryKind::deleted);
CHECK(reopened->count_below("levels").markers==3);
}

TEST_CASE("a file written into a writable pack after a recursive delete of its folder survives")
{
Mounted m;
auto release=pack_of(m.packs()/"release.zpk",78,{{"levels/l1.txt","1"},{"levels/l2.txt","2"}});
auto writable=pack_of(m.packs()/"save.zpk",79,{{"levels/mine.txt","mine"}});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(release,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(writable,true),true,&owner_b));
REQUIRE(m.vfs.delete_directory("levels",true));
CHECK(m.vfs.stat("levels").error().code==FileError::not_found);
// The delete and its markers were one change of the pack, so nothing later is turned into a marker.
REQUIRE(m.vfs.write_file("levels/l1.txt",span_of("new")));
CHECK(read_ok(m.vfs,"levels/l1.txt")=="new");
CHECK(m.vfs.stat("levels/l2.txt").error().code==FileError::not_found);
CHECK(listed(m.vfs,"levels")=="l1.txt");
// A delete in a host folder leaves nothing (it cannot hold markers): the older files show again.
Mounted host;
auto shipped=pack_of(host.packs()/"r.zpk",80,{{"maps/m.txt","m"}});
REQUIRE(host.vfs.mount("/",pack::make_pack_source(shipped,false),false,&owner_a));
TempDir newer;
fs::create_directories(newer.path/"maps");
put_bytes(newer.path/"maps"/"n.txt","n");
REQUIRE(host.vfs.mount("/",std::make_shared<FolderSource>(newer.path,false),true,&owner_b));
REQUIRE(host.vfs.delete_directory("maps",true));
CHECK(read_ok(host.vfs,"maps/m.txt")=="m");
}

TEST_CASE("a folder with more newer markers than files is still shown when some markers name files it lacks")
{
Mounted m;
auto base=pack_of(m.packs()/"base.zpk",81,{{"few/a.txt","a"},{"few/b.txt","b"}});
auto patch=make(m.packs()/"patch.zpk",test_key(82));
for(const char* name:{"few/a.txt","few/x.txt","few/y.txt","few/z/w.txt"})REQUIRE(patch->add_deletion_marker(name));
REQUIRE(patch->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(base,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(patch,false),false,&owner_b));
// 4 markers against 2 files, but only a.txt is one of them: b.txt is left.
CHECK(m.vfs.stat("few")->kind==EntryKind::directory);
CHECK(listed(m.vfs,"few")=="b.txt");
CHECK(listed(m.vfs,"/",ListKind::directories)=="few");
}

TEST_CASE("a newer folder that is also a deleted file's name hides the older file as a marker when the folder is not shown")
{
Mounted m;
// Oldest: the file p/X. Newer: a marker of p/X and a folder p/X with one file. Newest: a marker of that file.
auto oldest=pack_of(m.packs()/"o.zpk",83,{{"p/X","old file"}});
auto newer=make(m.packs()/"n.zpk",test_key(84));
REQUIRE(newer->add_deletion_marker("p/X"));
add(*newer,"p/X/inner.txt","inner");
REQUIRE(newer->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(oldest,false),false,&owner_a));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(newer,false),false,&owner_b));
// The newer folder is shown: it is the name, and the old file stays hidden.
CHECK(m.vfs.stat("p/X")->kind==EntryKind::directory);
CHECK(read_ok(m.vfs,"p/X/inner.txt")=="inner");
auto newest=make(m.packs()/"m.zpk",test_key(85));
REQUIRE(newest->add_deletion_marker("p/X/inner.txt"));
REQUIRE(newest->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(newest,false),false,&owner_c));
// The folder is not shown now, and its marker still hides the old file: stat, open_read, list and the folders above agree.
CHECK(m.vfs.stat("p/X").error().code==FileError::not_found);
CHECK(m.vfs.open_read("p/X").error().code==FileError::not_found);
CHECK(m.vfs.stat("p").error().code==FileError::not_found);
CHECK(m.vfs.list("p",ListKind::files).error().code==FileError::not_found);
CHECK(listed(m.vfs,"/",ListKind::directories).empty());
CHECK(m.vfs.write_file("p/X",span_of("w")).error().code==FileError::read_only_pack);
}

// D2 (vfs.hpp, Mount folders): no mount's file or marker hides a mount point or a folder above one.
TEST_CASE("a mount point and the folders above it stay folders whatever a newer mount holds there")
{
Mounted m;
auto extra=pack_of(m.packs()/"extra.zpk",68,{{"x.txt","x"}});
REQUIRE(m.vfs.mount("mods/extra",pack::make_pack_source(extra,false),false,&owner_a));
auto over=make(m.packs()/"over.zpk",test_key(69));
add(*over,"MODS","a file");
REQUIRE(over->add_deletion_marker("mods2"));
REQUIRE(over->flush());
auto marks=make(m.packs()/"marks.zpk",test_key(70));
REQUIRE(marks->add_deletion_marker("mods/extra"));
REQUIRE(marks->flush());
REQUIRE(m.vfs.mount("/",pack::make_pack_source(over,false),false,&owner_b));
REQUIRE(m.vfs.mount("/",pack::make_pack_source(marks,false),false,&owner_c));
for(const char* folder:{"mods","Mods/Extra"}){
INFO(folder);
CHECK(m.vfs.stat(folder)->kind==EntryKind::directory);
CHECK(m.vfs.open_read(folder).error().code==FileError::is_directory);
CHECK(m.vfs.write_file(folder,span_of("w")).error().code==FileError::is_directory);
CHECK(m.vfs.remove(folder).error().code==FileError::is_directory);
CHECK(m.vfs.delete_directory(folder,true).error().code==FileError::access_denied);
REQUIRE(m.vfs.create_directory(folder,false));
}
CHECK(listed(m.vfs,"/").empty());
CHECK(listed(m.vfs,"/",ListKind::directories)=="mods");
CHECK(listed(m.vfs,"mods",ListKind::directories)=="extra");
CHECK(listed(m.vfs,"mods/extra")=="x.txt");
CHECK(read_ok(m.vfs,"MODS/EXTRA/x.txt")=="x");
// Without the mount point, the newer pack's file is what the name is.
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(m.vfs.stat("mods")->kind==EntryKind::file);
CHECK(listed(m.vfs,"/")=="MODS");
CHECK(listed(m.vfs,"/",ListKind::directories).empty());
}

TEST_CASE("unmount cannot fail, a remount under the same owner works, and try_mount names its failures")
{
Mounted m;
auto storage=pack_of(m.packs()/"owner.zpk",58,{{"o.txt","o"}});
REQUIRE(m.vfs.try_mount("/",pack::make_pack_source(storage,false),false,&owner_a));
CHECK(m.vfs.try_mount("/",pack::make_pack_source(storage,false),false,&owner_a).error()==Vfs::MountError::taken);
CHECK(m.vfs.try_mount("../x",pack::make_pack_source(storage,false),false,&owner_b).error()==Vfs::MountError::bad_point);
CHECK(m.vfs.try_mount("/",nullptr,false,&owner_b).error()==Vfs::MountError::bad_point);
CHECK(m.vfs.unmount(&owner_a)==1);
CHECK(m.vfs.unmount(&owner_a)==0);
CHECK(m.vfs.open_read("o.txt").error().code==FileError::not_found);
REQUIRE(m.vfs.try_mount("/",pack::make_pack_source(storage,false),false,&owner_a));
CHECK(read_ok(m.vfs,"o.txt")=="o");
}

TEST_CASE("sync stores a pack file's bytes durably before it is closed")
{
Mounted m;
const fs::path file=m.packs()/"sync.zpk";
auto storage=pack_of(file,54,{});
REQUIRE(m.vfs.mount("/",pack::make_pack_source(storage,true),true,&owner_a));
auto target=m.vfs.open_write("saved.txt",WriteMode::truncate);
REQUIRE(target);
REQUIRE((*target)->write_at(0,span_of("line 1\n")));
REQUIRE((*target)->sync());
const auto on_disk=[&](const char* copy_name){
// The file as the device has it now, opened as a pack of its own.
const fs::path copy=m.packs()/copy_name;
put_bytes(copy,file_bytes(file));
const auto copied=reopen(copy,test_key(54));
const auto reader=copied->open_reader("saved.txt");
REQUIRE(reader);
const auto bytes=read_all(**reader);
REQUIRE(bytes);
return *bytes;
};
CHECK(on_disk("copy1.zpk")=="line 1\n");
REQUIRE((*target)->write_at(7,span_of("line 2\n")));
REQUIRE((*target)->sync());
CHECK(on_disk("copy2.zpk")=="line 1\nline 2\n");
REQUIRE((*target)->commit());
}

TEST_CASE("a lookup of a Turkish name, or one longer than the stack fold, allocates nothing but the long fold")
{
TempDir dir;
// 288 bytes: past PackStorage::kStackFold, so its fold goes to a string (one allocation, as the name is ASCII and folds to its own
// size). Its last segment is short, so the name stat returns stays inside the string's own buffer and allocates nothing.
const std::string long_name=std::string(200,'a')+"/"+std::string(80,'b')+"/x.ogg";
REQUIRE(long_name.size()>pack::PackStorage::kStackFold);
const std::string long_lookup=std::string(200,'A')+"/"+std::string(80,'B')+"/X.OGG";
auto storage=pack_of(dir.path/"names.zpk",60,{{"M\xC3\xBCzik/I\xC5\x9EIK.ogg","t"},{long_name,"l"}});
REQUIRE(storage->stat("M\xC3\xBCzik/I\xC5\x9EIK.ogg"));
REQUIRE(storage->stat(long_name));
const std::uint64_t before=allocations;
// IŞIK.ogg is found as ışık.ogg and işik.ogg, never as İŞİK.ogg (the fold is Unicode simple uppercase per code point).
const auto dotless=storage->stat("m\xC3\xBCzik/\xC4\xB1\xC5\x9F\xC4\xB1k.ogg");
const auto dotted=storage->stat("M\xC3\x9CZIK/i\xC5\x9Fik.ogg");
const auto capital_dotted=storage->stat("m\xC3\xBCzik/\xC4\xB0\xC5\x9E\xC4\xB0K.ogg");
const auto folder=storage->stat("M\xC3\x9CZIK");
const std::uint64_t turkish=allocations-before;
const std::uint64_t long_before=allocations;
const auto long_hit=storage->stat(long_lookup);
const std::uint64_t long_allocated=allocations-long_before;
REQUIRE(dotless);
CHECK(dotless->kind==EntryKind::file);
REQUIRE(dotted);
CHECK(dotted->kind==EntryKind::file);
CHECK_FALSE(capital_dotted);
REQUIRE(folder);
CHECK(folder->kind==EntryKind::directory);
CHECK(turkish==0);
REQUIRE(long_hit);
CHECK(long_hit->kind==EntryKind::file);
CHECK(long_allocated==1);
}

TEST_CASE("a lookup that finds an entry allocates nothing but what it returns")
{
TempDir dir;
auto storage=pack_of(dir.path/"alloc.zpk",55,{{"sounds/step.ogg","s"}});
REQUIRE(storage->stat("sounds/step.ogg"));
const std::uint64_t before=allocations;
const auto hit=storage->stat("SOUNDS/Step.ogg");
const auto miss=storage->stat("sounds/none.ogg");
const auto folder=storage->stat("Sounds");
const auto place=storage->place("sounds/new.ogg");
const std::uint64_t looked=allocations-before;
const std::uint64_t opening=allocations;
const auto reader=storage->open_mounted("sounds/STEP.ogg",storage);
const std::uint64_t opened=allocations-opening;
REQUIRE(hit);
CHECK(hit->kind==EntryKind::file);
CHECK_FALSE(miss);
REQUIRE(folder);
CHECK(folder->kind==EntryKind::directory);
REQUIRE(place);
CHECK(*place==EntryKind::none);
CHECK(looked==0);
REQUIRE(reader);
// The reader itself (one make_shared), nothing else.
CHECK(opened==1);
}
