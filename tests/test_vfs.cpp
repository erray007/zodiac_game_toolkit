// The virtual file system: path rules (Review Focus 2), case-insensitive folders (Review Focus 1), the mount table under concurrent
// lookups, host paths and the start-up mounts. Every test mounts temporary folders explicitly; nothing outside them is written.
#include <core/path_utf8.hpp>
#include <core/project_file.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/path.hpp>
#include <core/vfs/startup.hpp>
#include <core/vfs/vfs.hpp>
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {
namespace fs=std::filesystem;
using zodiac::path_from_utf8;
using zodiac::vfs::EntryKind;
using zodiac::vfs::FileError;
using zodiac::vfs::FolderSource;
using zodiac::vfs::ListKind;
using zodiac::vfs::Vfs;
using zodiac::vfs::WriteMode;
// A fresh folder with a non-ASCII name under the system temp folder, removed on destruction.
class TempDir{
public:
TempDir()
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/path_from_utf8("zodiac vfs \xC5\x9F "+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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
fs::path at(std::string_view relative) const{return dir_/path_from_utf8(relative);}
void write(std::string_view relative,std::string_view text) const
{
const fs::path target=at(relative);
fs::create_directories(target.parent_path());
std::ofstream out(target,std::ios::binary);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
}
private:
fs::path dir_;
};
std::string generic_utf8(const fs::path& path)
{
const std::u8string text=path.generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
std::string read_disk(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
// The whole file through the Vfs, or nullopt with the failure's code in error.
std::optional<std::string> read_vfs(Vfs& vfs,std::string_view path,FileError* error=nullptr)
{
auto reader=vfs.open_read(path);
if(!reader){
if(error)*error=reader.error().code;
return std::nullopt;
}
std::string text(static_cast<std::size_t>((*reader)->size()),'\0');
const std::int64_t got=(*reader)->read_at(0,std::as_writable_bytes(std::span(text)));
if(got<0)return std::nullopt;
text.resize(static_cast<std::size_t>(got));
return text;
}
bool write_vfs(Vfs& vfs,std::string_view path,std::string_view text)
{
auto target=vfs.open_write(path,WriteMode::truncate);
if(!target)return false;
return (*target)->write_at(0,std::as_bytes(std::span(text)))&&(*target)->commit().has_value();
}
std::string parse_error(std::string_view path,bool host_paths=false)
{
const auto parsed=zodiac::vfs::parse_path(path,host_paths);
REQUIRE_FALSE(parsed.has_value());
return parsed.error();
}
// Collects the Vfs's log lines.
struct LogLines{
std::vector<std::string> lines;
void attach(Vfs& vfs){vfs.set_log([this](zodiac_log_level,std::string_view line){lines.emplace_back(line);});}
};
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

TEST_CASE("parse_path normalizes separators, dots and mounts")
{
using zodiac::vfs::parse_path;
const auto dots=parse_path("a/./b/../c.txt",false);
REQUIRE(dots);
CHECK(dots->mount=="");
CHECK(dots->relative=="a/c.txt");
CHECK_FALSE(dots->folder);
const auto rooted=parse_path("/sounds/a.ogg",false);
REQUIRE(rooted);
CHECK(rooted->mount=="");
CHECK(rooted->relative=="sounds/a.ogg");
const auto data=parse_path("data:/saves/s1.txt",false);
REQUIRE(data);
CHECK(data->mount=="data:");
CHECK(data->relative=="saves/s1.txt");
const auto doubled=parse_path("a//b",false);
REQUIRE(doubled);
CHECK(doubled->relative=="a/b");
// Review Focus 2: BGT-style backslashes.
const auto backslash=parse_path("sounds\\shot.ogg",false);
REQUIRE(backslash);
CHECK(backslash->relative=="sounds/shot.ogg");
for(const auto& [text,mount,relative]:{std::tuple{"","",""},std::tuple{"/","",""},std::tuple{"data:/","data:",""},std::tuple{"sounds/","","sounds"},std::tuple{"a/b/..","","a"},std::tuple{"data:","data:",""}}){
CAPTURE(text);
const auto folder=parse_path(text,false);
REQUIRE(folder);
CHECK(folder->folder);
CHECK(folder->mount==mount);
CHECK(folder->relative==relative);
}
}

TEST_CASE("parse_path refuses leaving a mount")
{
CHECK(parse_error("../x")=="\"../x\" leaves its mount");
CHECK(parse_error("data:/../x")=="\"data:/../x\" leaves its mount");
CHECK(parse_error("a/../../x")=="\"a/../../x\" leaves its mount");
}

TEST_CASE("host paths are refused unless enabled")
{
for(const std::string path:{"C:/x","c:\\x","//srv/x","\\\\srv\\x","host:/etc/x","HOST:/etc/x"}){
CAPTURE(path);
CHECK(parse_error(path)=="\""+path+"\" is a host path; set [files] host_paths = true to use it");
}
const TempDir temp;
const std::string generic=generic_utf8(temp.dir());
const auto host=zodiac::vfs::parse_path("host:"+generic+"/x.txt",true);
REQUIRE(host);
CHECK(host->mount=="host:");
CHECK(host->host==(temp.dir()/"x.txt").lexically_normal());
CHECK_FALSE(host->folder);
CHECK(parse_error("host:"+generic+"/con.txt",true)=="\"host:"+generic+"/con.txt\" is not a valid file name");
}

TEST_CASE("device names and bad characters are not valid file names on every platform")
{
// Review Focus 2.
// ("a:b" would be a drive-relative host path; a colon later in a name is a bad character.)
for(const std::string name:{"con","aux.txt","COM1.log","nul","lpt9","CONIN$","conout$.txt","CLOCK$","COM\xC2\xB9.txt","lpt\xC2\xB3","a.","a ","a|b","prn","Nul.tar.gz","com0","con .txt","a<b","a>b","ab:c","a\"b","a?b","a*b","a\x01" "b","a\x7F" "b"}){
CAPTURE(name);
CHECK(parse_error(name)=="\""+zodiac::one_line(name)+"\" is not a valid file name");
CHECK(parse_error("sounds/"+name+"/x.ogg")=="\"sounds/"+zodiac::one_line(name)+"/x.ogg\" is not a valid file name");
}
for(const std::string name:{"console.txt","com10.txt","lpt","COM\xC2\xB4.txt","contoso","nul_","a.b","..a",".hidden","con-x","\xC4\xB1\xC5\x9F\xC4\xB1k.ogg"}){
CAPTURE(name);
CHECK(zodiac::vfs::parse_path(name,false).has_value());
CHECK(zodiac::vfs::valid_name(name));
}
}

TEST_CASE("fold_name is Unicode simple uppercase per code point")
{
using zodiac::vfs::fold_name;
CHECK(fold_name("sounds/Shot.ogg")=="SOUNDS/SHOT.OGG");
// Review Focus 1: dotless i and plain i both fold to I; dotted capital I stays itself.
CHECK(fold_name("\xC4\xB1\xC5\x9F\xC4\xB1k")=="I\xC5\x9EIK");
CHECK(fold_name("i\xC5\x9Fik")=="I\xC5\x9EIK");
CHECK(fold_name("\xC4\xB0\xC5\x9E\xC4\xB0K")=="\xC4\xB0\xC5\x9E\xC4\xB0K");
CHECK(fold_name("stra\xC3\x9F" "e")=="STRA\xC3\x9F" "E");
// Bytes that are not UTF-8 are kept; nothing is normalized.
CHECK(fold_name("a\xFF" "b\xC3")=="A\xFF" "B\xC3");
CHECK(fold_name("s\xCC\xA7")=="S\xCC\xA7");
}

TEST_CASE("folder lookups are case-insensitive with a dev mode warning")
{
const TempDir temp;
temp.write("sounds/shot.ogg","bang");
for(const bool dev:{true,false}){
CAPTURE(dev);
Vfs vfs;
LogLines log;
log.attach(vfs);
vfs.set_dev_mode(dev);
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
for(int i=0;i<3;++i){
CHECK(read_vfs(vfs,"Sounds/Shot.ogg")==std::optional<std::string>("bang"));
const auto info=vfs.stat("SOUNDS/SHOT.OGG");
REQUIRE(info);
CHECK(info->kind==EntryKind::file);
CHECK(info->size==4);
// stat names the entry as asked, on every platform.
CHECK(info->name=="SHOT.OGG");
}
// Review Focus 2: a backslash path opens the same file, and the right spelling never warns.
CHECK(read_vfs(vfs,"sounds\\shot.ogg")==std::optional<std::string>("bang"));
if(dev){
CHECK(log.lines==std::vector<std::string>{
"warning: \"Sounds/Shot.ogg\" was found as \"sounds/shot.ogg\"; use the real name, other systems may not find it",
"warning: \"SOUNDS/SHOT.OGG\" was found as \"sounds/shot.ogg\"; use the real name, other systems may not find it"});
}
else CHECK(log.lines.empty());
}
}

TEST_CASE("two names that differ only in case are ambiguous")
{
const TempDir temp;
temp.write("a.txt","lower");
temp.write("A.txt","upper");
// A file system that ignores case (Windows, macOS) holds only one of them; the rule is checked where both can exist.
if(read_disk(temp.at("a.txt"))!="lower"||read_disk(temp.at("A.txt"))!="upper"){
MESSAGE("the temporary folder ignores case; skipped");
return;
}
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
const auto opened=vfs.open_read("A.TXT");
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::ambiguous);
CHECK(opened.error().message=="\"A.TXT\" matches both \"A.txt\" and \"a.txt\"; rename one of them");
CHECK(read_failure_text("A.TXT",opened.error())==opened.error().message);
CHECK(read_vfs(vfs,"a.txt")==std::optional<std::string>("lower"));
CHECK(read_vfs(vfs,"A.txt")==std::optional<std::string>("upper"));
const auto stat=vfs.stat("dir/../A.tXt");
REQUIRE_FALSE(stat);
CHECK(stat.error().message=="\"dir/../A.tXt\" matches both \"A.txt\" and \"a.txt\"; rename one of them");
}

TEST_CASE("a file made by another process is found through the modification time")
{
const TempDir temp;
temp.write("folder/first.txt","1");
// An old folder time: the cache scanned now is not racy, so only the time change can make it look again.
const fs::path folder=temp.at("folder");
fs::last_write_time(folder,fs::last_write_time(folder)-std::chrono::hours(1));
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
CHECK(read_vfs(vfs,"FOLDER/FIRST.TXT")==std::optional<std::string>("1"));
FileError error{};
CHECK_FALSE(read_vfs(vfs,"folder/other.txt",&error));
CHECK(error==FileError::not_found);
// Written behind the Vfs's back, as another process would.
temp.write("folder/Other.TXT","2");
CHECK(read_vfs(vfs,"folder/other.txt")==std::optional<std::string>("2"));
CHECK(read_vfs(vfs,"Folder/OTHER.txt")==std::optional<std::string>("2"));
}

TEST_CASE("Turkish letters fold like NTFS and NFD names stay apart")
{
// Review Focus 1: identical on Windows and Linux.
const TempDir temp;
temp.write("I\xC5\x9EIK.ogg","light");
temp.write("s\xCC\xA7" "ehir.txt","nfd");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
CHECK(read_vfs(vfs,"\xC4\xB1\xC5\x9F\xC4\xB1k.ogg")==std::optional<std::string>("light"));
CHECK(read_vfs(vfs,"i\xC5\x9Fik.ogg")==std::optional<std::string>("light"));
FileError error{};
CHECK_FALSE(read_vfs(vfs,"\xC4\xB0\xC5\x9E\xC4\xB0K.ogg",&error));
CHECK(error==FileError::not_found);
CHECK(read_vfs(vfs,"s\xCC\xA7" "ehir.txt")==std::optional<std::string>("nfd"));
error={};
CHECK_FALSE(read_vfs(vfs,"\xC5\x9F" "ehir.txt",&error));
CHECK(error==FileError::not_found);
}

TEST_CASE("writes reuse a name of another case and invalidate the cache")
{
const TempDir temp;
temp.write("saves/save.txt","old");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
const auto before=vfs.list("saves",ListKind::files);
REQUIRE(before);
CHECK(*before==std::vector<std::string>{"save.txt"});
REQUIRE(write_vfs(vfs,"SAVES/SAVE.TXT","new"));
CHECK(read_disk(temp.at("saves/save.txt"))=="new");
REQUIRE(write_vfs(vfs,"Saves/Second.txt","2"));
const auto listed=vfs.list("saves/",ListKind::files);
REQUIRE(listed);
CHECK(*listed==std::vector<std::string>{"save.txt","Second.txt"});
REQUIRE(vfs.remove("saves/SECOND.TXT"));
const auto after=vfs.list("saves",ListKind::files);
REQUIRE(after);
CHECK(*after==std::vector<std::string>{"save.txt"});
}

TEST_CASE("folder operations and folder-form paths")
{
const TempDir temp;
temp.write("a.txt","a");
temp.write("B.txt","b");
temp.write("c.log","c");
temp.write("sub/x.txt","x");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
for(const char* folder:{"","/","sub/","sub/."}){
CAPTURE(folder);
const auto opened=vfs.open_read(folder);
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::is_directory);
CHECK(read_failure_text(folder,opened.error())=="cannot read \""+std::string(folder)+"\": is a directory");
const auto info=vfs.stat(folder);
REQUIRE(info);
CHECK(info->kind==EntryKind::directory);
}
const auto txt=vfs.list("",ListKind::files,"*.TXT");
REQUIRE(txt);
CHECK(*txt==std::vector<std::string>{"a.txt","B.txt"});
const auto dirs=vfs.list("/",ListKind::directories);
REQUIRE(dirs);
CHECK(*dirs==std::vector<std::string>{"sub"});
REQUIRE(vfs.create_directory("deep/er/est",true));
CHECK(fs::is_directory(temp.at("deep/er/est")));
const auto flat=vfs.create_directory("missing/child",false);
REQUIRE_FALSE(flat);
CHECK(flat.error().code==FileError::not_found);
const auto not_empty=vfs.delete_directory("deep",false);
REQUIRE_FALSE(not_empty);
CHECK(not_empty.error().code==FileError::not_empty);
CHECK(read_failure_text("deep",not_empty.error())=="cannot read \"deep\": directory not empty");
const auto on_file=vfs.list("a.txt",ListKind::files);
REQUIRE_FALSE(on_file);
CHECK(on_file.error().code==FileError::not_a_directory);
const auto below_file=vfs.list("a.txt/x",ListKind::files);
REQUIRE_FALSE(below_file);
CHECK(below_file.error().code==FileError::not_a_directory);
const auto nowhere=vfs.list("nowhere",ListKind::files);
REQUIRE_FALSE(nowhere);
CHECK(nowhere.error().code==FileError::not_found);
REQUIRE(vfs.delete_directory("DEEP",true));
CHECK_FALSE(fs::exists(temp.at("deep")));
const auto root=vfs.delete_directory("/",true);
REQUIRE_FALSE(root);
CHECK(root.error().code==FileError::access_denied);
const auto missing=vfs.open_read("nothing.txt");
REQUIRE_FALSE(missing);
CHECK(read_failure_text("nothing.txt",missing.error())=="cannot read \"nothing.txt\": file not found");
const auto bad=vfs.open_read("con");
REQUIRE_FALSE(bad);
CHECK(bad.error().code==FileError::bad_path);
CHECK(read_failure_text("con",bad.error())=="\"con\" is not a valid file name");
}

TEST_CASE("a read-only mount refuses writes with the pack text")
{
const TempDir temp;
temp.write("defaults.txt","d");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),false));
CHECK(read_vfs(vfs,"defaults.txt")==std::optional<std::string>("d"));
const auto written=vfs.open_write("defaults.txt",WriteMode::truncate);
REQUIRE_FALSE(written);
CHECK(written.error().code==FileError::read_only_pack);
CHECK(written.error().message=="\"defaults.txt\" is in a read-only pack; save files in data:/ instead");
CHECK(read_disk(temp.at("defaults.txt"))=="d");
}

