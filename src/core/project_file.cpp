// toml++ is only included here, with exceptions disabled: every failure comes back as a toml::parse_result or a std::expected error.
#ifndef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#endif
#include <core/project_file.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <fstream>
#include <ios>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace zodiac {
namespace {
namespace fs=std::filesystem;
using Failure=std::unexpected<std::string>;
Failure fail(std::string text)
{
return Failure(std::move(text));
}
Failure fail_line(std::string_view text)
{
std::string message;
message.reserve(text.size()+20);
message.append("zodiac.toml: error: ").append(text);
return Failure(std::move(message));
}
fs::path from_utf8(const std::string& text)
{
return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
bool valid_module_name(std::string_view name) noexcept
{
return !name.empty()&&std::ranges::all_of(name,[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});
}
void add_unique(std::vector<std::string>& list,std::string value)
{
if(std::ranges::find(list,value)==list.end())list.push_back(std::move(value));
}
// Resolves a project-relative path from the file. Returns nullopt when it is absolute, rooted, empty or leaves root.
std::optional<fs::path> inside_root(const fs::path& root,const std::string& text)
{
const fs::path relative=from_utf8(text);
if(relative.empty()||relative.has_root_path())return std::nullopt;
fs::path full=(root/relative).lexically_normal();
const fs::path inner=full.lexically_relative(root);
if(inner.empty()||inner==fs::path(".")||*inner.begin()==fs::path(".."))return std::nullopt;
return full;
}
// False for an asset pattern that could never match a project-relative '/' path: one with a backslash, an absolute one
// (leading '/' or a drive letter "C:") or one with a ".." segment.
bool usable_asset_pattern(std::string_view pattern) noexcept
{
if(pattern.contains('\\')||pattern.starts_with('/'))return false;
if(pattern.size()>=2&&pattern[1]==':'&&((pattern[0]>='A'&&pattern[0]<='Z')||(pattern[0]>='a'&&pattern[0]<='z')))return false;
for(std::size_t start=0;;)
{
const std::size_t cut=pattern.find('/',start);
if(pattern.substr(start,cut==std::string_view::npos?std::string_view::npos:cut-start)=="..")return false;
if(cut==std::string_view::npos)return true;
start=cut+1;
}
}
// Returns the table under key, an empty pointer when absent, or an error when it is something else.
std::expected<const toml::table*,std::string> section(const toml::table& root,std::string_view key)
{
const toml::node* node=root.get(key);
if(!node)return nullptr;
if(const toml::table* table=node->as_table())return table;
return fail("zodiac.toml: error: ["+std::string(key)+"] must be a table");
}
// Reads an optional array of strings into out. section_name and key are only used in the error text.
std::expected<void,std::string> string_array(const toml::table* table,std::string_view section_name,std::string_view key,std::vector<std::string>& out)
{
if(!table)return {};
const toml::node* node=table->get(key);
if(!node)return {};
const toml::array* array=node->as_array();
if(array)
{
bool all_strings=true;
for(const toml::node& element:*array)
{
const toml::value<std::string>* value=element.as_string();
if(!value)
{
all_strings=false;
break;
}
out.push_back(value->get());
}
if(all_strings)return {};
}
out.clear();
return fail("zodiac.toml: error: ["+std::string(section_name)+"] "+std::string(key)+" must be an array of strings");
}
// Reads an optional boolean into out. section_name and key are only used in the error text.
std::expected<void,std::string> boolean(const toml::table* table,std::string_view section_name,std::string_view key,bool& out)
{
if(!table)return {};
const toml::node* node=table->get(key);
if(!node)return {};
if(const toml::value<bool>* value=node->as_boolean()){
out=value->get();
return {};
}
return fail("zodiac.toml: error: ["+std::string(section_name)+"] "+std::string(key)+" must be a boolean");
}
// Reads an optional array of asset patterns into out, refusing one usable_asset_pattern refuses.
std::expected<void,std::string> pattern_array(const toml::table* table,std::string_view section_name,std::string_view key,std::vector<std::string>& out)
{
if(auto result=string_array(table,section_name,key,out);!result)return result;
for(const std::string& pattern:out)
{
if(!usable_asset_pattern(pattern))return fail("zodiac.toml: error: ["+std::string(section_name)+"] "+std::string(key)+" \""+one_line(pattern)+"\" must use / and stay inside the project");
}
return {};
}
std::expected<void,std::string> output_mode(const toml::table* table,std::string_view key,OutputMode& out)
{
if(!table)return {};
const toml::node* node=table->get(key);
if(!node)return {};
if(const toml::value<std::string>* value=node->as_string())
{
const std::string& text=value->get();
if(text=="auto")
{
out=OutputMode::automatic;
return {};
}
if(text=="always")
{
out=OutputMode::always;
return {};
}
if(text=="never")
{
out=OutputMode::never;
return {};
}
}
return fail("zodiac.toml: error: [diagnostics] "+std::string(key)+" must be \"auto\", \"always\" or \"never\"");
}
bool is_regular(const fs::path& path) noexcept
{
std::error_code ec;
const bool exists=fs::is_regular_file(path,ec);
return exists&&!ec;
}
// The whole file, or nullopt when it cannot be read completely.
std::optional<std::string> read_whole(const fs::path& path)
{
std::error_code ec;
const std::uintmax_t size=fs::file_size(path,ec);
std::ifstream in(path,std::ios::binary);
if(ec||!in)return std::nullopt;
std::string content(static_cast<std::size_t>(size),'\0');
in.read(content.data(),static_cast<std::streamsize>(size));
if(in.gcount()!=static_cast<std::streamsize>(size))return std::nullopt;
return content;
}
// A TOML basic string body: backslash, quote and control characters escaped, everything else (UTF-8) unchanged.
void append_toml_escaped(std::string& out,std::string_view text)
{
constexpr std::string_view digits="0123456789ABCDEF";
for(const char c:text){
const unsigned char byte=static_cast<unsigned char>(c);
if(c=='\\'||c=='"'){
out.push_back('\\');
out.push_back(c);
}
else if(byte<0x20||byte==0x7F){
out.append("\\u00");
out.push_back(digits[byte>>4]);
out.push_back(digits[byte&15]);
}
else out.push_back(c);
}
}
// Parses and validates content, the text of project.file; project.root and project.file are already set. Errors start with "zodiac.toml".
std::expected<Project,std::string> parse_project(Project project,const std::string& content)
{
std::error_code ec;
toml::parse_result parsed=toml::parse(content,std::string_view("zodiac.toml"));
if(!parsed)
{
const toml::parse_error& error=parsed.error();
const toml::source_position position=error.source().begin;
return fail("zodiac.toml:"+std::to_string(position.line)+":"+std::to_string(position.column)+": error: "+std::string(error.description()));
}
const toml::table& root=parsed.table();
const auto game=section(root,"game");
if(!game)return fail(std::move(game.error()));
const toml::node* main_node=*game?(*game)->get("main"):nullptr;
if(!main_node)return fail_line("[game] main is required");
const toml::value<std::string>* main_value=main_node->as_string();
if(!main_value)return fail_line("[game] main must be a string");
project.name=path_to_utf8(project.root.filename());
if(*game)
{
if(const toml::node* name_node=(*game)->get("name"))
{
const toml::value<std::string>* name_value=name_node->as_string();
if(!name_value)return fail_line("[game] name must be a string");
project.name=name_value->get();
}
}
const auto modules=section(root,"modules");
if(!modules)return fail(std::move(modules.error()));
if(auto result=string_array(*modules,"modules","static",project.static_modules);!result)return fail(std::move(result.error()));
if(auto result=string_array(*modules,"modules","dynamic",project.dynamic_modules);!result)return fail(std::move(result.error()));
for(std::vector<std::string>* list:{&project.static_modules,&project.dynamic_modules})
{
std::vector<std::string> cleaned;
cleaned.reserve(list->size());
for(std::string& name:*list)
{
if(name=="core")continue;
if(!valid_module_name(name))return fail("zodiac.toml: error: module name \""+one_line(name)+"\" is invalid");
add_unique(cleaned,std::move(name));
}
*list=std::move(cleaned);
}
for(const std::string& name:project.static_modules)
{
if(std::ranges::find(project.dynamic_modules,name)!=project.dynamic_modules.end())return fail("zodiac.toml: error: module \""+one_line(name)+"\" is listed as both static and dynamic");
}
const auto core=section(root,"core");
if(!core)return fail(std::move(core.error()));
std::vector<std::string> excluded;
if(auto result=string_array(*core,"core","exclude",excluded);!result)return fail(std::move(result.error()));
for(std::string& name:excluded)
{
if(name!="window"&&name!="keyboard")return fail("zodiac.toml: error: unknown [core] exclude entry \""+one_line(name)+"\"; valid: window, keyboard");
add_unique(project.core_exclude,std::move(name));
}
const auto diagnostics=section(root,"diagnostics");
if(!diagnostics)return fail(std::move(diagnostics.error()));
if(auto result=output_mode(*diagnostics,"console",project.diagnostics.console);!result)return fail(std::move(result.error()));
if(auto result=output_mode(*diagnostics,"dialog",project.diagnostics.dialog);!result)return fail(std::move(result.error()));
if(*diagnostics)
{
if(const toml::node* file_node=(*diagnostics)->get("file"))
{
const toml::value<std::string>* file_value=file_node->as_string();
if(!file_value)return fail_line("[diagnostics] file must be a string");
auto file=inside_root(project.root,file_value->get());
if(!file)return fail("zodiac.toml: error: [diagnostics] file \""+one_line(file_value->get())+"\" must be inside the project");
project.diagnostics.file=std::move(*file);
}
}
const auto build=section(root,"build");
if(!build)return fail(std::move(build.error()));
if(auto result=pattern_array(*build,"build","assets",project.build.assets);!result)return fail(std::move(result.error()));
if(auto result=boolean(*build,"build","strip_debug_info",project.build.strip_debug_info);!result)return fail(std::move(result.error()));
if(*build)
{
const toml::node* sign_node=(*build)->get("sign");
if(sign_node)
{
const toml::table* sign=sign_node->as_table();
if(!sign)return fail_line("[build.sign] must be a table");
if(auto result=string_array(sign,"build.sign","command",project.build.sign_command);!result)return fail(std::move(result.error()));
const bool has_placeholder=std::ranges::any_of(project.build.sign_command,[](const std::string& argument){return argument.find("{file}")!=std::string::npos;});
if(!project.build.sign_command.empty()&&!has_placeholder)return fail_line("[build.sign] command must contain {file}");
}
}
const auto files=section(root,"files");
if(!files)return fail(std::move(files.error()));
if(auto result=boolean(*files,"files","host_paths",project.files.host_paths);!result)return fail(std::move(result.error()));
const auto pack=section(root,"pack");
if(!pack)return fail(std::move(pack.error()));
for(const auto& [key,value]:{std::pair{"enabled",&project.pack.enabled},{"external",&project.pack.external},{"writable",&project.pack.writable}})
{
if(auto result=boolean(*pack,"pack",key,*value);!result)return fail(std::move(result.error()));
}
// A running executable is locked by the system, so a pack the game writes can only be the external one.
if(project.pack.writable)project.pack.external=true;
if(auto result=pattern_array(*pack,"pack","exclude",project.pack.exclude);!result)return fail(std::move(result.error()));
const auto release=section(root,"release");
if(!release)return fail(std::move(release.error()));
if(auto result=boolean(*release,"release","run_as_admin",project.release.run_as_admin);!result)return fail(std::move(result.error()));
const auto workers=section(root,"workers");
if(!workers)return fail(std::move(workers.error()));
std::vector<std::string> worker_names;
if(auto result=string_array(*workers,"workers","scripts",worker_names);!result)return fail(std::move(result.error()));
for(const std::string& worker_name:worker_names)
{
auto worker=inside_root(project.root,worker_name);
if(!worker)return fail("zodiac.toml: error: worker script \""+one_line(worker_name)+"\" must be inside the project");
const bool worker_is_file=fs::is_regular_file(*worker,ec);
if(ec||!worker_is_file)return fail("zodiac.toml: error: worker script \""+one_line(worker_name)+"\" does not exist");
if(!has_extension(*worker,".zs"))return fail("zodiac.toml: error: worker script \""+one_line(worker_name)+"\" must be a .zs file");
if(std::ranges::find(project.workers,*worker)==project.workers.end())project.workers.push_back(std::move(*worker));
}
auto main_script=inside_root(project.root,main_value->get());
if(!main_script)return fail("zodiac.toml: error: main script \""+one_line(main_value->get())+"\" must be inside the project");
const bool is_file=fs::is_regular_file(*main_script,ec);
if(ec||!is_file)return fail("zodiac.toml: error: main script \""+one_line(main_value->get())+"\" does not exist");
project.main_script=std::move(*main_script);
return project;
}
}

std::expected<Project,std::string> load_project(const fs::path& dir)
{
std::error_code ec;
Project project;
project.root=fs::absolute(dir,ec).lexically_normal();
if(ec)return fail("zodiac.toml: error: invalid project directory \""+one_line(path_to_utf8(dir))+"\"");
if(!project.root.has_filename()&&project.root.has_relative_path())project.root=project.root.parent_path();
project.file=project.root/"zodiac.toml";
if(!is_regular(project.file))return fail("zodiac.toml: error: no zodiac.toml in \""+one_line(path_to_utf8(project.root))+"\"");
const std::optional<std::string> content=read_whole(project.file);
if(!content)return fail("zodiac.toml: error: cannot read \""+one_line(path_to_utf8(project.file))+"\"");
return parse_project(std::move(project),*content);
}

std::expected<Project,std::string> load_project_file(const fs::path& config_file)
{
const auto missing=[&config_file]{return fail("error: config file \""+one_line(path_to_utf8(config_file))+"\" does not exist");};
std::error_code ec;
Project project;
project.file=fs::absolute(config_file,ec).lexically_normal();
if(ec||!project.file.has_filename()||!is_regular(project.file))return missing();
const std::optional<std::string> content=read_whole(project.file);
if(!content)return missing();
project.root=project.file.parent_path();
auto result=parse_project(std::move(project),*content);
if(result)return result;
// parse_project names the file "zodiac.toml"; the message names the file actually read.
constexpr std::string_view default_name="zodiac.toml";
std::string message=std::move(result.error());
if(message.starts_with(default_name))message.replace(0,default_name.size(),one_line(path_to_utf8(config_file.filename())));
return fail(std::move(message));
}

std::expected<void,std::string> set_main_script(Project& project,const fs::path& script_file)
{
std::error_code ec;
fs::path file=fs::absolute(script_file,ec).lexically_normal();
if(ec)file=script_file.lexically_normal();
// Each folder above the file is compared with the root through the file system, so a differently spelled root (another case on Windows,
// a link) still counts; the main script is then rebuilt below root itself, keeping every path of the project spelled alike.
std::vector<fs::path> below{file.filename()};
fs::path dir=file.parent_path();
for(;;){
const bool same=fs::equivalent(dir,project.root,ec);
if(same&&!ec){
fs::path main=project.root;
for(auto it=below.rbegin();it!=below.rend();++it)main/=*it;
project.main_script=std::move(main);
return {};
}
fs::path parent=dir.parent_path();
if(parent.empty()||parent==dir)break;
below.push_back(dir.filename());
dir=std::move(parent);
}
return fail("error: script \""+one_line(path_to_utf8(file))+"\" is outside the project folder \""+one_line(path_to_utf8(project.root))+"\"");
}

std::string project_file_text(std::string_view name,std::string_view main_relative,std::span<const std::string> static_modules,std::span<const std::string> dynamic_modules)
{
constexpr std::string_view head="# Zodiac project file.\n[game]\nname = \"";
constexpr std::string_view after_name="\"\nmain = \"";
constexpr std::string_view after_main="\"\n\n[modules]\n# Modules this game uses, for example \"example\". Both lists are searched as shared libraries.\nstatic = [";
constexpr std::string_view between="]\ndynamic = [";
constexpr std::string_view tail="]\n";
std::size_t size=head.size()+after_name.size()+after_main.size()+between.size()+tail.size()+name.size()+main_relative.size();
for(const auto list:{static_modules,dynamic_modules})for(const std::string& module:list)size+=module.size()+4;
std::string text;
text.reserve(size);
text.append(head);
append_toml_escaped(text,name);
text.append(after_name);
append_toml_escaped(text,main_relative);
text.append(after_main);
const auto append_list=[&text](std::span<const std::string> list){
for(std::size_t i=0;i<list.size();++i){
if(i)text.append(", ");
text.push_back('"');
append_toml_escaped(text,list[i]);
text.push_back('"');
}
};
append_list(static_modules);
text.append(between);
append_list(dynamic_modules);
text.append(tail);
return text;
}

std::optional<fs::path> find_project_root(const fs::path& script_file)
{
std::error_code ec;
fs::path dir=fs::absolute(script_file,ec).lexically_normal();
if(ec)return std::nullopt;
dir=dir.parent_path();
for(;;)
{
const bool found=fs::is_regular_file(dir/"zodiac.toml",ec);
if(found&&!ec)return dir;
// The filesystem root is its own parent.
fs::path parent=dir.parent_path();
if(parent.empty()||parent==dir)return std::nullopt;
dir=std::move(parent);
}
}

std::expected<Project,std::string> load_project_for_script(const fs::path& script_file)
{
std::error_code ec;
fs::path file=fs::absolute(script_file,ec).lexically_normal();
if(ec)return fail("zodiac.toml: error: invalid script path \""+one_line(path_to_utf8(script_file))+"\"");
if(const std::optional<fs::path> root=find_project_root(file)){
auto project=load_project(*root);
if(!project)return project;
project->main_script=std::move(file);
return project;
}
Project project;
project.root=file.parent_path();
project.main_script=file;
project.name=path_to_utf8(file.stem());
return project;
}

std::expected<std::vector<fs::path>,std::string> expand_assets(const Project& project)
{
std::vector<fs::path> files;
if(project.build.assets.empty())return files;
const fs::path release_dir=project.root/"release";
// The build's own state (the pack key, the last release's manifest): never an asset, so a key can never be packed or copied out.
const fs::path state_dir=project.root/".zodiac";
const fs::path manifest=project.root/"zodiac.toml";
std::error_code ec;
fs::recursive_directory_iterator it(project.root,fs::directory_options::none,ec);
if(ec)return fail("zodiac.toml: error: cannot read directory \""+one_line(path_to_utf8(project.root))+"\": "+one_line(error_text(ec)));
for(const fs::recursive_directory_iterator end;it!=end;it.increment(ec))
{
const fs::path& path=it->path();
// Compared as the file system compares names (case-insensitively on Windows): "--config AYAR.TXT" names the same file as ayar.txt on disk.
// Every one of those paths is directly in the root (the project file's folder is the root), so deeper entries skip the comparisons.
if(it.depth()==0)
{
if(same_path_text(path,release_dir)||same_path_text(path,state_dir))
{
it.disable_recursion_pending();
continue;
}
if(same_path_text(path,manifest)||(!project.file.empty()&&same_path_text(path,project.file)))continue;
}
fs::file_status status=it->status(ec);
// The entry's cached type does not follow every kind of link (a Windows junction is cached as a folder, even when its
// target is gone), so a folder is asked again through the filesystem before the walk enters it.
if(!ec&&fs::is_directory(status))status=fs::status(path,ec);
if(ec)
{
// Only a link whose target is gone (nothing to copy) is skipped; any other unreadable entry stops the build.
if(status.type()!=fs::file_type::not_found)return fail("zodiac.toml: error: cannot read \""+one_line(path_to_utf8(path))+"\": "+one_line(error_text(ec)));
ec.clear();
}
if(status.type()==fs::file_type::not_found)
{
it.disable_recursion_pending();
continue;
}
if(!fs::is_regular_file(status))continue;
const std::u8string relative=path.lexically_relative(project.root).generic_u8string();
const std::string_view text(reinterpret_cast<const char*>(relative.data()),relative.size());
if(std::ranges::any_of(project.build.assets,[text](const std::string& pattern){return glob_match(pattern,text);}))files.push_back(path);
}
// A failed increment turns the iterator into end and leaves the reason in ec.
if(ec)return fail("zodiac.toml: error: cannot read directory \""+one_line(path_to_utf8(project.root))+"\": "+one_line(error_text(ec)));
std::ranges::sort(files);
files.erase(std::ranges::unique(files).begin(),files.end());
return files;
}

bool packed_in_release(const PackConfig& pack,std::string_view relative) noexcept
{
if(!pack.enabled)return false;
constexpr std::string_view extension=".zpk";
if(relative.size()>=extension.size())
{
bool zpk=true;
const std::string_view tail=relative.substr(relative.size()-extension.size());
for(std::size_t i=0;i<extension.size();++i)
{
char c=tail[i];
if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
if(c!=extension[i])zpk=false;
}
if(zpk)return false;
}
return std::ranges::none_of(pack.exclude,[relative](const std::string& pattern){return glob_match(pattern,relative);});
}
}
