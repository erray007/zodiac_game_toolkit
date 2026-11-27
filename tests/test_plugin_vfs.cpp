// The plugin VFS entries (include/zodiac/plugin.h, VFS rules; src/core/host.hpp), through a real ScriptRuntime with mounted packs:
// - the vfs_reader fixture module reads a packed file as scripts do (case-insensitive names), gets NOT_FOUND, BAD_PATH and
//   DAMAGED (none of a damaged chunk's bytes delivered), and reads one file on 8 threads at once, each through its own handle;
// - a module built against an older struct_size still loads and registers, and the fixture's struct_size check refuses a host
//   table without the VFS entries, at registration and at a call;
// - the C++ VfsFile of plugin.hpp, from the fixture module and directly (string_view paths, a NUL in one, moves, struct_size);
// - a handle of one host refused by another in the same process; drive and host: paths refused with host paths off;
// - straight through the table: bad, closed, double-closed and forged handles; bounds-checked seeks; reads at chunk edges and
//   past the end; vfs_exists; a host without a VFS; one handle shared by several threads (serialized: every byte read once) and
//   closed under them; slot reuse; a file read after its pack is unmounted; and many handles at once.
// Packs are made with raw keys and 64 KiB chunks.
#include "script_fixture.hpp"
#include <core/diagnostics.hpp>
#include <core/module_registry.hpp>
#include <core/pack/pack_format.hpp>
#include <core/pack/pack_source.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <platform/console.hpp>
#include <doctest/doctest.h>
#include <zodiac/plugin.h>
#include <zodiac/plugin.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
namespace pack=zodiac::pack;
namespace crypto=zodiac::crypto;
// Runs before doctest starts so a failing module load can never raise a Windows modal dialog.
const int g_dialogs_disabled=(zodiac::platform::disable_error_dialogs(),0);
const pack::PackOptions kFast{64,1,16};
constexpr std::size_t kChunk=65536;
// 3 chunks and 7 bytes: an entry whose last chunk is short.
constexpr std::size_t kBigSize=3*kChunk+7;
constexpr std::size_t kWords=25000;
int owner_game=0;
int owner_broken=0;