TEST_CASE("newer mounts are asked first and mount points read as folders")
{
const TempDir base;
const TempDir extra;
base.write("shared.txt","base");
base.write("only_base.txt","b");
extra.write("shared.txt","extra");
Vfs vfs;
int owner=0;
REQUIRE(vfs.mount_folder("/",base.dir(),true));
REQUIRE(vfs.mount("/",std::make_shared<FolderSource>(extra.dir(),false),true,&owner));
CHECK_FALSE(vfs.mount("/",std::make_shared<FolderSource>(extra.dir(),false),true,&owner));
CHECK(read_vfs(vfs,"shared.txt")==std::optional<std::string>("extra"));
CHECK(read_vfs(vfs,"only_base.txt")==std::optional<std::string>("b"));
REQUIRE(vfs.mount("mods/Extra",std::make_shared<FolderSource>(extra.dir(),false),false,&owner));
CHECK(read_vfs(vfs,"MODS/extra/shared.txt")==std::optional<std::string>("extra"));
const auto mods=vfs.stat("mods");
REQUIRE(mods);
CHECK(mods->kind==EntryKind::directory);
const auto dirs=vfs.list("",ListKind::directories);
REQUIRE(dirs);
CHECK(*dirs==std::vector<std::string>{"mods"});
const auto inner=vfs.list("mods",ListKind::directories);
REQUIRE(inner);
CHECK(*inner==std::vector<std::string>{"Extra"});
const auto files=vfs.list("",ListKind::files);
REQUIRE(files);
CHECK(*files==std::vector<std::string>{"only_base.txt","shared.txt"});
CHECK(vfs.unmount(&owner)==2);
CHECK(read_vfs(vfs,"shared.txt")==std::optional<std::string>("base"));
CHECK_FALSE(vfs.stat("mods"));
CHECK_FALSE(vfs.mount("../x",std::make_shared<FolderSource>(extra.dir(),false),true,&owner));
CHECK_FALSE(vfs.mount("C:/x",std::make_shared<FolderSource>(extra.dir(),false),true,&owner));
}

