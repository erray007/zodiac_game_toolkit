// Calls zodiac::release::start directly with payloads made from fixture projects (build + save_bytecode), using a temporary folder as the executable's folder.
// The release pack cases lay game.exe out as zodiac build does (filler for the linked image, the pack, its trailer) or put <game>.zpk beside it.
// start() writes its diagnostics to the console sink, so standard error is redirected to a file while it runs.
#include <core/diagnostics.hpp>
#include <core/module_registry.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/pack/release_pack.hpp>
#include <core/payload.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <doctest/doctest.h>
#include <platform/console.hpp>
#include <platform/file.hpp>
#include <release/stub.hpp>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
namespace fs=std::filesystem;
// Runs before doctest starts so a failing module load can never raise a Windows modal dialog.
const int g_dialogs_disabled=(zodiac::platform::disable_error_dialogs(),0);
fs::path from_utf8(std::string_view text)
{
return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
std::string to_utf8(const fs::path& path)
{
const std::u8string text=path.generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
zodiac::ModuleSearchPaths search(){return zodiac::ModuleSearchPaths{{from_utf8(ZODIAC_TEST_MODULE_DIR),from_utf8(ZODIAC_TEST_FIXTURE_DIR)}};}
std::string module_file(std::string_view name){return "zodiac_mod_"+std::string(name)+ZODIAC_TEST_MODULE_SUFFIX;}
// A fresh directory under the system temp dir, removed on destruction.
class TempDir{
public:
TempDir()
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/("zodiac_stub_test_"+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
fs::create_directories(dir_);
}
~TempDir()
{
std::error_code ec;
fs::remove_all(dir_,ec);
}
TempDir(const TempDir&)=delete;
TempDir& operator=(const TempDir&)=delete;
const fs::path& dir() const{return dir_;}
private:
fs::path dir_;
};
// start() changes the working directory; it is restored so the temporary folders can be removed.
class WorkingDirectoryGuard{
public:
WorkingDirectoryGuard():saved_(fs::current_path()){}
~WorkingDirectoryGuard()
{
std::error_code ec;
fs::current_path(saved_,ec);
}
WorkingDirectoryGuard(const WorkingDirectoryGuard&)=delete;
WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&)=delete;
private:
fs::path saved_;
};
std::string read_file(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void write_file(const fs::path& path,std::string_view text)
{
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
REQUIRE(out.good());
}
std::vector<std::string> split_lines(std::string_view text)
{
std::vector<std::string> lines;
while(!text.empty()){
const std::size_t end=text.find('\n');
lines.emplace_back(text.substr(0,end));
if(end==std::string_view::npos)break;
text.remove_prefix(end+1);
}
return lines;
}
// Points file descriptor 2 at a file for the lifetime of the object, so the real ConsoleSink output can be inspected.
class StderrCapture{
public:
explicit StderrCapture(const fs::path& file):file_(file)
{
std::fflush(stderr);
std::FILE* target=std::fopen(file.string().c_str(),"wb");
REQUIRE(target!=nullptr);
saved_=dup(2);
REQUIRE(saved_>=0);
REQUIRE(dup2(fileno(target),2)>=0);
std::fclose(target);
}
~StderrCapture(){restore();}
StderrCapture(const StderrCapture&)=delete;
StderrCapture& operator=(const StderrCapture&)=delete;
std::vector<std::string> lines()
{
restore();
return split_lines(read_file(file_));
}
private:
void restore()
{
if(saved_<0)return;
std::fflush(stderr);
dup2(saved_,2);
close(saved_);
saved_=-1;
}
fs::path file_;
int saved_=-1;
};
// Builds the project in dir like zodiac build will: requested names in load order, the registered modules with their versions and link modes, the main script's bytecode.
zodiac::GamePayload payload_of(const fs::path& dir)
{
auto project=zodiac::load_project(dir);
INFO(project.error_or(""));
REQUIRE(project.has_value());
zodiac::Diagnostics diagnostics;
zodiac::ScriptRuntime runtime(diagnostics,search());
REQUIRE(runtime.build(*project)==0);
auto bytecode=runtime.save_bytecode(false);
INFO(bytecode.error_or(""));
REQUIRE(bytecode.has_value());
zodiac::GamePayload payload;
payload.kit_version="0.1.0";
payload.game_name=project->name;
payload.requested=project->static_modules;
for(const std::string& name:project->dynamic_modules)if(std::ranges::find(payload.requested,name)==payload.requested.end())payload.requested.push_back(name);
for(const zodiac_module_info* info:runtime.modules()){
const bool is_static=std::ranges::find(project->static_modules,std::string_view(info->name))!=project->static_modules.end();
payload.modules.push_back({info->name,info->version,is_static?zodiac::LinkMode::static_link:zodiac::LinkMode::dynamic_link});
}
payload.core_exclude=project->core_exclude;
payload.host_paths=project->files.host_paths;
payload.scripts.push_back({to_utf8(project->main_script.lexically_relative(project->root)),true,std::move(*bytecode)});
return payload;
}
zodiac::GamePayload fixture_payload(std::string_view name){return payload_of(from_utf8(ZODIAC_TEST_SCRIPT_DIR)/from_utf8(name));}
// Writes a project with the given [modules] dynamic list and main.zs into a fresh folder and returns its payload.
zodiac::GamePayload source_payload(std::string_view dynamic,std::string_view script)
{
const TempDir source;
write_file(source.dir()/"zodiac.toml","[game]\nmain = \"main.zs\"\n[modules]\ndynamic = ["+std::string(dynamic)+"]\n");
write_file(source.dir()/"main.zs",script);
return payload_of(source.dir());
}
void copy_module(std::string_view name,const fs::path& from,const fs::path& to)
{
fs::create_directories(to);
fs::copy_file(from/module_file(name),to/module_file(name),fs::copy_options::overwrite_existing);
}
struct Started{
int code=-1;
std::vector<std::string> lines;
};
// Runs start() with folder/game.exe as the executable and the serialized payload (or the raw bytes given).
Started start_in(const fs::path& folder,std::string_view bytes)
{
const TempDir capture_dir;
const WorkingDirectoryGuard guard;
Started started;
StderrCapture capture(capture_dir.dir()/"stderr.txt");
started.code=zodiac::release::start(bytes,folder/"game.exe");
started.lines=capture.lines();
return started;
}
Started start_in(const fs::path& folder,const zodiac::GamePayload& payload){return start_in(folder,zodiac::serialize_payload(payload));}
std::string join(const std::vector<std::string>& lines)
{
std::string text;
for(const std::string& line:lines)text.append(line).push_back('\n');
return text;
}
// Sets an environment variable for the test's duration, restoring the old value.
class ScopedEnv{
public:
ScopedEnv(const char* name,const fs::path& value):name_(name)
{
#ifdef _WIN32
// The wide value: the narrow one is in the ANSI code page, which cannot hold every folder name.
const std::wstring wide(name_.begin(),name_.end());
if(const wchar_t* old=_wgetenv(wide.c_str()))old_=fs::path(old);
_wputenv_s(wide.c_str(),value.c_str());
#else
if(const char* old=std::getenv(name))old_=fs::path(old);
setenv(name,value.c_str(),1);
#endif
}
~ScopedEnv()
{
#ifdef _WIN32
const std::wstring wide(name_.begin(),name_.end());
_wputenv_s(wide.c_str(),old_?old_->c_str():L"");
#else
if(old_)setenv(name_.c_str(),old_->c_str(),1);
else unsetenv(name_.c_str());
#endif
}
ScopedEnv(const ScopedEnv&)=delete;
ScopedEnv& operator=(const ScopedEnv&)=delete;
private:
std::string name_;
std::optional<fs::path> old_;
};
#ifdef _WIN32
constexpr const char* data_variable="APPDATA";
#else
constexpr const char* data_variable="XDG_DATA_HOME";
#endif
}

TEST_CASE("start runs the payload's main")
{
const TempDir folder;
const Started started=start_in(folder.dir(),fixture_payload("int_main"));
INFO(join(started.lines));
CHECK(started.code==7);
CHECK(started.lines.empty());
}

TEST_CASE("start refuses a damaged payload")
{
const TempDir folder;
std::string bytes=zodiac::serialize_payload(fixture_payload("int_main"));
bytes[bytes.size()/2]=static_cast<char>(bytes[bytes.size()/2]^0x01);
const Started started=start_in(folder.dir(),bytes);
CHECK(started.code==2);
REQUIRE(started.lines.size()==1);
CHECK(started.lines[0].starts_with("error: the game data is damaged"));
}

TEST_CASE("start refuses a missing dynamic module")
{
const TempDir folder;
zodiac::GamePayload payload=fixture_payload("uses_module");
REQUIRE(payload.modules.size()==1);
payload.modules[0].link=zodiac::LinkMode::dynamic_link;
// A release loads dynamic modules from the executable's folder only, never from plugins/ or the working directory.
copy_module("example",from_utf8(ZODIAC_TEST_MODULE_DIR),folder.dir()/"plugins");
const Started started=start_in(folder.dir(),payload);
CHECK(started.code==2);
REQUIRE(started.lines.size()==1);
CHECK(started.lines[0]=="error: module \"example\" is missing");
}

TEST_CASE("start refuses a static module that is not linked in")
{
const TempDir folder;
zodiac::GamePayload payload=source_payload("\"cwd_probe\"","int main(){return cwd().length()>0?0:3;}\n");
REQUIRE(payload.modules.size()==1);
payload.modules[0].link=zodiac::LinkMode::static_link;
// The shared library next to the executable must not stand in for a static module.
copy_module("cwd_probe",from_utf8(ZODIAC_TEST_FIXTURE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
CHECK(started.code==2);
REQUIRE(started.lines.size()==1);
CHECK(started.lines[0]=="error: module \"cwd_probe\" is missing");
}

#ifdef ZODIAC_TEST_STATIC_EXAMPLE
TEST_CASE("start registers a static module from the records linked into the executable")
{
const TempDir folder;
const zodiac::GamePayload payload=fixture_payload("uses_module");
REQUIRE(payload.modules.size()==1);
CHECK(payload.modules[0].link==zodiac::LinkMode::static_link);
// No shared library anywhere near the executable: example comes from zodiac_mod_example_static, forced in with -u.
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
// uses_module returns example_add(2,3)+example_echo("ab").length().
CHECK(started.code==7);
CHECK(started.lines.empty());
}
#endif

TEST_CASE("start_linked checks the stored size against the linked section")
{
const TempDir folder;
const std::string payload=zodiac::serialize_payload(fixture_payload("int_main"));
std::string section(8,'\0');
for(std::size_t i=0;i<8;++i)section[i]=static_cast<char>((static_cast<std::uint64_t>(payload.size())>>(8*i))&0xFFu);
section.append(payload);
const auto* begin=reinterpret_cast<const unsigned char*>(section.data());
const auto run=[&folder](const unsigned char* first,const unsigned char* last){
const TempDir capture_dir;
const WorkingDirectoryGuard guard;
Started started;
StderrCapture capture(capture_dir.dir()/"stderr.txt");
started.code=zodiac::release::start_linked(first,last,folder.dir()/"game.exe");
started.lines=capture.lines();
return started;
};
const Started intact=run(begin,begin+section.size());
INFO(join(intact.lines));
CHECK(intact.code==7);
const std::vector<std::string> damaged{"error: the game data is damaged (size mismatch)"};
// A stored size larger than the section (a tampered length field) is refused without reading past the end.
Started started=run(begin,begin+section.size()-1);
CHECK(started.code==2);
CHECK(started.lines==damaged);
section[0]=static_cast<char>(section[0]+1);
started=run(begin,begin+section.size());
CHECK(started.code==2);
CHECK(started.lines==damaged);
// A section too short to hold the size field.
started=run(begin,begin+7);
CHECK(started.code==2);
CHECK(started.lines==damaged);
}

TEST_CASE("start refuses a different module version")
{
const TempDir folder;
zodiac::GamePayload payload=fixture_payload("uses_module");
REQUIRE(payload.modules.size()==1);
payload.modules[0].link=zodiac::LinkMode::dynamic_link;
payload.modules[0].version="9.9.9";
payload.diagnostics.file="logs/game.log";
copy_module("example",from_utf8(ZODIAC_TEST_MODULE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
const std::string expected="error: module \"example\" is version 0.1.0, but the game was built with 9.9.9";
CHECK(started.code==2);
CHECK(started.lines==std::vector<std::string>{expected});
// The log file is relative to the executable's folder.
CHECK(read_file(folder.dir()/"logs"/"game.log")==expected+"\n");
}

TEST_CASE("start refuses modules registered in a different order")
{
const TempDir folder;
zodiac::GamePayload payload=source_payload("\"dep_child\"","int main(){return 0;}\n");
REQUIRE(payload.modules.size()==2);
CHECK(payload.modules[0].name=="example");
CHECK(payload.modules[1].name=="dep_child");
std::ranges::reverse(payload.modules);
copy_module("example",from_utf8(ZODIAC_TEST_MODULE_DIR),folder.dir());
copy_module("dep_child",from_utf8(ZODIAC_TEST_FIXTURE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
CHECK(started.code==2);
// The check runs before any module registers, so dep_child logs nothing.
CHECK(started.lines==std::vector<std::string>{"error: the modules were registered in a different order than when the game was built"});
}

TEST_CASE("start refuses a module whose interface changed without a new version")
{
const TempDir folder;
// The payload stores example 0.1.0, as the DLL next to the executable reports, but its bytecode calls a function that
// example does not register: what a DLL replaced by one with the same version and another interface looks like.
zodiac::GamePayload payload=source_payload("\"example\"","int main(){return example_add(2,3);}\n");
const zodiac::GamePayload other=source_payload("\"cwd_probe\"","int main(){return cwd().length()>0?0:3;}\n");
payload.scripts=other.scripts;
copy_module("example",from_utf8(ZODIAC_TEST_MODULE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
CHECK(started.code==2);
CHECK(started.lines==std::vector<std::string>{"error: the game data does not match the modules it was built with"});
}

TEST_CASE("a global initializer failure while the bytecode loads is a script error")
{
const TempDir folder;
const std::string expected=to_utf8(fs::canonical(folder.dir()));
// The initializer only fails in the executable's folder, so the build that makes the payload succeeds.
const zodiac::GamePayload payload=source_payload("\"cwd_probe\"","array<int> g_values;\nint g_first=cwd()==\""+expected+"\"?g_values[3]:0;\nint main(){return 0;}\n");
copy_module("cwd_probe",from_utf8(ZODIAC_TEST_FIXTURE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
CHECK(started.code==1);
REQUIRE_FALSE(started.lines.empty());
CHECK(started.lines[0]=="main.zs:2:5: error: Failed to initialize global variable 'g_first'");
CHECK(std::ranges::none_of(started.lines,[](const std::string& line){return line.contains("does not match");}));
}

TEST_CASE("start runs a dynamic module from the executable's folder and keeps the module order")
{
const TempDir folder;
const zodiac::GamePayload payload=source_payload("\"dep_child\"","int main(){return 5;}\n");
copy_module("example",from_utf8(ZODIAC_TEST_MODULE_DIR),folder.dir());
copy_module("dep_child",from_utf8(ZODIAC_TEST_FIXTURE_DIR),folder.dir());
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
CHECK(started.code==5);
// dep_child logs while registering and shutting down; example logs nothing.
CHECK(started.lines==std::vector<std::string>{"dep_child register","dep_child shutdown"});
}

TEST_CASE("start sets the working directory to the executable folder")
{
const TempDir base;
const fs::path folder=base.dir()/from_utf8("oyun klas\xC3\xB6r\xC3\xBC");
fs::create_directories(folder);
const std::string expected=to_utf8(fs::canonical(folder));
// The global is initialised while the bytecode loads, so the working directory is already set then (zodiac run sets it before the build).
const zodiac::GamePayload payload=source_payload("\"cwd_probe\"","string g_start=cwd();\nint main(){if(g_start!=\""+expected+"\")return 4;return cwd()==\""+expected+"\"?0:3;}\n");
copy_module("cwd_probe",from_utf8(ZODIAC_TEST_FIXTURE_DIR),folder);
const Started started=start_in(folder,payload);
INFO(join(started.lines));
CHECK(started.code==0);
CHECK(started.lines.empty());
}

TEST_CASE("start clamps the exit code like zodiac run")
{
const TempDir folder;
const Started started=start_in(folder.dir(),source_payload("","int main(){return 256;}\n"));
CHECK(started.code==1);
CHECK(started.lines==std::vector<std::string>{"warning: main() returned 256; exit codes must be 0-255, using 1"});
}

TEST_CASE("start lets the script open host paths exactly when the payload says [files] host_paths")
{
const TempDir folder;
const TempDir outside;
write_file(outside.dir()/"host.txt","from the host");
const std::string path="host:"+to_utf8(outside.dir()/"host.txt");
const std::string script="int main(){if(!file_exists(\""+path+"\"))return 3;return read_file(\""+path+"\")==\"from the host\"?0:4;}\n";
zodiac::GamePayload payload;
{
const TempDir source;
write_file(source.dir()/"zodiac.toml","[game]\nmain = \"main.zs\"\n[files]\nhost_paths = true\n");
write_file(source.dir()/"main.zs",script);
payload=payload_of(source.dir());
}
CHECK(payload.host_paths);
const Started allowed=start_in(folder.dir(),payload);
INFO(join(allowed.lines));
CHECK(allowed.code==0);
CHECK(allowed.lines.empty());
payload.host_paths=false;
const Started refused=start_in(folder.dir(),payload);
INFO(join(refused.lines));
CHECK(refused.code==3);
}

TEST_CASE("start mounts the executable folder and data:/")
{
const TempDir folder;
const TempDir user_data;
const ScopedEnv data_home(data_variable,user_data.dir());
write_file(folder.dir()/"a.txt","from the game folder");
zodiac::GamePayload payload;
{
const TempDir source;
write_file(source.dir()/"zodiac.toml","[game]\nname = \"stub_game\"\nmain = \"main.zs\"\n");
write_file(source.dir()/"main.zs","int main(){string text=read_file(\"a.txt\"); if(!write_file(\"data:/s.txt\", text+\"!\"))return 3; return file_exists(\"data:/S.TXT\")?0:4;}\n");
payload=payload_of(source.dir());
}
REQUIRE(payload.game_name=="stub_game");
// Nothing is made in the data folder before the first write.
CHECK_FALSE(fs::exists(user_data.dir()/"stub_game"));
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
CHECK(started.code==0);
CHECK(started.lines.empty());
CHECK(read_file(user_data.dir()/"stub_game"/"s.txt")=="from the game folder!");
}

namespace {
zodiac::crypto::Key release_key()
{
zodiac::crypto::Key key;
for(std::size_t i=0;i<key.size();++i)key[i]=static_cast<std::uint8_t>(0x5Au^(i*29));
return key;
}
// A release pack holding entries (name, bytes) under key, as bytes.
std::string release_pack(const std::vector<std::pair<std::string,std::string>>& entries,const zodiac::crypto::Key& key)
{
const TempDir dir;
const fs::path path=dir.dir()/"build.zpk";
{
auto storage=zodiac::pack::PackStorage::open(path,zodiac::pack::KeySource(key),true);
REQUIRE(storage);
for(const auto& [name,data]:entries)REQUIRE((*storage)->add(name,std::as_bytes(std::span(data)),0));
REQUIRE((*storage)->close());
}
return read_file(path);
}
// payload set up for a pack: the game name "packed", the mode, the key and writable.
void set_pack(zodiac::GamePayload& payload,zodiac::PackMode mode,const zodiac::crypto::Key& key,bool writable=false)
{
payload.game_name="packed";
payload.pack_mode=mode;
payload.pack_writable=writable;
payload.pack_key.assign(reinterpret_cast<const char*>(key.data()),key.size());
}
// folder/game.exe as zodiac build lays out an embedded pack: 4096 bytes of filler (the linked image), the pack, alignment, the trailer.
void write_embedded(const fs::path& folder,std::string_view pack_bytes,std::uint32_t payload_crc)
{
std::string image(4096,'\x90');
image.append(pack_bytes);
while((image.size()+zodiac::pack::kTrailerSize)%zodiac::pack::kTrailerAlignment)image.push_back('\0');
image.append(zodiac::pack::trailer_bytes(4096,pack_bytes.size(),payload_crc));
write_file(folder/"game.exe",image);
}
constexpr const char* kPackedScript="int main(){if(read_file(\"data/a.txt\")!=\"from the pack\")return 3;return file_exists(\"DATA/A.TXT\")?0:4;}\n";
}

TEST_CASE("start runs a game whose pack is embedded")
{
const TempDir folder;
const zodiac::crypto::Key key=release_key();
zodiac::GamePayload payload=source_payload("",kPackedScript);
set_pack(payload,zodiac::PackMode::embedded,key);
const std::string bytes=zodiac::serialize_payload(payload);
const std::string pack_bytes=release_pack({{"data/a.txt","from the pack"}},key);
write_embedded(folder.dir(),pack_bytes,zodiac::pack::payload_checksum(bytes));
// The pack lies in its window at offset 4096 of the executable; the file around it is no pack.
{
auto file=zodiac::platform::File::open(folder.dir()/"game.exe",zodiac::platform::FileAccess::read);
REQUIRE(file);
const auto location=zodiac::pack::find_embedded_pack(*file,zodiac::pack::payload_checksum(bytes));
REQUIRE(location);
CHECK(location->offset==4096);
CHECK(location->size==pack_bytes.size());
}
const Started started=start_in(folder.dir(),bytes);
INFO(join(started.lines));
CHECK(started.code==0);
CHECK(started.lines.empty());
// Nothing was unpacked into the game folder.
CHECK_FALSE(fs::exists(folder.dir()/"data"));
}

TEST_CASE("start refuses an embedded pack of another build, of another key or with no trailer")
{
const TempDir folder;
const zodiac::crypto::Key key=release_key();
zodiac::GamePayload payload=source_payload("",kPackedScript);
set_pack(payload,zodiac::PackMode::embedded,key);
const std::string bytes=zodiac::serialize_payload(payload);
const std::vector<std::string> damaged{"error: the game data is damaged (pack: wrong key or damaged pack)"};
const std::string pack_bytes=release_pack({{"data/a.txt","from the pack"}},key);
// Another build's payload.
write_embedded(folder.dir(),pack_bytes,zodiac::pack::payload_checksum(bytes)^1u);
Started started=start_in(folder.dir(),bytes);
CHECK(started.code==2);
CHECK(started.lines==damaged);
// Another key.
zodiac::crypto::Key other=key;
other[0]^=0x01;
write_embedded(folder.dir(),release_pack({{"data/a.txt","from the pack"}},other),zodiac::pack::payload_checksum(bytes));
started=start_in(folder.dir(),bytes);
CHECK(started.code==2);
CHECK(started.lines==damaged);
// No trailer at all.
write_file(folder.dir()/"game.exe",std::string(4096,'\x90'));
started=start_in(folder.dir(),bytes);
CHECK(started.code==2);
CHECK(started.lines==damaged);
// A pack mode without a key, and writable without external, are contradictions no build writes.
zodiac::GamePayload keyless=payload;
keyless.pack_key.clear();
started=start_in(folder.dir(),keyless);
CHECK(started.code==2);
CHECK(started.lines==std::vector<std::string>{"error: the game data is damaged (invalid pack key)"});
zodiac::GamePayload writable=payload;
writable.pack_writable=true;
started=start_in(folder.dir(),writable);
CHECK(started.code==2);
CHECK(started.lines==std::vector<std::string>{"error: the game data is damaged (invalid pack or file setting)"});
}

TEST_CASE("start reads an external pack")
{
const TempDir folder;
const zodiac::crypto::Key key=release_key();
zodiac::GamePayload payload=source_payload("",kPackedScript);
set_pack(payload,zodiac::PackMode::external,key);
write_file(folder.dir()/"packed.zpk",release_pack({{"data/a.txt","from the pack"}},key));
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
CHECK(started.code==0);
CHECK(started.lines.empty());
// Read only, so a script's write to a packed name fails with the read-only pack text, and the pack is unchanged.
zodiac::GamePayload writing=source_payload("","int main(){if(write_file(\"data/a.txt\",\"x\"))return 3;return last_file_error()==\"\\\"data/a.txt\\\" is in a read-only pack; save files in data:/ instead\"?0:4;}\n");
set_pack(writing,zodiac::PackMode::external,key);
const std::string before=read_file(folder.dir()/"packed.zpk");
const Started refused=start_in(folder.dir(),writing);
INFO(join(refused.lines));
CHECK(refused.code==0);
CHECK(read_file(folder.dir()/"packed.zpk")==before);
}

TEST_CASE("start refuses a missing external pack")
{
const TempDir folder;
zodiac::GamePayload payload=source_payload("",kPackedScript);
set_pack(payload,zodiac::PackMode::external,release_key());
const Started started=start_in(folder.dir(),payload);
CHECK(started.code==2);
CHECK(started.lines==std::vector<std::string>{"error: the game data is damaged (pack \"packed.zpk\" is missing)"});
// The writable one too.
payload.pack_writable=true;
const Started writable=start_in(folder.dir(),payload);
CHECK(writable.code==2);
CHECK(writable.lines==std::vector<std::string>{"error: the game data is damaged (pack \"packed.zpk\" is missing)"});
}

TEST_CASE("start mounts the pack before global initializers run")
{
const TempDir folder;
const zodiac::crypto::Key key=release_key();
// The build that makes the payload runs the initializer too, with nothing mounted: file_exists keeps it from throwing there.
zodiac::GamePayload payload=source_payload("","string g=file_exists(\"data/a.txt\")?read_file(\"data/a.txt\"):\"\";\nint main(){return g==\"from the pack\"?0:3;}\n");
set_pack(payload,zodiac::PackMode::external,key);
write_file(folder.dir()/"packed.zpk",release_pack({{"data/a.txt","from the pack"}},key));
const Started started=start_in(folder.dir(),payload);
INFO(join(started.lines));
CHECK(started.code==0);
CHECK(started.lines.empty());
}

TEST_CASE("a writable release pack keeps a script's write across two starts")
{
const TempDir folder;
const zodiac::crypto::Key key=release_key();
zodiac::GamePayload payload=source_payload("","int main(){if(file_exists(\"save.txt\"))return read_file(\"save.txt\")==\"level 2\"?10:11;return write_file(\"save.txt\",\"level 2\")?0:12;}\n");
set_pack(payload,zodiac::PackMode::external,key,true);
write_file(folder.dir()/"packed.zpk",release_pack({{"data/a.txt","from the pack"}},key));
const Started first=start_in(folder.dir(),payload);
INFO(join(first.lines));
CHECK(first.code==0);
// The write went into the pack, not into the game folder.
CHECK_FALSE(fs::exists(folder.dir()/"save.txt"));
const Started second=start_in(folder.dir(),payload);
INFO(join(second.lines));
CHECK(second.code==10);
CHECK(second.lines.empty());
}