fs::path from_utf8(std::string_view text)
{
return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
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
// size pseudo-random lower-case letters, so a script compares them as text.
std::string text_of(std::size_t size,std::uint64_t seed)
{
std::mt19937_64 random(seed);
std::string bytes(size,'\0');
for(char& c:bytes)c=static_cast<char>('a'+random()%26);
return bytes;
}
// kWords little-endian 64-bit words, word i holding i.
std::string words()
{
std::string bytes(kWords*8,'\0');
for(std::uint64_t i=0;i<kWords;++i)for(unsigned b=0;b<8;++b)bytes[i*8+b]=static_cast<char>((i>>(8*b))&0xff);
return bytes;
}
std::uint64_t word_at(const char* bytes)
{
std::uint64_t value=0;
for(unsigned b=0;b<8;++b)value|=std::uint64_t{static_cast<unsigned char>(bytes[b])}<<(8*b);
return value;
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
std::span<const std::byte> span_of(std::string_view text)
{
return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
// A flushed pack at path holding entries; the storage is dropped, so the file can be changed and opened again.
void write_pack(const fs::path& path,std::uint8_t seed,const std::vector<std::pair<std::string,std::string>>& entries)
{
fs::create_directories(path.parent_path());
auto storage=pack::PackStorage::open(path,test_key(seed),true,kFast);
REQUIRE(storage);
for(const auto& [name,bytes]:entries)REQUIRE((*storage)->add(name,span_of(bytes),0));
REQUIRE((*storage)->flush());
REQUIRE((*storage)->close());
}
std::shared_ptr<pack::PackStorage> open_pack(const fs::path& path,std::uint8_t seed)
{
auto storage=pack::PackStorage::open(path,test_key(seed),false);
REQUIRE(storage);
return std::move(*storage);
}
class MemorySink final:public zodiac::DiagnosticSink{
public:
explicit MemorySink(std::vector<std::string>& lines):lines_(lines){}
void write(zodiac_log_level,std::string_view line) noexcept override{lines_.emplace_back(line);}
private:
std::vector<std::string>& lines_;
};
std::string joined(const std::vector<std::string>& lines)
{
std::string out;
for(const std::string& line:lines)out.append(line).push_back('\n');
return out;
}

// A game: a project in a temp folder whose main.zs is main, using modules (dynamic), and a ScriptRuntime whose Vfs mounts two
// packs at "/": game.zpk (data/a.txt, data/b.txt, words.bin, the folder data/) and broken.zpk (broken/c.bin, chunk 1 damaged).
struct Game{
TempDir dir;
std::vector<std::string> lines;
zodiac::Diagnostics diagnostics;
std::unique_ptr<zodiac::ScriptRuntime> runtime;
std::string big=text_of(kBigSize,1);
std::string broken=text_of(kBigSize,2);
// host_size, when set, cuts the host table to that struct_size before the modules register; build must then give built.
Game(std::string_view main,std::string_view modules,std::uint32_t host_size=0,int built=0)
{
const fs::path root=dir.path/"game";
put_bytes(root/"zodiac.toml","[game]\nmain = \"main.zs\"\n[modules]\ndynamic = ["+std::string(modules)+"]\n");
put_bytes(root/"main.zs",main);
write_pack(dir.path/"game.zpk",3,{{"data/a.txt",big},{"data/b.txt","hello pack"},{"words.bin",words()}});
const fs::path damaged=dir.path/"broken.zpk";
write_pack(damaged,4,{{"broken/c.bin",broken}});
// The only entry starts right after the creation's index and footer; flip a byte inside chunk 1's ciphertext.
std::string file=file_bytes(damaged);
const std::size_t chunk1=pack::kHeaderSize+pack::kMinIndexLength+pack::kFooterSize+kChunk+pack::kTagSize;
REQUIRE(file.size()>chunk1+100);
file[chunk1+100]^=1;
put_bytes(damaged,file);
diagnostics.add_sink(std::make_unique<MemorySink>(lines));
runtime=std::make_unique<zodiac::ScriptRuntime>(diagnostics,zodiac::ModuleSearchPaths{{from_utf8(ZODIAC_TEST_FIXTURE_DIR)}});
REQUIRE(runtime->vfs().mount("/",pack::make_pack_source(open_pack(dir.path/"game.zpk",3),false),false,&owner_game));
REQUIRE(runtime->vfs().mount("/",pack::make_pack_source(open_pack(damaged,4),false),false,&owner_broken));
auto project=zodiac::load_project(root);
INFO(project.error_or(""));
REQUIRE(project);
zodiac_host* table=runtime->host().abi();
const std::uint32_t size=table->struct_size;
if(host_size)table->struct_size=host_size;
const int result=runtime->build(*project);
table->struct_size=size;
INFO(joined(lines));
REQUIRE(result==built);
}
~Game()
{
runtime.reset();
}
Game(const Game&)=delete;
Game& operator=(const Game&)=delete;
zodiac_host* host() const{return runtime->host().abi();}
int run()
{
const int result=runtime->run();
INFO(joined(lines));
return result;
}
};
constexpr std::string_view kNoScript="int main(){return 0;}";

// An open handle, closed at the end of the scope.
struct File{
zodiac_host* host;
zodiac_vfs_file* file=nullptr;
File(zodiac_host* owner,const char* path):host(owner)
{
REQUIRE(host->vfs_open(host,path,&file)==ZODIAC_OK);
REQUIRE(file!=nullptr);
}
~File()
{
host->vfs_close(host,file);
}
File(const File&)=delete;
File& operator=(const File&)=delete;
};
zodiac_vfs_file* forged(std::uint64_t value)
{
return reinterpret_cast<zodiac_vfs_file*>(static_cast<std::uintptr_t>(value));
}
// Threads joined at the end of the scope (or by join), also when a REQUIRE leaves it.
struct Threads{
std::vector<std::thread> list;
Threads()=default;
Threads(const Threads&)=delete;
Threads& operator=(const Threads&)=delete;
~Threads()
{
join();
}
void join()
{
for(std::thread& thread:list)if(thread.joinable())thread.join();
list.clear();
}
};
}

TEST_CASE("a plugin reads packed files through the VFS exactly as scripts do")
{
Game game(R"(void expect(const string &in got,const string &in want)
{
if(got!=want)throw("got \""+got+"\", want \""+want+"\"");
}
string failure(const string &in path)
{
try{
vfs_read_all(path);
}
catch{
return exception_info();
}
return "no failure";
}
int main()
{
expect(vfs_read_all("data/B.txt"),"hello pack");
const string big=vfs_read_all("Data/A.TXT");
if(big.length()!=196615)throw("the plugin read "+big.length()+" bytes");
if(big!=read_file("data/a.txt"))throw("the plugin read other bytes than the script");
expect(failure("missing.txt"),"vfs_open failed: -6 after 0 bytes");
expect(failure("../x"),"vfs_open failed: -9 after 0 bytes");
expect(failure("con"),"vfs_open failed: -9 after 0 bytes");
expect(failure("data"),"vfs_open failed: -9 after 0 bytes");
expect(failure("data/"),"vfs_open failed: -9 after 0 bytes");
// The first chunk is delivered, the damaged one not at all.
expect(failure("broken/c.bin"),"vfs_read failed: -8 after 65536 bytes");
const int threads=vfs_threads("Data/A.TXT",8);
if(threads!=0)throw(threads+" threads read other bytes");
const int damaged=vfs_threads("broken/c.bin",2);
if(damaged!=-8)throw("vfs_threads on the damaged file gave "+damaged);
return 0;
})","\"vfs_reader\"");
CHECK(game.run()==0);
}

TEST_CASE("a plugin reads through the C++ VfsFile of plugin.hpp")
{
Game game(R"(void expect(const string &in got,const string &in want)
{
if(got!=want)throw("got \""+got+"\", want \""+want+"\"");
}
string failure(const string &in path)
{
try{
vfs_read_hpp(path);
}
catch{
return exception_info();
}
return "no failure";
}
int main()
{
expect(vfs_read_hpp("data/B.txt"),"hello pack");
if(vfs_read_hpp("Data/A.TXT")!=read_file("data/a.txt"))throw("VfsFile read other bytes than the script");
expect(failure("missing.txt"),"VfsFile::open failed: -6");
expect(failure("../x"),"VfsFile::open failed: -9");
expect(failure("broken/c.bin"),"VfsFile::read failed: -8");
if(vfs_exists_hpp("data/b.txt")!=1)throw("data/b.txt is no file");
if(vfs_exists_hpp("data")!=0)throw("data is a file");
if(vfs_exists_hpp("nothing.txt")!=0)throw("nothing.txt is a file");
if(vfs_exists_hpp("../x")!=-9)throw("../x is a valid path");
return 0;
})","\"vfs_reader\"");
CHECK(game.run()==0);
}