TEST_CASE("host paths read and write through the host source")
{
const TempDir temp;
Vfs vfs;
const std::string generic=generic_utf8(temp.dir());
const std::string host="host:"+generic+"/x.txt";
const auto refused=vfs.open_write(host,WriteMode::truncate);
REQUIRE_FALSE(refused);
CHECK(refused.error().code==FileError::bad_path);
CHECK(refused.error().message=="\""+host+"\" is a host path; set [files] host_paths = true to use it");
vfs.set_host_paths(true);
REQUIRE(write_vfs(vfs,host,"host"));
CHECK(read_disk(temp.at("x.txt"))=="host");
CHECK(read_vfs(vfs,host)==std::optional<std::string>("host"));
#ifdef _WIN32
// A drive path as BGT scripts write it.
CHECK(read_vfs(vfs,generic+"/x.txt")==std::optional<std::string>("host"));
std::string backslashed=generic+"\\x.txt";
for(char& c:backslashed)if(c=='/')c='\\';
CHECK(read_vfs(vfs,backslashed)==std::optional<std::string>("host"));
#endif
const auto listed=vfs.list("host:"+generic,ListKind::files);
REQUIRE(listed);
CHECK(*listed==std::vector<std::string>{"x.txt"});
REQUIRE(vfs.remove(host));
CHECK_FALSE(fs::exists(temp.at("x.txt")));
}

