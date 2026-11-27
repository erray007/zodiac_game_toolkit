// zodiac::cli::build_project (see cli/build_command.hpp): loads, checks and compiles a project, then links, copies and signs its release.
#include <cli/build_command.hpp>
#include <cli/build_report.hpp>
#include <core/path_utf8.hpp>
#include <platform/console.hpp>
#include <platform/result_window.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef _WIN32
#include <cli/linker.hpp>
#include <cli/literal_paths.hpp>
#include <cli/pack_key.hpp>
#include <cli/release_packing.hpp>
#include <core/coff_object.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/release_pack.hpp>
#include <core/kit_layout.hpp>
#include <core/module_registry.hpp>
#include <core/payload.hpp>
#include <core/pe_imports.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <core/text.hpp>
#include <core/vfs/startup.hpp>
#include <platform/process.hpp>
#include <zodiac/plugin.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <system_error>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef ZODIAC_VERSION
#error ZODIAC_VERSION must be defined by the build (the project version)
#endif

namespace zodiac::cli {
namespace {
constexpr int exit_build_error=2;

// What a build produced, also when it failed part way: the executable's path (empty until known) and the game's name (empty until the project loads).
// used_lone_file is set once the project loads: true when a lone script was built without a project file (with the core only), so the result
// window can offer the project file it could have had; root and main_script (absolute) are where that file goes and the script it names.
struct BuildOutcome{
std::filesystem::path exe;
std::string game_name;
std::filesystem::path root;
std::filesystem::path main_script;
bool used_lone_file=false;
};

#ifdef _WIN32
namespace fs=std::filesystem;
constexpr int exit_ok=0;

std::string in_quotes(std::string_view text)
{
std::string result;
result.reserve(text.size()+2);
result.push_back('"');
result.append(text);
result.push_back('"');
return result;
}

std::string quoted_path(const fs::path& path)
{
return in_quotes(path_to_utf8(path));
}

int fail(Diagnostics& diagnostics,std::string_view message) noexcept
{
diagnostics.report(ZODIAC_LOG_ERROR,message);
return exit_build_error;
}

std::string_view or_empty(const char* text) noexcept
{
return text?std::string_view(text):std::string_view();
}

bool contains(std::span<const std::string> names,std::string_view name) noexcept
{
return std::ranges::find(names,name)!=names.end();
}

// Removes a temporary file of the build when the build is over, whatever happened; a file that is already gone is fine.
class TemporaryFile{
public:
TemporaryFile(fs::path path,Diagnostics& diagnostics):path_(std::move(path)),diagnostics_(diagnostics){}
~TemporaryFile()
{
std::error_code ec;
fs::remove(path_,ec);
if(ec)diagnostics_.report(ZODIAC_LOG_WARNING,"warning: cannot remove "+quoted_path(path_)+": "+error_text(ec));
}
TemporaryFile(const TemporaryFile&)=delete;
TemporaryFile& operator=(const TemporaryFile&)=delete;
const fs::path& path() const noexcept{return path_;}
private:
fs::path path_;
Diagnostics& diagnostics_;
};

// A whole file, or nullopt when it cannot be read.
std::optional<std::string> read_text(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
if(!in)return std::nullopt;
std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
if(in.bad())return std::nullopt;
return text;
}

std::string_view without_trailing_space(std::string_view text) noexcept
{
while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'||text.back()==' '||text.back()=='\t'))text.remove_suffix(1);
return text;
}

bool write_binary(const fs::path& path,std::string_view bytes)
{
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
out.close();
return !out.fail();
}

fs::path absolute_normal(const fs::path& path)
{
std::error_code ec;
fs::path result=fs::absolute(path,ec);
if(ec)result=path;
result=result.lexically_normal();
if(!result.has_filename()&&result.has_relative_path())result=result.parent_path();
return result;
}

// True when path lies below dir; both absolute and normalized. Each element is compared as the file system does, so "D:\Game\Release" is inside "d:\game".
bool is_inside(const fs::path& path,const fs::path& dir)
{
auto element=path.begin();
for(const fs::path& part:dir){
if(element==path.end()||!same_path_text(*element,part))return false;
++element;
}
return element!=path.end();
}

// True when name+".exe" is a plain file name on Windows: no separators, reserved or control characters, no trailing dot or space, no device name.
bool usable_file_name(std::string_view name) noexcept
{
if(name.empty()||name=="."||name==".."||name.back()=='.'||name.back()==' ')return false;
for(const char c:name){
const unsigned char byte=static_cast<unsigned char>(c);
if(byte<0x20||byte==0x7F||std::string_view("<>:\"/\\|?*").find(c)!=std::string_view::npos)return false;
}
// Device names stay reserved with any extension, case-insensitively ("con.exe" is the console).
const std::string_view stem=name.substr(0,name.find('.'));
std::string upper(stem);
for(char& c:upper)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');
constexpr std::array<std::string_view,4> devices{"CON","PRN","AUX","NUL"};
if(std::ranges::find(devices,upper)!=devices.end())return false;
if(upper.size()==4&&(upper.starts_with("COM")||upper.starts_with("LPT"))&&upper[3]>='0'&&upper[3]<='9')return false;
return true;
}

// The folder the project of target lives in: target itself, or for a file the nearest folder above it holding zodiac.toml.
std::optional<fs::path> project_root_of(const fs::path& target)
{
std::error_code ec;
if(fs::is_regular_file(target,ec))return find_project_root(target);
const fs::path root=absolute_normal(target);
if(!fs::is_regular_file(root/"zodiac.toml",ec)||ec)return std::nullopt;
return root;
}

struct StaticLibrary{
std::string name;
fs::path archive;
};

constexpr std::array<std::string_view,9> permissive_licenses{"Zlib","MIT","BSD-2-Clause","BSD-3-Clause","Apache-2.0","BSL-1.0","ISC","Unlicense","CC0-1.0"};

// Finds the static library of a module (step 2): the kit's modules/static/zodiac_mod_<name>.a, else <root>/plugins/static/zodiac_mod_<name>.a
// whose .stamp equals the kit's toolchain.txt. The kit's toolchain identity is read once per build and kept in toolchain.
class StaticLibraries{
public:
StaticLibraries(const KitLayout& kit,fs::path root):kit_(kit),plugin_dir_(std::move(root)/"plugins"/"static"){}
// The archive, or the full error line that refuses the module.
std::expected<fs::path,std::string> find(std::string_view name)
{
const std::string stem="zodiac_mod_"+std::string(name);
std::error_code ec;
fs::path archive=kit_.static_modules/(stem+".a");
if(fs::is_regular_file(archive,ec))return archive;
archive=plugin_dir_/(stem+".a");
if(!fs::is_regular_file(archive,ec))return std::unexpected("error: module "+in_quotes(name)+" has no static library in the kit or in plugins/static; list it under dynamic");
if(!toolchain_)toolchain_=read_text(kit_.release_lib/"toolchain.txt");
if(!*toolchain_)return std::unexpected("error: cannot read the kit's toolchain identity "+quoted_path(kit_.release_lib/"toolchain.txt"));
const std::optional<std::string> stamp=read_text(plugin_dir_/(stem+".stamp"));
if(!stamp||without_trailing_space(*stamp)!=without_trailing_space(**toolchain_))return std::unexpected("error: plugin "+in_quotes(name)+" was not built with this kit's toolchain; list it under dynamic");
return archive;
}
private:
const KitLayout& kit_;
fs::path plugin_dir_;
std::optional<std::optional<std::string>> toolchain_;
};

// Step 2 of the build: runs while the modules are loaded and none is registered yet. Fills libraries in registration order. Returns 0 or 2.
int check_static_modules(const Project& project,const KitLayout& kit,std::span<const zodiac_module_info* const> loaded,std::vector<StaticLibrary>& libraries,Diagnostics& diagnostics)
{
int result=exit_ok;
StaticLibraries archives(kit,project.root);
for(const zodiac_module_info* info:loaded){
const std::string_view name=or_empty(info->name);
if(!contains(project.static_modules,name))continue;
const std::string_view license=or_empty(info->license);
const std::string quoted_name=in_quotes(name);
if(!(module_link_modes(*info)&ZODIAC_LINK_STATIC)){
result=fail(diagnostics,"error: module "+quoted_name+" ("+std::string(license)+") cannot be linked statically; list it under dynamic");
continue;
}
if(std::ranges::find(permissive_licenses,license)==permissive_licenses.end())diagnostics.report(ZODIAC_LOG_WARNING,"warning: module "+quoted_name+" is linked statically under license "+std::string(license)+"; check its terms");
auto archive=archives.find(name);
if(!archive){
result=fail(diagnostics,archive.error());
continue;
}
libraries.push_back({std::string(name),std::move(*archive)});
}
return result;
}

// The payload of the release: with a pack (pack_mode not none), key is its key.
GamePayload payload_of(const Project& project,const ScriptRuntime& runtime,std::string bytecode,PackMode pack_mode,const crypto::Key& key)
{
GamePayload payload;
payload.kit_version=ZODIAC_VERSION;
payload.game_name=project.name;
// Exactly the names ScriptRuntime::load_modules passes to the registry, in that order.
payload.requested=project.static_modules;
for(const std::string& name:project.dynamic_modules)if(!contains(payload.requested,name))payload.requested.push_back(name);
for(const zodiac_module_info* info:runtime.modules()){
const std::string_view name=or_empty(info->name);
payload.modules.push_back({std::string(name),std::string(or_empty(info->version)),contains(project.static_modules,name)?LinkMode::static_link:LinkMode::dynamic_link});
}
payload.core_exclude=project.core_exclude;
payload.diagnostics=project.diagnostics;
if(!payload.diagnostics.file.empty())payload.diagnostics.file=payload.diagnostics.file.lexically_relative(project.root);
// A release without packed assets has no pack, so nothing is mounted writable either.
payload.pack_mode=pack_mode;
payload.pack_writable=pack_mode!=PackMode::none&&project.pack.writable;
if(pack_mode!=PackMode::none)payload.pack_key.assign(reinterpret_cast<const char*>(key.data()),key.size());
payload.host_paths=project.files.host_paths;
const std::u8string section=project.main_script.lexically_relative(project.root).generic_u8string();
payload.scripts.push_back({std::string(reinterpret_cast<const char*>(section.data()),section.size()),true,std::move(bytecode)});
return payload;
}

// The first folder in the runtime's search order (module_dirs, then <root>/plugins) holding the module's shared library.
std::optional<fs::path> find_module_library(std::string_view name,const std::vector<fs::path>& dirs,const fs::path& root)
{
const std::string file=module_file_name(name);
std::error_code ec;
for(const fs::path& dir:dirs)if(fs::is_regular_file(dir/file,ec))return dir/file;
const fs::path plugin=root/"plugins"/file;
if(fs::is_regular_file(plugin,ec))return plugin;
return std::nullopt;
}

// True when text starts with prefix (lower-case ASCII), compared ASCII case-insensitively.
bool starts_with_ascii_ci(std::string_view text,std::string_view prefix) noexcept
{
if(text.size()<prefix.size())return false;
for(std::size_t i=0;i<prefix.size();++i){
char c=text[i];
if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
if(c!=prefix[i])return false;
}
return true;
}

// Decides whether an imported DLL name not found next to a module is a Windows system DLL (spec item 7): an API set name, or a file
// SearchPathW finds in the system folder. The folder is asked for once, on the first name that needs it.
class SystemDlls{
public:
// name is printable ASCII (pe_imported_dlls guarantees it).
bool contains(std::string_view name)
{
if(starts_with_ascii_ci(name,"api-ms-win-")||starts_with_ascii_ci(name,"ext-ms-"))return true;
// A name with a path is never a system DLL; SearchPathW would look outside the system folder for it.
if(name.find_first_of("/\\:")!=std::string_view::npos)return false;
if(!folder_)folder_=system_folder();
if(folder_->empty())return false;
const std::wstring wide(name.begin(),name.end());
return SearchPathW(folder_->c_str(),wide.c_str(),L".dll",0,nullptr,nullptr)!=0;
}
private:
static std::wstring system_folder()
{
std::wstring folder(MAX_PATH,L'\0');
for(int attempt=0;attempt<2;++attempt){
const UINT length=GetSystemDirectoryW(folder.data(),static_cast<UINT>(folder.size()));
if(length==0)break;
if(length<folder.size()){
folder.resize(length);
return folder;
}
// Too small: length is the size needed, including the terminating NUL.
folder.assign(length,L'\0');
}
return {};
}
std::optional<std::wstring> folder_;
};

// The regular files directly inside each folder a module DLL lives in, listed once per build.
class FolderFiles{
public:
// The files in folder; nullptr after reporting "error: cannot read directory "<folder>": <reason>" when it cannot be listed.
const std::vector<fs::path>* of(const fs::path& folder,Diagnostics& diagnostics)
{
for(const auto& [listed,files]:folders_)if(same_path_text(listed,folder))return &files;
std::vector<fs::path> files;
std::error_code ec;
for(fs::directory_iterator entry(folder,ec),end;!ec&&entry!=end;entry.increment(ec)){
std::error_code type_error;
if(entry->is_regular_file(type_error))files.push_back(entry->path());
}
if(ec){
fail(diagnostics,"error: cannot read directory "+quoted_path(folder)+": "+error_text(ec));
return nullptr;
}
return &folders_.emplace_back(folder,std::move(files)).second;
}
private:
std::deque<std::pair<fs::path,std::vector<fs::path>>> folders_;
};

// The file the loader looks for when a DLL imports name: a name without a dot gets ".dll".
fs::path loader_file_name(std::string_view name)
{
fs::path file=path_from_utf8(name);
if(name.find('.')==std::string_view::npos)file+=".dll";
return file;
}

// The files that ship for the dynamic module module whose DLL is library (spec item 7): library, then the DLLs it imports that sit in its
// folder, recursively, breadth first, each once. Reports why and returns nullopt when the folder cannot be listed, a DLL is no x64 DLL or
// cannot be read, or an import is neither there nor a system DLL.
std::optional<std::vector<fs::path>> module_files(std::string_view module,const fs::path& library,FolderFiles& folders,SystemDlls& system_dlls,Diagnostics& diagnostics)
{
const std::vector<fs::path>* listing=folders.of(library.parent_path(),diagnostics);
if(!listing)return std::nullopt;
const std::vector<fs::path>& neighbours=*listing;
std::vector<fs::path> files{library};
const auto listed=[&files](const fs::path& file){return std::ranges::any_of(files,[&file](const fs::path& known){return same_path_text(known.filename(),file.filename());});};
for(std::size_t next=0;next<files.size();++next){
const auto imports=pe_imported_dlls(files[next]);
if(!imports){
fail(diagnostics,imports.error());
return std::nullopt;
}
for(const std::string& name:*imports){
const fs::path wanted=loader_file_name(name);
const auto found=std::ranges::find_if(neighbours,[&wanted](const fs::path& file){return same_path_text(file.filename(),wanted);});
if(found!=neighbours.end()){
if(!listed(*found))files.push_back(*found);
continue;
}
if(system_dlls.contains(name))continue;
fail(diagnostics,"error: module "+in_quotes(module)+" needs "+in_quotes(name)+", which is not next to "+quoted_path(library));
return std::nullopt;
}
}
return files;
}

// The files each dynamic module ships (module_files), walked once per module: before the modules load, so a missing third-party DLL
// is reported by name instead of as the loader's failure, and again only for modules that turn up while loading (undeclared dependencies).
class ModuleDlls{
public:
ModuleDlls(const std::vector<fs::path>& module_dirs,const fs::path& root):module_dirs_(module_dirs),root_(root){}
// Walks module when its library can be found; true when it cannot (the loading reports that) or the walk succeeds.
bool check(std::string_view module,Diagnostics& diagnostics)
{
return !find_module_library(module,module_dirs_,root_)||files(module,diagnostics);
}
// The files of module, the library first; nullptr after reporting why.
const std::vector<fs::path>* files(std::string_view module,Diagnostics& diagnostics)
{
for(const auto& [name,files]:walked_)if(name==module)return &files;
const std::optional<fs::path> library=find_module_library(module,module_dirs_,root_);
if(!library){
fail(diagnostics,"error: cannot find the library of module "+in_quotes(module)+" to copy it");
return nullptr;
}
std::optional<std::vector<fs::path>> found=module_files(module,*library,folders_,system_dlls_,diagnostics);
if(!found)return nullptr;
return &walked_.emplace_back(std::string(module),std::move(*found)).second;
}
private:
const std::vector<fs::path>& module_dirs_;
const fs::path& root_;
FolderFiles folders_;
SystemDlls system_dlls_;
std::deque<std::pair<std::string,std::vector<fs::path>>> walked_;
};

// Whether files a and b hold the same bytes; nullopt after reporting "error: cannot read "<file>"" when either cannot be read.
std::optional<bool> same_contents(const fs::path& a,const fs::path& b,Diagnostics& diagnostics)
{
std::error_code ec;
const std::uintmax_t size_a=fs::file_size(a,ec);
if(ec){
fail(diagnostics,"error: cannot read "+quoted_path(a));
return std::nullopt;
}
const std::uintmax_t size_b=fs::file_size(b,ec);
if(ec){
fail(diagnostics,"error: cannot read "+quoted_path(b));
return std::nullopt;
}
if(size_a!=size_b)return false;
std::ifstream in_a(a,std::ios::binary);
std::ifstream in_b(b,std::ios::binary);
if(!in_a||!in_b){
fail(diagnostics,"error: cannot read "+quoted_path(!in_a?a:b));
return std::nullopt;
}
constexpr std::size_t chunk=65536;
std::vector<char> buffer_a(chunk);
std::vector<char> buffer_b(chunk);
for(std::uintmax_t left=size_a;left>0;){
const std::size_t count=static_cast<std::size_t>(std::min<std::uintmax_t>(left,chunk));
in_a.read(buffer_a.data(),static_cast<std::streamsize>(count));
in_b.read(buffer_b.data(),static_cast<std::streamsize>(count));
if(!in_a||!in_b){
fail(diagnostics,"error: cannot read "+quoted_path(!in_a?a:b));
return std::nullopt;
}
if(std::memcmp(buffer_a.data(),buffer_b.data(),count)!=0)return false;
left-=count;
}
return true;
}

// Copies from over to and gives the copy from's modification time, so a release's plain asset reports the time zodiac run sees
// (file_modified; dev and release are identical): std::filesystem::copy_file keeps it on some platforms only.
bool copy_into(const fs::path& from,const fs::path& to,Diagnostics& diagnostics)
{
std::error_code ec;
fs::create_directories(to.parent_path(),ec);
if(!ec)fs::copy_file(from,to,fs::copy_options::overwrite_existing,ec);
fs::file_time_type modified;
if(!ec)modified=fs::last_write_time(from,ec);
if(!ec)fs::last_write_time(to,modified,ec);
if(!ec)return true;
fail(diagnostics,"error: cannot copy "+quoted_path(from)+" to "+quoted_path(to)+": "+error_text(ec));
return false;
}

// True when the failed replace of exe means another process holds the old file open (a sharing or lock violation).
// MoveFileExW reports a target opened without sharing as ERROR_ACCESS_DENIED, like a read-only file or a folder, so access denied counts only
// when exe is a writable regular file that cannot even be opened for update.
bool is_in_use(const fs::path& exe,const std::error_code& ec)
{
constexpr int access_denied=5;
constexpr int sharing_violation=32;
constexpr int lock_violation=33;
bool denied=false;
if(ec.category()==std::system_category()){
const int value=ec.value();
if(value==sharing_violation||value==lock_violation)return true;
denied=value==access_denied;
}
else{
if(ec==std::errc::text_file_busy||ec==std::errc::device_or_resource_busy||ec==std::errc::no_lock_available)return true;
denied=ec==std::errc::permission_denied;
}
if(!denied)return false;
std::error_code status_error;
const fs::file_status status=fs::status(exe,status_error);
if(status_error||status.type()!=fs::file_type::regular||(status.permissions()&fs::perms::owner_write)==fs::perms::none)return false;
// Opening for update ("r+b") changes nothing; it fails only when another process denies sharing.
const std::fstream probe(exe,std::ios::in|std::ios::out|std::ios::binary);
return !probe.is_open();
}

// The sign command's program: absolute as given, relative to the project root when it names a folder, otherwise searched on PATH (as given, then with .exe and .com).
std::expected<fs::path,std::string> find_program(const std::string& program,const fs::path& root)
{
const fs::path path=path_from_utf8(program);
if(path.empty())return std::unexpected(std::string("the sign command names no program"));
if(path.is_absolute())return path;
if(path.has_parent_path()||path.has_root_path())return (root/path).lexically_normal();
const wchar_t* variable=_wgetenv(L"PATH");
std::wstring_view list=variable?std::wstring_view(variable):std::wstring_view();
std::error_code ec;
while(!list.empty()){
const std::size_t end=list.find(L';');
std::wstring_view entry=list.substr(0,end);
list=end==std::wstring_view::npos?std::wstring_view():list.substr(end+1);
if(entry.size()>=2&&entry.front()==L'"'&&entry.back()==L'"')entry=entry.substr(1,entry.size()-2);
if(entry.empty())continue;
const fs::path dir(entry);
if(fs::is_regular_file(dir/path,ec))return dir/path;
if(path.has_extension())continue;
for(const wchar_t* extension:{L".exe",L".com"}){
fs::path candidate=dir/path;
candidate+=extension;
if(fs::is_regular_file(candidate,ec))return candidate;
}
}
return std::unexpected("cannot find "+in_quotes(program)+" on PATH");
}

void report_indented(Diagnostics& diagnostics,std::string_view text)
{
while(!text.empty()){
const std::size_t end=text.find('\n');
const std::string_view line=text.substr(0,end);
if(!line.empty())diagnostics.report(ZODIAC_LOG_ERROR,"  "+std::string(line));
if(end==std::string_view::npos)break;
text.remove_prefix(end+1);
}
}

// Renames the finished temporary file over target. Returns 0 or 2.
int replace_file(const fs::path& temporary,const fs::path& target,Diagnostics& diagnostics)
{
std::error_code ec;
fs::rename(temporary,target,ec);
if(!ec)return exit_ok;
if(is_in_use(target,ec))return fail(diagnostics,"error: cannot replace "+quoted_path(target)+": it is in use (is the game still running?)");
return fail(diagnostics,"error: cannot replace "+quoted_path(target)+": "+error_text(ec));
}

// Runs the sign command on file (a temporary name) and, on failure, reports it under shown, the path the file is built for.
int sign(const std::vector<std::string>& command,const fs::path& file,const fs::path& shown_as,const fs::path& root,Diagnostics& diagnostics)
{
const std::string path=path_to_utf8(file);
std::vector<std::string> argv;
argv.reserve(command.size());
for(const std::string& part:command){
std::string argument=part;
for(std::size_t at=argument.find("{file}");at!=std::string::npos;at=argument.find("{file}",at+path.size()))argument.replace(at,6,path);
argv.push_back(std::move(argument));
}
const std::string shown=path_to_utf8(shown_as);
const auto program=find_program(argv.front(),root);
std::expected<platform::ProcessResult,std::string> ran=std::unexpected(std::string());
if(program){
argv.front()=path_to_utf8(*program);
ran=platform::run_process(argv);
}
if(ran&&ran->exit_code==0)return exit_ok;
fail(diagnostics,"error: signing "+in_quotes(shown)+" failed");
report_indented(diagnostics,!program?program.error():!ran?ran.error():ran->output);
return exit_build_error;
}

// Wipes a key, or the bytes of a string holding one, when it goes out of scope.
struct WipeOnExit{
void* bytes;
std::size_t size;
~WipeOnExit(){crypto::wipe(bytes,size);}
};

// Removes the manifest's temporary file on every exit but a committed one.
class ManifestGuard{
public:
explicit ManifestGuard(const Project& project) noexcept:project_(project){}
~ManifestGuard(){if(!committed_)discard_release_manifest(project_);}
ManifestGuard(const ManifestGuard&)=delete;
ManifestGuard& operator=(const ManifestGuard&)=delete;
void committed() noexcept{committed_=true;}
private:
const Project& project_;
bool committed_=false;
};

// The file names the build itself puts at the top of the output folder, besides the assets: the executable, the external pack and the
// DLLs the dynamic modules ship. nullopt after a module's DLLs could not be walked (reported).
std::optional<std::vector<std::string>> shipped_names(const GamePayload& payload,const fs::path& exe,ModuleDlls& module_dlls,Diagnostics& diagnostics)
{
std::vector<std::string> names{path_to_utf8(exe.filename())};
if(payload.pack_mode==PackMode::external)names.push_back(payload.game_name+".zpk");
for(const PayloadModule& module:payload.modules){
if(module.link!=LinkMode::dynamic_link)continue;
const std::vector<fs::path>* files=module_dlls.files(module.name,diagnostics);
if(!files)return std::nullopt;
for(const fs::path& file:*files)names.push_back(path_to_utf8(file.filename()));
}
return names;
}

// Everything after the compile: the asset split and the literal-path warnings, the payload object, the link, the pack (appended to
// the executable or written beside it), the signing of the executable and the DLL copies (dynamic modules and the third-party DLLs
// they import) under temporary names, the renames, the release manifest, then the plain assets. Returns 0 or 2.
int write_release(const Project& project,const ScriptRuntime& runtime,const KitLayout& kit,const std::vector<StaticLibrary>& libraries,ModuleDlls& module_dlls,const fs::path& output,const fs::path& exe,Diagnostics& diagnostics)
{
auto bytecode=runtime.save_bytecode(project.build.strip_debug_info);
if(!bytecode)return fail(diagnostics,bytecode.error());
auto expanded=expand_assets(project);
if(!expanded)return fail(diagnostics,expanded.error());
// An output folder inside the project never feeds a later build's assets.
std::erase_if(*expanded,[&output](const fs::path& asset){return is_inside(asset,output);});
const AssetSplit split=split_assets(project,*expanded);
crypto::Key key{};
const WipeOnExit key_wipe{key.data(),key.size()};
PackMode pack_mode=PackMode::none;
// No packed asset: no pack, no key, no .zodiac folder (a hello stays one plain executable).
if(!split.packed.empty()){
auto loaded=load_or_create_pack_key(project.root);
if(!loaded)return fail(diagnostics,loaded.error());
key=*loaded;
crypto::wipe(loaded->data(),loaded->size());
pack_mode=project.pack.external?PackMode::external:PackMode::embedded;
}
GamePayload payload=payload_of(project,runtime,std::move(*bytecode),pack_mode,key);
const WipeOnExit payload_key_wipe{payload.pack_key.data(),payload.pack_key.size()};
// Made before the link, so the payload names the pack it belongs to (the release refuses an external pack of another build).
if(pack_mode!=PackMode::none){
auto id=new_build_id();
if(!id)return fail(diagnostics,id.error());
payload.build_id=std::move(*id);
}
const std::optional<std::vector<std::string>> shipped=shipped_names(payload,exe,module_dlls,diagnostics);
if(!shipped)return exit_build_error;
// The compiled scripts' literal paths against what the release holds: warnings only.
check_literal_paths(project,runtime.script_files(),*expanded,*shipped,diagnostics);
std::error_code ec;
fs::create_directories(output,ec);
if(ec)return fail(diagnostics,"error: cannot create directory "+quoted_path(output)+": "+error_text(ec));
std::string serialized=serialize_payload(payload);
const WipeOnExit serialized_wipe{serialized.data(),serialized.size()};
const std::uint32_t payload_crc=pack::payload_checksum(serialized);
// With a pack the object holds the key in clear, so it is made in .zodiac/, which .zodiac/.gitignore keeps out of git whatever
// happens to the build (a crash leaves it there, never in the output folder).
const TemporaryFile object((pack_mode!=PackMode::none?project.root/".zodiac":output)/path_from_utf8(project.name+".zodpay.obj"),diagnostics);
{
std::string object_bytes=write_payload_object(serialized);
const WipeOnExit object_wipe{object_bytes.data(),object_bytes.size()};
if(!write_binary(object.path(),object_bytes))return fail(diagnostics,"error: cannot write "+quoted_path(object.path()));
}
fs::path tmp_path=exe;
tmp_path+=".tmp";
const TemporaryFile tmp(std::move(tmp_path),diagnostics);
LinkRequest request;
request.output=tmp.path();
request.inputs.reserve(libraries.size()+5);
request.inputs.push_back(object.path());
// The application manifest every game carries (the Segment Heap on Windows 10 2004 and later, and the execution level; src/platform/
// windows/application.manifest.in): asInvoker, or requireAdministrator with [release] run_as_admin. Exactly one of the two objects is
// linked, so the game holds exactly one RT_MANIFEST 1.
request.inputs.push_back(kit.release_lib/(project.release.run_as_admin?"zodiac_manifest_admin.obj":"zodiac_manifest.obj"));
request.inputs.push_back(kit.release_lib/"libzodiac_release_stub.a");
for(const StaticLibrary& library:libraries){
request.inputs.push_back(library.archive);
request.forced_symbols.push_back("zodiac_static_module_"+library.name);
}
request.inputs.push_back(kit.release_lib/"libzodiac_core_release.a");
request.inputs.push_back(kit.release_lib/"libangelscript.a");
// link_release reports its own failures.
if(!link_release(kit,request,diagnostics))return exit_build_error;
// The pack, before the signing so the signature covers it: appended to the executable (the pack written beside it first, since a
// pack file starts at its own offset 0), or kept beside it as <name>.zpk.tmp until the renames.
std::optional<TemporaryFile> pack_tmp;
ManifestGuard manifest(project);
if(pack_mode!=PackMode::none){
pack_tmp.emplace(output/path_from_utf8(project.name+".zpk.tmp"),diagnostics);
if(auto written=write_release_pack(project,key,split,pack_tmp->path(),payload.build_id);!written)return fail(diagnostics,written.error());
if(pack_mode==PackMode::embedded){
if(auto appended=append_pack(tmp.path(),pack_tmp->path(),payload_crc);!appended)return fail(diagnostics,appended.error());
}
}
const bool signing=!project.build.sign_command.empty();
if(signing){
if(const int code=sign(project.build.sign_command,tmp.path(),exe,project.root,diagnostics))return code;
}
// The dynamic modules are copied (and signed) under temporary names too, so a failure up to here leaves the old release untouched.
struct Library{
Library(fs::path source_path,std::string_view module_name,fs::path final_path,fs::path temporary_path,Diagnostics& sink):source(std::move(source_path)),module(module_name),target(std::move(final_path)),copy(std::move(temporary_path),sink){}
fs::path source;
std::string module;
fs::path target;
TemporaryFile copy;
};
// A deque never moves its elements, which TemporaryFile cannot do.
std::deque<Library> copies;
for(const PayloadModule& module:payload.modules){
if(module.link!=LinkMode::dynamic_link)continue;
// The module DLL and the third-party DLLs it needs, as walked before the modules loaded (or now, for a module found while loading).
const std::vector<fs::path>* files=module_dlls.files(module.name,diagnostics);
if(!files)return exit_build_error;
for(const fs::path& file:*files){
fs::path target=output/file.filename();
// A file another module already ships goes out once; the same name with other bytes would leave one of the modules with the wrong DLL.
const auto shipped=std::ranges::find_if(copies,[&target](const Library& library){return same_path_text(library.target,target);});
if(shipped!=copies.end()){
if(same_path_text(shipped->source,file))continue;
const std::optional<bool> same=same_contents(shipped->source,file,diagnostics);
if(!same)return exit_build_error;
if(*same)continue;
return fail(diagnostics,"error: "+quoted_path(file.filename())+" is shipped by "+in_quotes(shipped->module)+" and "+in_quotes(module.name)+" with different contents");
}
fs::path copy_path=target;
copy_path+=".tmp";
const Library& copy=copies.emplace_back(file,module.name,std::move(target),std::move(copy_path),diagnostics);
if(!copy_into(file,copy.copy.path(),diagnostics))return exit_build_error;
if(signing){
if(const int code=sign(project.build.sign_command,copy.copy.path(),copy.target,project.root,diagnostics))return code;
}
}
}
if(const int code=replace_file(tmp.path(),exe,diagnostics))return code;
for(const Library& copy:copies)if(const int code=replace_file(copy.copy.path(),copy.target,diagnostics))return code;
if(pack_mode==PackMode::external){
if(const int code=replace_file(pack_tmp->path(),output/path_from_utf8(project.name+".zpk"),diagnostics))return code;
}
// The release is in place, so the manifest now describes what was shipped (patches compare against it).
if(pack_mode!=PackMode::none){
if(auto committed=commit_release_manifest(project);!committed)return fail(diagnostics,committed.error());
manifest.committed();
}
for(const fs::path& asset:split.plain){
if(!copy_into(asset,output/asset.lexically_relative(project.root),diagnostics))return exit_build_error;
}
return exit_ok;
}

// zodiac build --patch once the project is loaded (build_command.hpp, Patches). Returns 0 or 2.
int build_patch(const Project& project,const fs::path& output,BuildOutcome& outcome,Diagnostics& diagnostics)
{
crypto::Key key{};
const WipeOnExit key_wipe{key.data(),key.size()};
// The key only where write_patch will use it: without packing or a manifest, its own error is the one that tells what is missing.
std::error_code ec;
if(project.pack.enabled&&fs::exists(project.root/".zodiac"/"release.manifest",ec)){
auto loaded=load_pack_key(project.root);
if(!loaded)return fail(diagnostics,loaded.error());
key=*loaded;
crypto::wipe(loaded->data(),loaded->size());
}
const auto patched=write_patch(project,key,output);
if(!patched)return fail(diagnostics,patched.error());
for(const std::string& warning:patched->warnings)diagnostics.report(ZODIAC_LOG_WARNING,warning);
platform::write_stdout_line("patches carry asset files only; script changes need a full build");
if(!patched->written){
platform::write_stdout_line("no files changed since the last release build; no patch written");
return exit_ok;
}
outcome.exe=output/"patches"/path_from_utf8(pack::patch_file_name(static_cast<std::uint64_t>(patched->number)));
return exit_ok;
}

// Step 1: the project to build, as build_command.hpp describes it. Reports its own errors and the lone-file warning.
std::expected<Project,int> project_of(const BuildOptions& options,Diagnostics& diagnostics)
{
const auto failed=[&diagnostics](std::string_view message){return std::unexpected(fail(diagnostics,message));};
std::error_code ec;
if(!options.config.empty()){
auto project=load_project_file(options.config);
if(!project)return failed(project.error());
if(options.target.empty())return std::move(*project);
if(!fs::is_regular_file(options.target,ec))return failed("error: script "+quoted_path(options.target)+" does not exist");
if(auto replaced=set_main_script(*project,options.target);!replaced)return failed(replaced.error());
return std::move(*project);
}
const fs::path target=options.target.empty()?fs::path("."):options.target;
// As in zodiac run, a script argument must exist, also inside a project.
const bool script=has_extension(target,".zs");
if(script&&!fs::is_regular_file(target,ec))return failed("error: script "+quoted_path(target)+" does not exist");
if(const std::optional<fs::path> root=project_root_of(target)){
auto project=load_project(*root);
if(!project)return failed(project.error());
return std::move(*project);
}
if(!script)return failed("error: zodiac.toml not found; create a project with zodiac new");
auto project=load_project_for_script(target);
if(!project)return failed(project.error());
// A zodiac.toml that appeared since the search is used by load_project_for_script, and then it is no lone file.
if(!project->file.empty())return std::move(*project);
diagnostics.report(ZODIAC_LOG_WARNING,"warning: no zodiac.toml; only the core is available (create a project file to add modules)");
return std::move(*project);
}

int build_release(const BuildOptions& options,Diagnostics& diagnostics,BuildOutcome& outcome)
{
const fs::path kit_exe=platform::executable_path();
if(kit_exe.empty())return fail(diagnostics,"error: cannot find the folder of the zodiac executable");
const KitLayout kit=kit_layout(kit_exe);
auto project=project_of(options,diagnostics);
if(!project)return project.error();
outcome.game_name=project->name;
outcome.root=project->root;
outcome.main_script=project->main_script;
outcome.used_lone_file=project->file.empty();
if(!usable_file_name(project->name))return fail(diagnostics,"error: the game name "+in_quotes(project->name)+" cannot be used as a file name; "+(outcome.used_lone_file?std::string("rename the script"):"change [game] name in "+path_to_utf8(project->file.filename())));
// Every path is absolute before the project root becomes the working directory.
const fs::path output=options.output.empty()?project->root/"release":absolute_normal(options.output);
outcome.exe=output/path_from_utf8(project->name+".exe");
if(options.patch)return build_patch(*project,output,outcome,diagnostics);
const fs::path& exe=outcome.exe;
// Spec item 7, before anything loads: a third-party DLL missing next to a dynamic module would otherwise surface as the loader's
// "module could not be found" at the compile; the build names it instead.
ModuleDlls module_dlls(options.module_dirs,project->root);
for(const std::string& name:project->dynamic_modules){
if(contains(project->static_modules,name))continue;
if(!module_dlls.check(name,diagnostics))return exit_build_error;
}
ScriptRuntime runtime(diagnostics,ModuleSearchPaths{options.module_dirs});
// As in zodiac run: global initializers run during the compile, so they see the same working directory as in run and in the release.
if(const int code=enter_root(project->root,diagnostics))return code;
// Mounted as zodiac run mounts them, for the global initializers the compile runs, but without dev mode's spelling warnings.
vfs::mount_project(runtime.vfs(),*project,false);
std::vector<StaticLibrary> libraries;
const ScriptRuntime::ModuleCheck check=[&project,&kit,&libraries,&diagnostics](std::span<const zodiac_module_info* const> loaded){return check_static_modules(*project,kit,loaded,libraries,diagnostics);};
bool compile_failed=false;
if(const int code=runtime.build(*project,check,&compile_failed)){
// As in zodiac run: a lone script has the core only, so a call into a module is the likely cause of its compile error.
if(offers_project_file(outcome.used_lone_file,compile_failed))diagnostics.report(ZODIAC_LOG_WARNING,lone_script_compile_note);
return code;
}
return write_release(*project,runtime,kit,libraries,module_dlls,output,exe,diagnostics);
}
#else
int build_release(const BuildOptions&,Diagnostics& diagnostics,BuildOutcome&)
{
// Release builds are Windows-only in this plan: the kit's link line, CRT subset and linker come from llvm-mingw.
diagnostics.report(ZODIAC_LOG_ERROR,"error: release builds are not supported on this host yet");
return exit_build_error;
}
#endif
}

int build_project(const BuildOptions& options,Diagnostics& diagnostics)
{
auto owned=std::make_unique<ResultReport>(ResultVerb::build);
const ResultReport& report=*owned;
diagnostics.add_sink(std::move(owned));
BuildOutcome outcome;
const int code=build_release(options,diagnostics,outcome);
const bool succeeded=code==0;
// Spec 8.2a: the console gets the result in every case, so nothing is lost when the window is closed (or there is no window).
platform::write_stdout_line(report.summary(succeeded,outcome.exe));
if(options.no_window||!platform::has_interactive_desktop())return code;
// A project that did not load leaves the cwd as it was, so the target still resolves as given.
const std::filesystem::path& named=options.target.empty()&&!options.config.empty()?options.config.parent_path():options.target;
const std::string name=outcome.game_name.empty()?target_name(named):outcome.game_name;
const std::filesystem::path folder=outcome.exe.parent_path();
// A lone script's build offers the project file it could have had: that script as main, no modules yet (spec item 8).
platform::ResultAction create;
if(outcome.used_lone_file)create=create_project_action(outcome.root,outcome.game_name,outcome.main_script);
platform::show_result_window("Zodiac build - "+name,L"&Build output",report.text(succeeded,outcome.exe),succeeded,succeeded?&folder:nullptr,outcome.used_lone_file?&create:nullptr);
return code;
}
}