TEST_CASE("a module that needs the VFS refuses to register in a host table without the VFS entries")
{
Game game(kNoScript,"\"vfs_reader\"",static_cast<std::uint32_t>(offsetof(zodiac_host,vfs_open)),2);
CHECK(std::ranges::any_of(game.lines,[](const std::string& line){return line.find("vfs_reader needs a Zodiac whose plugin table has the VFS entries")!=std::string::npos;}));
// The module made before the VFS entries does not need them, so it registers in such a table.
Game old(R"(int main(){return vfs_reader_old(1)==2?0:1;})","\"vfs_reader_old\"",static_cast<std::uint32_t>(offsetof(zodiac_host,vfs_open)));
CHECK(old.run()==0);
}

TEST_CASE("the C++ VfsFile copies a string_view path, refuses a NUL in it, moves, and checks struct_size itself")
{
using zodiac::plugin::VfsFile;
Game game(kNoScript,"");
zodiac_host* host=game.host();
// A string_view that is not NUL-terminated where it ends.
const std::string text="data/b.txtXYZ";
auto opened=VfsFile::open(host,std::string_view(text).substr(0,10));
REQUIRE(opened);
VfsFile file=std::move(opened.value);
char buffer[16];
const auto got=file.read(std::span<char>(buffer));
REQUIRE(got);
CHECK(std::string_view(buffer,got.value)=="hello pack");
// A NUL inside the path would name another file.
CHECK(VfsFile::open(host,std::string_view("data/b.txt\0x",12)).code==ZODIAC_E_BAD_PATH);
CHECK(VfsFile::exists(host,std::string_view("data/b.txt\0x",12)).code==ZODIAC_E_BAD_PATH);
// A path longer than the stack copy is copied too.
const std::string long_path="data/"+std::string(600,'n')+".txt";
CHECK(VfsFile::open(host,std::string_view(long_path)).code==ZODIAC_E_NOT_FOUND);
const auto exists=VfsFile::exists(host,std::string_view("DATA/A.TXT"));
CHECK((exists&&exists.value));
// Move assignment closes the file it held.
zodiac_vfs_file* first=file.handle();
auto other=VfsFile::open(host,"data/a.txt");
REQUIRE(other);
file=std::move(other.value);
CHECK(host->vfs_size(host,first)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(file.size().value==kBigSize);
CHECK(file.seek(-7,ZODIAC_SEEK_END).value==kBigSize-7);
CHECK(file.seek(1,ZODIAC_SEEK_END).code==ZODIAC_E_INVALID_ARGUMENT);
CHECK(file.position().value==kBigSize-7);
// A table without the VFS entries, and no host at all.
const std::uint32_t size=host->struct_size;
host->struct_size=static_cast<std::uint32_t>(offsetof(zodiac_host,vfs_open));
CHECK_FALSE(VfsFile::available(host));
CHECK(VfsFile::open(host,"data/a.txt").code==ZODIAC_E_UNAVAILABLE);
CHECK(VfsFile::exists(host,"data/a.txt").code==ZODIAC_E_UNAVAILABLE);
host->struct_size=size;
CHECK(VfsFile::open(nullptr,"data/a.txt").code==ZODIAC_E_INVALID_ARGUMENT);
VfsFile closed;
CHECK(closed.read(buffer,sizeof buffer).code==ZODIAC_E_INVALID_ARGUMENT);
CHECK(closed.seek(0).code==ZODIAC_E_INVALID_ARGUMENT);
CHECK(closed.size().code==ZODIAC_E_INVALID_ARGUMENT);
}

TEST_CASE("a handle of one host is refused by another host in the same process")
{
TempDir dir;
put_bytes(dir.path/"one"/"f.txt","from one");
put_bytes(dir.path/"two"/"f.txt","from two");
zodiac::vfs::Vfs vfs_one;
zodiac::vfs::Vfs vfs_two;
REQUIRE(vfs_one.mount_folder("/",dir.path/"one",false));
REQUIRE(vfs_two.mount_folder("/",dir.path/"two",false));
zodiac::Host one(nullptr);
zodiac::Host two(nullptr);
one.set_vfs(&vfs_one);
two.set_vfs(&vfs_two);
zodiac_host* a=one.abi();
zodiac_host* b=two.abi();
zodiac_vfs_file* from_a=nullptr;
zodiac_vfs_file* from_b=nullptr;
REQUIRE(a->vfs_open(a,"f.txt",&from_a)==ZODIAC_OK);
REQUIRE(b->vfs_open(b,"f.txt",&from_b)==ZODIAC_OK);
// Both are each table's first slot, under generations of their own.
CHECK(from_a!=from_b);
char buffer[16];
CHECK(b->vfs_read(b,from_a,buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(a->vfs_read(a,from_b,buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(b->vfs_size(b,from_a)==ZODIAC_E_INVALID_ARGUMENT);
b->vfs_close(b,from_a);
CHECK(a->vfs_read(a,from_a,buffer,sizeof buffer)==8);
CHECK(std::string_view(buffer,8)=="from one");
CHECK(b->vfs_read(b,from_b,buffer,sizeof buffer)==8);
CHECK(std::string_view(buffer,8)=="from two");
a->vfs_close(a,from_a);
b->vfs_close(b,from_b);
}

TEST_CASE("a module built against an older struct_size loads and registers next to one that reads the VFS")
{
Game game(R"(int main()
{
if(vfs_reader_old(41)!=42)return 1;
return vfs_read_all("data/b.txt")=="hello pack"?0:2;
})","\"vfs_reader_old\", \"vfs_reader\"");
CHECK(game.run()==0);
}

TEST_CASE("a plugin that checks struct_size sees a host table without the VFS entries")
{
Game game(R"(int main()
{
try{
vfs_read_all("data/b.txt");
}
catch{
return exception_info()=="the host has no VFS entries"?0:1;
}
return 2;
})","\"vfs_reader\"");
// The table of a host from before the VFS entries: it ends after log.
zodiac_host* host=game.host();
const std::uint32_t size=host->struct_size;
host->struct_size=static_cast<std::uint32_t>(offsetof(zodiac_host,vfs_open));
const int result=game.run();
host->struct_size=size;
CHECK(result==0);
}

TEST_CASE("bad, closed, double-closed and forged handles are refused without touching the host")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
CHECK(host->struct_size==sizeof(zodiac_host));
char buffer[16]={};
zodiac_vfs_file* file=nullptr;
REQUIRE(host->vfs_open(host,"data/b.txt",&file)==ZODIAC_OK);
const auto value=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(file));
const std::uint64_t index=value&0xffffffffu;
const std::uint64_t generation=value>>32;
// Null, small numbers, a slot never opened, an index past every segment, the next generation of the open slot, too large a
// generation, and every bit set.
for(const std::uint64_t bad:{std::uint64_t{0},std::uint64_t{1},std::uint64_t{0x1234},(std::uint64_t{1}<<32)|(index+5),(std::uint64_t{1}<<32)|0xffffffffu,((generation+1)<<32)|index,(std::uint64_t{1}<<63)|index,~std::uint64_t{0}}){
INFO(bad);
zodiac_vfs_file* handle=forged(bad);
CHECK(host->vfs_read(host,handle,buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,handle,0,ZODIAC_SEEK_SET)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_size(host,handle)==ZODIAC_E_INVALID_ARGUMENT);
host->vfs_close(host,handle);
}
// The real handle is untouched by all of that.
CHECK(host->vfs_read(host,file,buffer,sizeof buffer)==10);
CHECK(std::string_view(buffer,10)=="hello pack");
host->vfs_close(host,file);
CHECK(host->vfs_read(host,file,buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,file,0,ZODIAC_SEEK_SET)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_size(host,file)==ZODIAC_E_INVALID_ARGUMENT);
host->vfs_close(host,file);
host->vfs_close(host,file);
host->vfs_close(host,nullptr);
// The slot is used again under a new generation: the old handle stays refused, and closing it again leaves the new one open.
zodiac_vfs_file* again=nullptr;
REQUIRE(host->vfs_open(host,"data/a.txt",&again)==ZODIAC_OK);
CHECK((static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(again))&0xffffffffu)==index);
CHECK(again!=file);
CHECK(host->vfs_read(host,file,buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
host->vfs_close(host,file);
CHECK(host->vfs_size(host,again)==static_cast<std::int64_t>(kBigSize));
host->vfs_close(host,again);
// Null arguments.
CHECK(host->vfs_open(host,nullptr,&file)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(file==nullptr);
CHECK(host->vfs_open(host,"data/b.txt",nullptr)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_open(nullptr,"data/b.txt",&file)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_exists(host,nullptr)==ZODIAC_E_INVALID_ARGUMENT);
File open(host,"data/b.txt");
CHECK(host->vfs_read(host,open.file,nullptr,1)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_read(nullptr,open.file,buffer,1)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_read(host,open.file,nullptr,0)==0);
}

TEST_CASE("seeks are bounds-checked and reads stop at the end of the file")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
File big(host,"DATA/a.TXT");
CHECK(host->vfs_size(host,big.file)==static_cast<std::int64_t>(kBigSize));
std::string buffer(kBigSize+100,'\0');
// A read starting at 65535 for 3 bytes crosses a chunk edge.
CHECK(host->vfs_seek(host,big.file,65535,ZODIAC_SEEK_SET)==65535);
CHECK(host->vfs_read(host,big.file,buffer.data(),3)==3);
CHECK(buffer.substr(0,3)==game.big.substr(65535,3));
CHECK(host->vfs_seek(host,big.file,0,ZODIAC_SEEK_CUR)==65538);
CHECK(host->vfs_seek(host,big.file,-2,ZODIAC_SEEK_CUR)==65536);
CHECK(host->vfs_read(host,big.file,buffer.data(),5)==5);
CHECK(buffer.substr(0,5)==game.big.substr(65536,5));
// Out of range: negative, past the end, overflowing, an unknown origin; the position stays.
CHECK(host->vfs_seek(host,big.file,-1,ZODIAC_SEEK_SET)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,static_cast<std::int64_t>(kBigSize)+1,ZODIAC_SEEK_SET)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,1,ZODIAC_SEEK_END)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,-static_cast<std::int64_t>(kBigSize)-1,ZODIAC_SEEK_END)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,std::numeric_limits<std::int64_t>::max(),ZODIAC_SEEK_CUR)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,std::numeric_limits<std::int64_t>::min(),ZODIAC_SEEK_CUR)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,0,3)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,0,-1)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_seek(host,big.file,0,ZODIAC_SEEK_CUR)==65541);
// The end itself is a position; a read there gives 0, and so does a read of 0 bytes anywhere.
CHECK(host->vfs_seek(host,big.file,0,ZODIAC_SEEK_END)==static_cast<std::int64_t>(kBigSize));
CHECK(host->vfs_read(host,big.file,buffer.data(),buffer.size())==0);
CHECK(host->vfs_seek(host,big.file,-static_cast<std::int64_t>(kBigSize),ZODIAC_SEEK_END)==0);
CHECK(host->vfs_read(host,big.file,buffer.data(),0)==0);
CHECK(host->vfs_seek(host,big.file,0,ZODIAC_SEEK_CUR)==0);
// A buffer larger than the file gets the whole file in one read, three chunks and a part.
CHECK(host->vfs_read(host,big.file,buffer.data(),buffer.size())==static_cast<std::int64_t>(kBigSize));
CHECK(buffer.substr(0,kBigSize)==game.big);
CHECK(host->vfs_read(host,big.file,buffer.data(),buffer.size())==0);
// A seek to exactly 65536 and a read to the end.
CHECK(host->vfs_seek(host,big.file,65536,ZODIAC_SEEK_SET)==65536);
CHECK(host->vfs_read(host,big.file,buffer.data(),buffer.size())==static_cast<std::int64_t>(kBigSize-65536));
CHECK(buffer.substr(0,kBigSize-65536)==game.big.substr(65536));
}