TEST_CASE("concurrent lookups while a second folder is mounted and unmounted")
{
const TempDir base;
const TempDir extra;
base.write("stable.txt","stable");
extra.write("other.txt","other");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",base.dir(),true));
std::atomic<int> failures{0};
std::vector<std::thread> threads;
for(int t=0;t<8;++t){
threads.emplace_back([&vfs,&failures,t]{
for(int i=0;i<10000;++i){
// Every other lookup spells the name in another case, so the case caches are read concurrently too.
const char* name=(i+t)%2?"STABLE.txt":"stable.txt";
const auto info=vfs.stat(name);
if(!info||info->kind!=EntryKind::file||info->size!=6)failures.fetch_add(1);
if(read_vfs(vfs,name)!=std::optional<std::string>("stable"))failures.fetch_add(1);
}
});
}
int owner=0;
for(int i=0;i<1000;++i){
if(!vfs.mount("/",std::make_shared<FolderSource>(extra.dir(),false),true,&owner))failures.fetch_add(1);
if(vfs.unmount(&owner)!=1)failures.fetch_add(1);
}
for(std::thread& thread:threads)thread.join();
CHECK(failures.load()==0);
}

TEST_CASE("mount_project maps data:/ to the per-user data folder and creates it on the first write")
{
const TempDir temp;
const fs::path user_data=temp.at("user data");
const ScopedEnv env(data_variable,user_data);
fs::create_directories(temp.at("project"));
zodiac::Project project;
project.root=temp.at("project");
project.name="vfs game \xC5\x9F";
Vfs vfs;
LogLines log;
log.attach(vfs);
zodiac::vfs::mount_project(vfs,project,true);
CHECK_FALSE(vfs.host_paths());
CHECK(vfs.dev_mode());
const fs::path game_data=user_data/path_from_utf8(project.name);
// Nothing is made at start-up; data:/ reads as an empty folder until then.
CHECK_FALSE(fs::exists(user_data));
const auto empty=vfs.list("data:/",ListKind::files);
REQUIRE(empty);
CHECK(empty->empty());
const auto info=vfs.stat("data:/");
REQUIRE(info);
CHECK(info->kind==EntryKind::directory);
REQUIRE(write_vfs(vfs,"data:/x","saved"));
CHECK(read_disk(game_data/"x")=="saved");
CHECK(read_vfs(vfs,"DATA:/X")==std::optional<std::string>("saved"));
REQUIRE(vfs.create_directory("data:/saves/slot1",true));
CHECK(fs::is_directory(game_data/"saves"/"slot1"));
REQUIRE(write_vfs(vfs,"/root.txt","r"));
CHECK(read_disk(project.root/"root.txt")=="r");
CHECK(log.lines==std::vector<std::string>{"warning: \"DATA:/X\" was found as \"data:/x\"; use the real name, other systems may not find it"});
project.files.host_paths=true;
Vfs host_vfs;
zodiac::vfs::mount_project(host_vfs,project,false);
CHECK(host_vfs.host_paths());
CHECK_FALSE(host_vfs.dev_mode());
}

