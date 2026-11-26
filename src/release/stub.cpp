#include <release/stub.hpp>
#include <core/diagnostics.hpp>
#include <core/module_registry.hpp>
#include <core/path_utf8.hpp>
#include <core/payload.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <core/static_modules.hpp>
#include <core/vfs/startup.hpp>
#include <zodiac/plugin.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace zodiac::release {
namespace {
namespace fs=std::filesystem;
constexpr int exit_script_error=1;
constexpr int exit_start_error=2;

std::string_view or_empty(const char* text) noexcept
{
return text?std::string_view(text):std::string_view();
}

std::string module_message(std::string_view name,std::string_view rest)
{
std::string line="error: module \"";
line.append(name).append("\" ").append(rest);
return line;
}

const PayloadModule* find_module(const GamePayload& payload,std::string_view name) noexcept
{
const auto found=std::ranges::find(payload.modules,name,&PayloadModule::name);
return found==payload.modules.end()?nullptr:&*found;
}

// Builds the project a release runs: the game folder as root, the stored main section, and the requested names split by their stored link mode.
// Returns 0, or 2 after reporting why the payload cannot describe a project.
int project_of(const GamePayload& payload,const fs::path& root,Project& project,const PayloadScript*& main,Diagnostics& diagnostics)
{
main=nullptr;
for(const PayloadScript& script:payload.scripts){
if(!script.is_main)continue;
if(main){
diagnostics.report(ZODIAC_LOG_ERROR,"error: the game data is damaged (more than one main script)");
return exit_start_error;
}
main=&script;
}
if(!main){
diagnostics.report(ZODIAC_LOG_ERROR,"error: the game data is damaged (no main script)");
return exit_start_error;
}
project.root=root;
project.name=payload.game_name;
project.main_script=(root/path_from_utf8(main->section)).lexically_normal();
for(const std::string& name:payload.requested){
const PayloadModule* module=find_module(payload,name);
if(!module){
diagnostics.report(ZODIAC_LOG_ERROR,"error: the game data is damaged (module \""+name+"\" is requested but not listed)");
return exit_start_error;
}
(module->link==LinkMode::static_link?project.static_modules:project.dynamic_modules).push_back(name);
}
project.core_exclude=payload.core_exclude;
project.diagnostics=payload.diagnostics;
project.files.host_paths=payload.host_paths;
return 0;
}

// Checks that every listed module is present before anything is loaded: a static one linked into the executable (collected into linked,
// the only infos the registry may use), a dynamic one as a shared library in the game folder. Returns 0, or 2 after reporting the first missing one.
int collect_modules(const GamePayload& payload,const fs::path& root,std::vector<const zodiac_module_info*>& linked,Diagnostics& diagnostics)
{
const std::span<const zodiac_module_info* const> available=linked_static_modules();
for(const PayloadModule& module:payload.modules){
bool present=false;
if(module.link==LinkMode::static_link){
const auto info=std::ranges::find_if(available,[&module](const zodiac_module_info* candidate){return candidate&&or_empty(candidate->name)==module.name;});
present=info!=available.end();
if(present)linked.push_back(*info);
}
else{
std::error_code error;
present=fs::is_regular_file(root/module_file_name(module.name),error);
}
if(!present){
diagnostics.report(ZODIAC_LOG_ERROR,module_message(module.name,"is missing"));
return exit_start_error;
}
}
return 0;
}

// The loaded modules must be exactly the payload's, with the stored versions, in the stored order: bytecode is only valid against the interface it was compiled with.
int check_modules(const GamePayload& payload,std::span<const zodiac_module_info* const> loaded,Diagnostics& diagnostics)
{
for(const zodiac_module_info* info:loaded){
const PayloadModule* expected=find_module(payload,or_empty(info->name));
const std::string_view found=or_empty(info->version);
if(!expected||expected->version==found)continue;
std::string rest="is version ";
rest.append(found).append(", but the game was built with ").append(expected->version);
diagnostics.report(ZODIAC_LOG_ERROR,module_message(expected->name,rest));
return exit_start_error;
}
const bool same=std::ranges::equal(loaded,payload.modules,[](const zodiac_module_info* info,const PayloadModule& module){return or_empty(info->name)==module.name;});
if(same)return 0;
diagnostics.report(ZODIAC_LOG_ERROR,"error: the modules were registered in a different order than when the game was built");
return exit_start_error;
}

// sinks_ready is set once the sinks follow the payload's settings (or the console was added to report a damaged payload).
int run_game(std::string_view payload_bytes,const fs::path& executable,Diagnostics& diagnostics,bool& sinks_ready)
{
auto payload=parse_payload(payload_bytes);
if(!payload){
// Without readable settings the console is the only place the reason can go.
diagnostics.add_sink(std::make_unique<ConsoleSink>());
sinks_ready=true;
diagnostics.report(ZODIAC_LOG_ERROR,payload.error());
return exit_start_error;
}
// Console first: a log file that cannot be opened is reported to the sinks added before it.
if(payload->diagnostics.console!=OutputMode::never)diagnostics.add_sink(std::make_unique<ConsoleSink>());
sinks_ready=true;
const fs::path root=executable.parent_path().lexically_normal();
if(root.empty()){
diagnostics.report(ZODIAC_LOG_ERROR,"error: cannot find the folder of the game executable");
return exit_start_error;
}
// The payload stores the log file relative to the game folder; the project carries the resolved path, as load_project does in dev.
const fs::path log_file=payload->diagnostics.file.empty()?fs::path():(root/payload->diagnostics.file).lexically_normal();
if(!log_file.empty()&&!diagnostics.add_file_sink(log_file))return exit_start_error;
Project project;
const PayloadScript* main=nullptr;
if(const int code=project_of(*payload,root,project,main,diagnostics))return code;
project.diagnostics.file=log_file;
std::vector<const zodiac_module_info*> linked;
if(const int code=collect_modules(*payload,root,linked,diagnostics))return code;
if(const int code=enter_root(root,diagnostics))return code;
ScriptRuntime runtime(diagnostics,ModuleSearchPaths{{root}});
// The same mounts as zodiac run, for the executable's folder, before global initializers run: release mode, host paths as [files] host_paths was at build time.
vfs::mount_game_folders(runtime.vfs(),root,payload->game_name,payload->host_paths,false);
const ScriptRuntime::ModuleCheck check=[&payload,&diagnostics](std::span<const zodiac_module_info* const> loaded){return check_modules(*payload,loaded,diagnostics);};
if(const int code=runtime.load_bytecode(project,main->bytecode,linked,check))return code;
return exit_code_of(runtime.run(),diagnostics);
}

// Runs body(diagnostics,sinks_ready) and turns any exception into one reported line and exit code 1.
template<class Body>
int guarded(Body&& body) noexcept
{
Diagnostics diagnostics;
bool sinks_ready=false;
try{
return body(diagnostics,sinks_ready);
}
catch(...){
// Nothing may reach std::terminate: on Windows that could show an abort dialog.
try{
std::string message="error: internal error: ";
try{
throw;
}
catch(const std::exception& exception){
message.append(exception.what());
}
catch(...){
message.append("unknown exception");
}
if(!sinks_ready)diagnostics.add_sink(std::make_unique<ConsoleSink>());
diagnostics.report(ZODIAC_LOG_ERROR,message);
}
catch(...){
}
return exit_script_error;
}
}
}

int start(std::string_view payload_bytes,const std::filesystem::path& executable)
{
return guarded([payload_bytes,&executable](Diagnostics& diagnostics,bool& sinks_ready){return run_game(payload_bytes,executable,diagnostics,sinks_ready);});
}

int start_linked(const unsigned char* begin,const unsigned char* end,const std::filesystem::path& executable)
{
constexpr std::size_t length_field=8;
const std::size_t available=begin&&end>begin?static_cast<std::size_t>(end-begin):0;
std::uint64_t size=0;
if(available>=length_field)for(std::size_t i=0;i<length_field;++i)size|=static_cast<std::uint64_t>(begin[i])<<(8*i);
if(available<length_field||size!=available-length_field){
return guarded([](Diagnostics& diagnostics,bool& sinks_ready){
diagnostics.add_sink(std::make_unique<ConsoleSink>());
sinks_ready=true;
diagnostics.report(ZODIAC_LOG_ERROR,"error: the game data is damaged (size mismatch)");
return exit_start_error;
});
}
return start(std::string_view(reinterpret_cast<const char*>(begin+length_field),available-length_field),executable);
}
}