TEST_CASE("a damaged chunk gives DAMAGED, moves nothing and delivers none of its bytes")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
File broken(host,"broken/c.bin");
std::string buffer(kBigSize,'\x7f');
// One read over every chunk fails as a whole; the damaged chunk's place holds none of its bytes.
CHECK(host->vfs_read(host,broken.file,buffer.data(),buffer.size())==ZODIAC_E_DAMAGED);
CHECK(host->vfs_seek(host,broken.file,0,ZODIAC_SEEK_CUR)==0);
CHECK(buffer.substr(kChunk,kChunk).find_first_not_of('\0')==std::string::npos);
// The good chunk before it reads.
CHECK(host->vfs_read(host,broken.file,buffer.data(),kChunk)==static_cast<std::int64_t>(kChunk));
CHECK(buffer.substr(0,kChunk)==game.broken.substr(0,kChunk));
// A part of the damaged chunk fails too and leaves the buffer as it was.
std::string part(10,'\x7f');
CHECK(host->vfs_seek(host,broken.file,kChunk+5,ZODIAC_SEEK_SET)==static_cast<std::int64_t>(kChunk+5));
CHECK(host->vfs_read(host,broken.file,part.data(),part.size())==ZODIAC_E_DAMAGED);
CHECK(part==std::string(10,'\x7f'));
CHECK(host->vfs_seek(host,broken.file,0,ZODIAC_SEEK_CUR)==static_cast<std::int64_t>(kChunk+5));
// The chunk after it reads.
CHECK(host->vfs_seek(host,broken.file,2*kChunk,ZODIAC_SEEK_SET)==static_cast<std::int64_t>(2*kChunk));
CHECK(host->vfs_read(host,broken.file,buffer.data(),buffer.size())==static_cast<std::int64_t>(kBigSize-2*kChunk));
CHECK(buffer.substr(0,kBigSize-2*kChunk)==game.broken.substr(2*kChunk));
}