TEST_CASE("data:/ is not mounted for a game name that cannot be a folder name")
{
const TempDir temp;
const ScopedEnv env(data_variable,temp.at("user data"));
Vfs vfs;
LogLines log;
log.attach(vfs);
zodiac::vfs::mount_game_folders(vfs,temp.dir(),"a/b",false,false);
CHECK(log.lines==std::vector<std::string>{"warning: data:/ is not available: the game name \"a/b\" cannot be used as a folder name"});
const auto written=vfs.open_write("data:/x",WriteMode::truncate);
REQUIRE_FALSE(written);
CHECK(written.error().code==FileError::not_found);
}

TEST_CASE("the release pack check warns once per path in dev mode")
{
const TempDir temp;
Vfs vfs;
LogLines log;
log.attach(vfs);
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
vfs.set_release_pack_check([](std::string_view relative){return relative.starts_with("data/");});
REQUIRE(vfs.create_directory("data",false));
REQUIRE(write_vfs(vfs,"data/a.txt","1"));
vfs.set_dev_mode(true);
REQUIRE(write_vfs(vfs,"data/a.txt","1"));
REQUIRE(write_vfs(vfs,"data/a.txt","2"));
REQUIRE(write_vfs(vfs,"other.txt","3"));
CHECK(log.lines==std::vector<std::string>{"warning: \"data/a.txt\" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead"});
}

TEST_CASE("ill-formed UTF-8 is not a valid file name and never throws")
{
for(const std::string name:{"save\xFF.txt","a\xC3","\xED\xA0\x80.txt","\xC0\xAF","x\xF4\x90\x80\x80","\x80"}){
CAPTURE(zodiac::one_line(name));
CHECK_FALSE(zodiac::vfs::valid_name(name));
CHECK(parse_error(name)=="\""+zodiac::one_line(name)+"\" is not a valid file name");
}
const TempDir temp;
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
vfs.set_host_paths(true);
const std::string generic=generic_utf8(temp.dir());
for(const std::string& path:{std::string("save\xFF.txt"),std::string("dir/\xC3/x"),"host:"+generic+"/save\xFF.txt",generic+"/\xFF"}){
CAPTURE(zodiac::one_line(path));
const auto stat=vfs.stat(path);
REQUIRE_FALSE(stat);
CHECK(stat.error().code==FileError::bad_path);
CHECK(stat.error().message=="\""+zodiac::one_line(path)+"\" is not a valid file name");
CHECK_FALSE(vfs.open_read(path));
CHECK_FALSE(vfs.open_write(path,WriteMode::truncate));
CHECK_FALSE(vfs.list(path,ListKind::files));
CHECK_FALSE(vfs.create_directory(path,true));
CHECK_FALSE(vfs.remove(path));
}
CHECK(fs::is_empty(temp.dir()));
}

TEST_CASE("host paths into the Win32 device namespaces are refused")
{
for(const std::string path:{"//./C:/x","//?/C:/x","\\\\.\\PhysicalDrive0","\\\\?\\C:\\x","host://./pipe/x","host:\\\\?\\C:\\x","//.","\\\\?"}){
CAPTURE(path);
CHECK(parse_error(path)=="\""+path+"\" is a host path; set [files] host_paths = true to use it");
CHECK(parse_error(path,true)=="\""+path+"\" is not a valid file name");
}
// A server name that only starts with a dot is an ordinary share.
CHECK(zodiac::vfs::parse_path("//.server/share/x",true).has_value());
}

TEST_CASE("creating many files in a folder does not rescan it once per file")
{
const TempDir temp;
fs::create_directories(temp.at("many"));
// An old folder time, so the scan below is not racy and only a real change of the folder could call for another.
fs::last_write_time(temp.at("many"),fs::last_write_time(temp.at("many"))-std::chrono::hours(1));
const auto source=std::make_shared<FolderSource>(temp.dir(),false);
Vfs vfs;
REQUIRE(vfs.mount("/",source,true,nullptr));
const auto empty=vfs.list("many",ListKind::files);
REQUIRE(empty);
CHECK(empty->empty());
const std::uint64_t before=source->scans();
constexpr int count=300;
for(int i=0;i<count;++i)REQUIRE(write_vfs(vfs,"many/file"+std::to_string(i)+".txt","x"));
// Each create misses the exact name, so it walks the cache: without in-place updates that would be one scan per file. Every
// write leaves the folder racy, which allows one rescan per racy window at most.
CHECK(source->scans()-before<=3);
const auto listed=vfs.list("many",ListKind::files);
REQUIRE(listed);
CHECK(listed->size()==static_cast<std::size_t>(count));
CHECK(listed->front()=="file0.txt");
for(int i=0;i<count;i+=2)REQUIRE(vfs.remove("MANY/FILE"+std::to_string(i)+".TXT"));
REQUIRE(vfs.create_directory("many/sub",false));
const auto after=vfs.list("many",ListKind::files);
REQUIRE(after);
CHECK(after->size()==static_cast<std::size_t>(count/2));
const auto dirs=vfs.list("many",ListKind::directories);
REQUIRE(dirs);
CHECK(*dirs==std::vector<std::string>{"sub"});
CHECK(source->scans()-before<=6);
}

TEST_CASE("a folder time in the future does not keep the cache racy")
{
const TempDir temp;
temp.write("future/a.txt","a");
fs::last_write_time(temp.at("future"),fs::file_time_type::clock::now()+std::chrono::hours(1));
const auto source=std::make_shared<FolderSource>(temp.dir(),false);
Vfs vfs;
REQUIRE(vfs.mount("/",source,true,nullptr));
REQUIRE(vfs.list("future",ListKind::files));
// The root's own scan, which the misses below walk through, happens first.
CHECK_FALSE(vfs.stat("future/missing.txt"));
const std::uint64_t before=source->scans();
for(int i=0;i<5;++i)CHECK_FALSE(vfs.stat("future/missing.txt"));
CHECK(read_vfs(vfs,"FUTURE/A.TXT")==std::optional<std::string>("a"));
CHECK(source->scans()==before);
}