TEST_CASE("vfs_exists answers 1 for a file, 0 for nothing or a folder, and a code for a bad path")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
CHECK(host->vfs_exists(host,"Data/A.txt")==1);
CHECK(host->vfs_exists(host,"data\\b.txt")==1);
CHECK(host->vfs_exists(host,"data/missing.txt")==0);
CHECK(host->vfs_exists(host,"data")==0);
CHECK(host->vfs_exists(host,"data/")==0);
CHECK(host->vfs_exists(host,"data/b.txt/inner")==0);
CHECK(host->vfs_exists(host,"../x")==ZODIAC_E_BAD_PATH);
CHECK(host->vfs_exists(host,"aux.txt")==ZODIAC_E_BAD_PATH);
CHECK(host->vfs_exists(host,"host:C:/x")==ZODIAC_E_BAD_PATH);
zodiac_vfs_file* file=nullptr;
CHECK(host->vfs_open(host,"data/missing.txt",&file)==ZODIAC_E_NOT_FOUND);
CHECK(file==nullptr);
// With host paths off (the default), neither a drive path nor a host: path opens anything.
for(const char* host_path:{"host:C:/x","host:/x","C:/x","C:\\Windows\\win.ini"}){
INFO(host_path);
CHECK(host->vfs_open(host,host_path,&file)==ZODIAC_E_BAD_PATH);
CHECK(file==nullptr);
}
}