TEST_CASE("8.3 short names are not found, as on Linux")
{
const TempDir temp;
temp.write("longfilename.ogg","long");
temp.write("a~b.txt","tilde");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
CHECK(read_vfs(vfs,"longfilename.ogg")==std::optional<std::string>("long"));
// A real name with a tilde is found through the cache, in any case.
CHECK(read_vfs(vfs,"a~b.txt")==std::optional<std::string>("tilde"));
CHECK(read_vfs(vfs,"A~B.TXT")==std::optional<std::string>("tilde"));
std::error_code ec;
if(!fs::exists(temp.at("LONGFI~1.OGG"),ec)){
MESSAGE("the volume makes no 8.3 names; only the long-name checks ran");
return;
}
for(const char* path:{"LONGFI~1.OGG","longfi~1.ogg"}){
CAPTURE(path);
FileError error{};
CHECK_FALSE(read_vfs(vfs,path,&error));
CHECK(error==FileError::not_found);
CHECK_FALSE(vfs.stat(path));
}
}

TEST_CASE("plain folders before a non-ASCII or tilde segment may be spelled in another case")
{
const TempDir temp;
temp.write("Sesler/a\xC4\x9F" "a\xC3\xA7.ogg","tree");
temp.write("Sesler/Alt/longfilename.ogg","long");
Vfs vfs;
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
for(const char* path:{"Sesler/a\xC4\x9F" "a\xC3\xA7.ogg","sesler/a\xC4\x9F" "a\xC3\xA7.ogg","SESLER/A\xC4\x9E" "A\xC3\x87.OGG"}){
CAPTURE(zodiac::one_line(path));
CHECK(read_vfs(vfs,path)==std::optional<std::string>("tree"));
const auto info=vfs.stat(path);
REQUIRE(info);
CHECK(info->size==4);
}
CHECK(read_vfs(vfs,"SESLER/alt/LongFileName.OGG")==std::optional<std::string>("long"));
FileError error{};
CHECK_FALSE(read_vfs(vfs,"nope/a\xC4\x9F" "a\xC3\xA7.ogg",&error));
CHECK(error==FileError::not_found);
// A file where the plain folder should be.
temp.write("plain.txt","p");
CHECK_FALSE(vfs.stat("PLAIN.TXT/a\xC4\x9F.ogg"));
std::error_code ec;
if(fs::exists(temp.at("Sesler/Alt/LONGFI~1.OGG"),ec)){
for(const char* path:{"Sesler/Alt/LONGFI~1.OGG","sesler/ALT/longfi~1.ogg"}){
CAPTURE(path);
error={};
CHECK_FALSE(read_vfs(vfs,path,&error));
CHECK(error==FileError::not_found);
}
}
else MESSAGE("the volume makes no 8.3 names; the short-name checks were skipped");
// Writes take the same route: a new non-ASCII name below an other-case plain folder lands in the real folder.
REQUIRE(write_vfs(vfs,"SESLER/yeni\xC5\x9F.ogg","new"));
CHECK(read_disk(temp.at("Sesler/yeni\xC5\x9F.ogg"))=="new");
}

namespace {
// Throws std::logic_error from every lookup, as a broken source would.
class ThrowingSource final:public zodiac::vfs::Source{
public:
std::expected<zodiac::vfs::EntryInfo,FileError> stat(std::string_view) override{throw std::logic_error("source broke");}
std::expected<std::shared_ptr<zodiac::vfs::Reader>,FileError> open_read(std::string_view) override{throw 42;}
std::expected<std::unique_ptr<zodiac::vfs::WriteTarget>,FileError> open_write(std::string_view,WriteMode) override{throw std::logic_error("source broke");}
std::expected<void,FileError> list(std::string_view,std::vector<zodiac::vfs::EntryInfo>&) override{throw std::logic_error("source broke");}
std::expected<void,FileError> remove(std::string_view,bool) override{throw std::logic_error("source broke");}
std::expected<void,FileError> make_directory(std::string_view) override{throw std::logic_error("source broke");}
std::expected<void,FileError> remove_directory(std::string_view,bool) override{throw std::logic_error("source broke");}
bool read_only() const noexcept override{return false;}
};
}

TEST_CASE("an exception inside a file operation becomes io and is logged in dev mode")
{
Vfs vfs;
LogLines log;
log.attach(vfs);
REQUIRE(vfs.mount("/",std::make_shared<ThrowingSource>(),true,nullptr));
const auto quiet=vfs.stat("x.txt");
REQUIRE_FALSE(quiet);
CHECK(quiet.error().code==FileError::io);
CHECK(log.lines.empty());
vfs.set_dev_mode(true);
const auto stat=vfs.stat("x.txt");
REQUIRE_FALSE(stat);
CHECK(stat.error().code==FileError::io);
CHECK_FALSE(vfs.open_read("y.txt"));
CHECK_FALSE(vfs.list("",ListKind::files));
CHECK(log.lines==std::vector<std::string>{
"error: internal error in stat of \"x.txt\": source broke",
"error: internal error in open_read of \"y.txt\": unknown exception",
"error: internal error in list of \"\": source broke"});
// A log handler that throws as well: still nothing escapes.
vfs.set_log([](zodiac_log_level,std::string_view){throw std::runtime_error("log broke");});
const auto again=vfs.open_write("z.txt",WriteMode::truncate);
REQUIRE_FALSE(again);
CHECK(again.error().code==FileError::io);
CHECK_FALSE(vfs.remove("z.txt"));
CHECK_FALSE(vfs.create_directory("d",true));
CHECK_FALSE(vfs.delete_directory("d",true));
}

TEST_CASE("a throwing callback after a successful write keeps the write")
{
const TempDir temp;
Vfs vfs;
LogLines log;
log.attach(vfs);
vfs.set_dev_mode(true);
REQUIRE(vfs.mount_folder("/",temp.dir(),true));
vfs.set_release_pack_check([](std::string_view)->bool{throw std::logic_error("check broke");});
REQUIRE(write_vfs(vfs,"saved.txt","kept"));
CHECK(read_disk(temp.at("saved.txt"))=="kept");
CHECK(log.lines==std::vector<std::string>{"error: internal error in the release pack check of \"saved.txt\": check broke"});
// The warning's own log handler throwing: the write still stands and nothing escapes.
vfs.set_release_pack_check([](std::string_view){return true;});
vfs.set_log([](zodiac_log_level,std::string_view){throw std::runtime_error("log broke");});
REQUIRE(write_vfs(vfs,"again.txt","also kept"));
CHECK(read_disk(temp.at("again.txt"))=="also kept");
const auto parsed=vfs.parse("fine/name.txt");
REQUIRE(parsed);
CHECK(parsed->relative=="fine/name.txt");
CHECK(vfs.unmount(&temp)==0);
}

namespace {
// A source that reports every lookup as ambiguous without naming the matches.
class AmbiguousSource final:public zodiac::vfs::Source{
public:
std::expected<zodiac::vfs::EntryInfo,FileError> stat(std::string_view) override{return std::unexpected(FileError::ambiguous);}
std::expected<std::shared_ptr<zodiac::vfs::Reader>,FileError> open_read(std::string_view) override{return std::unexpected(FileError::ambiguous);}
std::expected<std::unique_ptr<zodiac::vfs::WriteTarget>,FileError> open_write(std::string_view,WriteMode) override{return std::unexpected(FileError::ambiguous);}
std::expected<void,FileError> list(std::string_view,std::vector<zodiac::vfs::EntryInfo>&) override{return std::unexpected(FileError::ambiguous);}
std::expected<void,FileError> remove(std::string_view,bool) override{return std::unexpected(FileError::ambiguous);}
std::expected<void,FileError> make_directory(std::string_view) override{return std::unexpected(FileError::ambiguous);}
std::expected<void,FileError> remove_directory(std::string_view,bool) override{return std::unexpected(FileError::ambiguous);}
bool read_only() const noexcept override{return false;}
};
}

TEST_CASE("an ambiguity is never worded as a read failure")
{
Vfs vfs;
REQUIRE(vfs.mount("/",std::make_shared<AmbiguousSource>(),true,nullptr));
const auto opened=vfs.open_read("Thing.txt");
REQUIRE_FALSE(opened);
CHECK(opened.error().code==FileError::ambiguous);
CHECK(read_failure_text("Thing.txt",opened.error())=="\"Thing.txt\" matches more than one name; rename one of them");
const auto listed=vfs.list("x",ListKind::files);
REQUIRE_FALSE(listed);
CHECK(listed.error().message=="\"x\" matches more than one name; rename one of them");
}

TEST_CASE("dev mode warns once per misspelled path and checks a right spelling once")
{
const TempDir temp;
temp.write("sounds/shot.ogg","bang");
const auto source=std::make_shared<FolderSource>(temp.dir(),false);
Vfs vfs;
LogLines log;
log.attach(vfs);
vfs.set_dev_mode(true);
REQUIRE(vfs.mount("/",source,true,nullptr));
for(int i=0;i<100;++i){
REQUIRE(vfs.stat("sounds/shot.ogg"));
REQUIRE(vfs.stat("Sounds/shot.ogg"));
}
CHECK(log.lines==std::vector<std::string>{"warning: \"Sounds/shot.ogg\" was found as \"sounds/shot.ogg\"; use the real name, other systems may not find it"});
// More distinct paths than the checked table holds: lookups stay correct and each misspelling still warns once.
for(int i=0;i<5000;++i)temp.write("many/f"+std::to_string(i),"");
log.lines.clear();
for(int round=0;round<2;++round)for(int i=0;i<5000;++i)REQUIRE(vfs.stat("many/f"+std::to_string(i)));
CHECK(log.lines.empty());
REQUIRE(vfs.stat("MANY/f1"));
REQUIRE(vfs.stat("MANY/f1"));
CHECK(log.lines.size()==1);
}