TEST_CASE("a host without a VFS gives ZODIAC_E_UNAVAILABLE")
{
zodiac::Host bare(nullptr);
zodiac_host* host=bare.abi();
zodiac_vfs_file* file=forged(0x1234);
CHECK(host->vfs_open(host,"data/b.txt",&file)==ZODIAC_E_UNAVAILABLE);
CHECK(file==nullptr);
CHECK(host->vfs_exists(host,"data/b.txt")==ZODIAC_E_UNAVAILABLE);
char buffer[4];
CHECK(host->vfs_read(host,forged((std::uint64_t{1}<<32)|0),buffer,sizeof buffer)==ZODIAC_E_INVALID_ARGUMENT);
host->vfs_close(host,forged((std::uint64_t{1}<<32)|0));
}

TEST_CASE("different handles read one file on several threads at once")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
constexpr int kThreads=8;
std::atomic<int> wrong{0};
{
Threads threads;
for(int t=0;t<kThreads;++t){
threads.list.emplace_back([&,t]{
std::mt19937_64 random(static_cast<std::uint64_t>(t)+100);
zodiac_vfs_file* file=nullptr;
if(host->vfs_open(host,"data/a.txt",&file)!=ZODIAC_OK){
++wrong;
return;
}
std::string buffer(70000,'\0');
for(int i=0;i<200;++i){
const auto at=static_cast<std::int64_t>(random()%(kBigSize+1));
const auto size=static_cast<std::size_t>(random()%buffer.size());
if(host->vfs_seek(host,file,at,ZODIAC_SEEK_SET)!=at){
++wrong;
break;
}
const std::int64_t got=host->vfs_read(host,file,buffer.data(),size);
const std::size_t want=std::min<std::size_t>(size,kBigSize-static_cast<std::size_t>(at));
if(got!=static_cast<std::int64_t>(want)||std::memcmp(buffer.data(),game.big.data()+at,want)!=0){
++wrong;
break;
}
}
host->vfs_close(host,file);
});
}
}
CHECK(wrong.load()==0);
}

TEST_CASE("one handle shared by several threads is serialized: every byte is read exactly once")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
File shared(host,"words.bin");
constexpr int kThreads=8;
std::vector<std::vector<std::uint64_t>> seen(kThreads);
std::atomic<int> wrong{0};
{
Threads threads;
for(int t=0;t<kThreads;++t){
threads.list.emplace_back([&,t]{
// Whole words, so every read starts and ends on a word whatever ran before it.
std::string buffer(8*(1+static_cast<std::size_t>(t)*37),'\0');
for(;;){
const std::int64_t got=host->vfs_read(host,shared.file,buffer.data(),buffer.size());
if(got==0)break;
if(got<0||got%8){
++wrong;
break;
}
for(std::int64_t at=0;at<got;at+=8)seen[static_cast<std::size_t>(t)].push_back(word_at(buffer.data()+at));
}
});
}
}
CHECK(wrong.load()==0);
std::vector<std::uint64_t> all;
for(const auto& part:seen){
// Each thread's words go up: a read is one run of the file, after the runs other threads took before it.
CHECK(std::ranges::is_sorted(part));
all.insert(all.end(),part.begin(),part.end());
}
std::ranges::sort(all);
REQUIRE(all.size()==kWords);
for(std::uint64_t i=0;i<kWords;++i)if(all[i]!=i){
FAIL("word "<<i<<" was read as "<<all[i]);
break;
}
}

TEST_CASE("a handle closed while other threads use it is refused from then on, also after its slot is reused")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
zodiac_vfs_file* file=nullptr;
REQUIRE(host->vfs_open(host,"words.bin",&file)==ZODIAC_OK);
constexpr int kThreads=4;
std::atomic<int> wrong{0};
std::atomic<int> refused{0};
std::atomic<std::uint64_t> reads{0};
zodiac_vfs_file* reused=nullptr;
{
Threads threads;
for(int t=0;t<kThreads;++t){
threads.list.emplace_back([&,t]{
char buffer[64];
// Until the close: words.bin's words (another thread may move the position between the seek and the read).
for(std::int64_t i=t;;++i){
const std::int64_t at=host->vfs_seek(host,file,(i%1000)*8,ZODIAC_SEEK_SET);
const std::int64_t got=host->vfs_read(host,file,buffer,sizeof buffer);
if(got==ZODIAC_E_INVALID_ARGUMENT)break;
if(got!=static_cast<std::int64_t>(sizeof buffer)||at<0||word_at(buffer)>=kWords){
++wrong;
return;
}
++reads;
}
// From the first refusal on, the handle never reads again, not even the file its reused slot now holds.
char other[64];
for(int i=0;i<2000;++i){
if(host->vfs_seek(host,file,0,ZODIAC_SEEK_SET)!=ZODIAC_E_INVALID_ARGUMENT||host->vfs_read(host,file,other,sizeof other)!=ZODIAC_E_INVALID_ARGUMENT||host->vfs_size(host,file)!=ZODIAC_E_INVALID_ARGUMENT){
++wrong;
return;
}
}
++refused;
});
}
while(reads.load()<4000&&wrong.load()==0)std::this_thread::yield();
host->vfs_close(host,file);
// The slot is used again for another file at once, while the threads still call with the closed handle.
REQUIRE(host->vfs_open(host,"data/a.txt",&reused)==ZODIAC_OK);
CHECK(reused!=file);
threads.join();
}
CHECK(wrong.load()==0);
CHECK(refused.load()==kThreads);
CHECK(host->vfs_size(host,file)==ZODIAC_E_INVALID_ARGUMENT);
CHECK(host->vfs_size(host,reused)==static_cast<std::int64_t>(kBigSize));
host->vfs_close(host,reused);
}

TEST_CASE("a file opened through a pack keeps reading after the pack is unmounted")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
File big(host,"data/a.txt");
CHECK(game.runtime->vfs().unmount(&owner_game)==1);
CHECK(host->vfs_exists(host,"data/a.txt")==0);
std::string buffer(kBigSize,'\0');
CHECK(host->vfs_read(host,big.file,buffer.data(),buffer.size())==static_cast<std::int64_t>(kBigSize));
CHECK(buffer==game.big);
}

TEST_CASE("many handles at once each read their file, and closed slots are used again")
{
Game game(kNoScript,"");
zodiac_host* host=game.host();
// 1000 handles fill the segments of 64, 128, 256 and 512 slots.
constexpr std::size_t kHandles=1000;
for(int round=0;round<2;++round){
std::vector<zodiac_vfs_file*> files(kHandles,nullptr);
for(zodiac_vfs_file*& file:files)REQUIRE(host->vfs_open(host,"data/b.txt",&file)==ZODIAC_OK);
CHECK(std::set<zodiac_vfs_file*>(files.begin(),files.end()).size()==kHandles);
std::size_t highest=0;
for(zodiac_vfs_file* file:files){
char buffer[16];
CHECK(host->vfs_read(host,file,buffer,sizeof buffer)==10);
CHECK(std::string_view(buffer,10)=="hello pack");
highest=std::max<std::size_t>(highest,static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(file))&0xffffffffu);
}
// The second round takes the slots the first one freed.
CHECK(highest<kHandles);
for(zodiac_vfs_file* file:files)host->vfs_close(host,file);
}
}
