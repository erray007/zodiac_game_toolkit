// The zodiac command line: new, run, build, modules, help and --version. A thin front end over zodiac_core; the platform layer owns the entry point.
// Normal output (help, listings, --version, the build summary, a failed run's summary) goes to standard output through platform::write_stdout_line; every error goes through Diagnostics.
#include <cli/build_command.hpp>
#include <cli/build_report.hpp>
#include <cli/templates.hpp>
#include <core/diagnostics.hpp>
#include <core/dynamic_library.hpp>
#include <core/module_registry.hpp>
#include <core/path_utf8.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <core/text.hpp>
#include <core/vfs/startup.hpp>
#include <platform/console.hpp>
#include <platform/elevation.hpp>
#include <platform/process.hpp>
#include <platform/result_window.hpp>
#include <zodiac/plugin.h>
#include <algorithm>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef ZODIAC_VERSION
#error ZODIAC_VERSION must be defined by the build (the project version)
#endif

namespace zodiac::cli {
namespace {
namespace fs=std::filesystem;

constexpr int exit_ok=0;
constexpr int exit_usage_error=2;

struct Options{
std::string command;
std::vector<std::string> positional;
std::vector<fs::path> module_dirs;
fs::path log_file;
// run and build: --config, already absolute (resolved against the working directory zodiac started in); empty when not given.
fs::path config;
// build only: --output.
fs::path output;
bool output_given=false;
// run and build: --no-window.
bool no_window=false;
// Set when the command line is malformed; reported through Diagnostics once it exists.
std::string error;
};

std::string in_quotes(std::string_view text)
{
std::string result;
result.reserve(text.size()+2);
result.push_back('"');
result.append(text);
result.push_back('"');
return result;
}

void fail(Diagnostics& diagnostics,std::string_view message) noexcept
{
diagnostics.report(ZODIAC_LOG_ERROR,message);
}

Options parse_arguments(std::span<const std::string> args)
{
Options options;
for(std::size_t i=0;i<args.size();++i){
const std::string& arg=args[i];
if(arg=="--module-dir"||arg=="--log-file"||arg=="--output"){
if(i+1>=args.size()){
options.error="error: "+arg+" needs a value";
return options;
}
const std::string& value=args[++i];
if(arg=="--module-dir")options.module_dirs.push_back(path_from_utf8(value));
else if(arg=="--log-file")options.log_file=path_from_utf8(value);
else{
options.output=path_from_utf8(value);
options.output_given=true;
}
continue;
}
if(arg=="--no-window"){
options.no_window=true;
continue;
}
if(arg=="--config"||arg.starts_with("--config=")){
if(!options.config.empty()){
options.error="error: --config given twice";
return options;
}
std::string_view value;
if(arg.size()>8)value=std::string_view(arg).substr(9);
else if(i+1<args.size()&&!args[i+1].starts_with("--"))value=args[++i];
// A value starting with "--" is the next option, not a file name.
if(value.empty()||value.starts_with("--")){
options.error="error: --config needs a value";
return options;
}
// Before anything can change the working directory, so a relative path means the folder zodiac started in.
const fs::path config=path_from_utf8(value);
std::error_code error;
fs::path absolute=fs::absolute(config,error);
options.config=error?config:absolute.lexically_normal();
continue;
}
if(options.command.empty()&&(arg=="--version"||arg=="--help"||arg=="-h")){
options.command=arg=="--version"?"version":"help";
continue;
}
if(arg.size()>1&&arg.front()=='-'){
options.error="error: unknown option "+in_quotes(arg)+"; run zodiac help";
return options;
}
if(options.command.empty())options.command=arg;
else options.positional.push_back(arg);
}
return options;
}

// Adds the log file sink requested with --log-file. False (already reported) when it cannot be opened.
bool add_log_file(Diagnostics& diagnostics,const fs::path& file)
{
return file.empty()||diagnostics.add_file_sink(file);
}

// Absolute, so the search is unaffected when run enters the project root.
std::vector<fs::path> search_dirs(const Options& options)
{
std::vector<fs::path> dirs;
dirs.reserve(options.module_dirs.size()+1);
for(const fs::path& dir:options.module_dirs){
std::error_code error;
fs::path absolute=fs::absolute(dir,error);
dirs.push_back(error?dir:absolute.lexically_normal());
}
const fs::path exe=platform::executable_path();
if(!exe.empty())dirs.push_back((exe.parent_path()/".."/"modules").lexically_normal());
return dirs;
}

int print_help()
{
platform::write_stdout_line("zodiac new <dir>: create a new game project in an empty directory");
platform::write_stdout_line("zodiac run [project_dir|file.zs]: build and run the game in project_dir or a single script (default: the current directory)");
platform::write_stdout_line("zodiac build [project_dir|file.zs]: build a release of the game (default output: <project>/release)");
platform::write_stdout_line("zodiac modules: list the modules found in the module directories");
platform::write_stdout_line("zodiac help: show this list");
platform::write_stdout_line("zodiac --version: show the version");
platform::write_stdout_line("options for run, build and modules: --module-dir <dir> adds a module directory (repeatable), --log-file <path> also writes messages to a file");
platform::write_stdout_line("options for run and build: --config <file> (or --config=<file>) names the project file, of any name; its folder is the project root, and a script given with it replaces main");
platform::write_stdout_line("options for run and build: --no-window reports on the console only (without it, build always and run after an error also show the result in a window when a desktop is available)");
platform::write_stdout_line("options for build: --output <dir> writes the release there");
platform::write_stdout_line("exit codes: 0 success, 1 script error, 2 usage/project/module error, otherwise the value returned by int main() (0-255)");
return exit_ok;
}

int print_version()
{
platform::write_stdout_line("zodiac " ZODIAC_VERSION);
return exit_ok;
}

// True when the positional argument names a script (a .zs file name, whether or not it exists).
bool names_script(const Options& options)
{
return !options.positional.empty()&&has_extension(path_from_utf8(options.positional.front()),".zs");
}

// --config names the project, so a project folder beside it is refused. False (already reported) on that conflict.
bool check_config_target(const Options& options,Diagnostics& diagnostics)
{
if(options.config.empty()||options.positional.empty()||names_script(options))return true;
fail(diagnostics,"error: --config cannot be combined with a project folder");
return false;
}

// What run_game tells zodiac run besides the exit code.
struct RunInfo{
// Set once the project loads.
std::string game_name;
// Set exactly when lone_script_compile_note was reported (offers_project_file); empty otherwise.
std::optional<platform::ResultAction> create;
};

// Loads, builds and runs the game; info is filled as RunInfo says. The caller has checked the command line and opened --log-file.
// A script without a project file (and without --config) runs with the core only; when its scripts fail to compile (exit 1), the warning-level
// line lone_script_compile_note follows the compiler's errors (not for a missing main() or a failing global initializer).
int run_game(const Options& options,Diagnostics& diagnostics,RunInfo& info)
{
// A copy restart_as_admin started waits until the process that started it has finished writing (platform/elevation.hpp).
platform::wait_for_restart();
const fs::path directory=options.positional.empty()?fs::path("."):path_from_utf8(options.positional.front());
const std::vector<fs::path> dirs=search_dirs(options);
std::expected<Project,std::string> project=std::unexpected(std::string());
if(names_script(options)){
std::error_code error;
if(!fs::is_regular_file(directory,error)){
fail(diagnostics,"error: script "+in_quotes(options.positional.front())+" does not exist");
return exit_usage_error;
}
if(options.config.empty())project=load_project_for_script(directory);
else{
project=load_project_file(options.config);
if(project){
if(auto replaced=set_main_script(*project,directory);!replaced)project=std::unexpected(std::move(replaced.error()));
}
}
}
else project=options.config.empty()?load_project(directory):load_project_file(options.config);
if(!project){
fail(diagnostics,project.error());
return exit_usage_error;
}
info.game_name=project->name;
if(options.log_file.empty()&&!project->diagnostics.file.empty()&&!diagnostics.add_file_sink(project->diagnostics.file))return exit_usage_error;
// Every path from the command line is resolved (the module directories) or already used (the log file is open, the project root is absolute)
// before the project root becomes the working directory, which happens before any module registers or script code runs, exactly as in a release.
ScriptRuntime runtime(diagnostics,ModuleSearchPaths{dirs});
if(const int code=enter_root(project->root,diagnostics);code!=0)return code;
// The game's files before anything compiles (global initializers run during the build): the project at /, the player's data at data:/,
// in dev mode, which warns about names spelled in another case than on disk.
vfs::mount_project(runtime.vfs(),*project,true);
bool compile_failed=false;
if(const int code=runtime.build(*project,{},&compile_failed);code!=0){
// A lone script has the core only (load_project_for_script), so a call into a module is the likely cause of its compile error.
if(offers_project_file(project->file.empty(),compile_failed)){
diagnostics.report(ZODIAC_LOG_WARNING,lone_script_compile_note);
info.create=create_project_action(project->root,project->name,project->main_script);
}
return code;
}
return exit_code_of(runtime.run(),diagnostics);
}

// zodiac run. Once the command line is checked, every message of the run (project loading, modules, compile, the game, the shutdown) is also
// tallied; when at least one ERROR-level message was reported (only engine errors are: a game's own non-zero exit code is none), the summary
// "Run failed: <N> errors" is the last line on standard output and, with an interactive desktop, the result window shows it with every message,
// titled "Zodiac run - <game name>" (the target's own name when no project loaded), without "Open output folder". Its only action is "Create project
// file" (create_project_action), offered exactly when lone_script_compile_note was reported; every other failure has none.
// --no-window keeps the run as it was: console only, no summary. The exit code is the run's in every case.
int run_project(const Options& options,Diagnostics& diagnostics)
{
if(options.positional.size()>1){
fail(diagnostics,"error: run takes at most one project directory or script; run zodiac help");
return exit_usage_error;
}
if(!check_config_target(options,diagnostics))return exit_usage_error;
// --log-file goes first so that a project file error is captured as well.
if(!add_log_file(diagnostics,options.log_file))return exit_usage_error;
RunInfo info;
if(options.no_window)return run_game(options,diagnostics,info);
auto owned=std::make_unique<ResultReport>(ResultVerb::run);
const ResultReport& report=*owned;
diagnostics.add_sink(std::move(owned));
const int code=run_game(options,diagnostics,info);
if(report.errors()==0)return code;
platform::write_stdout_line(report.summary(false,{}));
if(!platform::has_interactive_desktop())return code;
if(info.game_name.empty())info.game_name=target_name(!options.positional.empty()?path_from_utf8(options.positional.front()):!options.config.empty()?options.config.parent_path():fs::path());
platform::show_result_window("Zodiac run - "+info.game_name,L"&Run output",report.text(false,{}),false,nullptr,info.create?&*info.create:nullptr);
return code;
}

int build_release(const Options& options,Diagnostics& diagnostics)
{
if(options.positional.size()>1){
fail(diagnostics,"error: build takes at most one project directory or script; run zodiac help");
return exit_usage_error;
}
if(!check_config_target(options,diagnostics))return exit_usage_error;
if(!add_log_file(diagnostics,options.log_file))return exit_usage_error;
BuildOptions build;
if(!options.positional.empty())build.target=path_from_utf8(options.positional.front());
build.config=options.config;
build.output=options.output;
build.module_dirs=search_dirs(options);
build.no_window=options.no_window;
return build_project(build,diagnostics);
}

struct ModuleLine{
std::string name;
std::string text;
};

std::string_view or_empty(const char* text) noexcept
{
return text?std::string_view(text):std::string_view();
}

// Loads the library just far enough to read its info; nothing is registered. False when the file is not a usable module (already reported).
bool read_module(const fs::path& file,Diagnostics& diagnostics,std::vector<ModuleLine>& lines)
{
const std::string shown=path_to_utf8(file);
auto library=DynamicLibrary::open(file);
if(!library){
fail(diagnostics,"error: cannot read module "+in_quotes(shown)+": "+library.error());
return false;
}
using EntryFunction=const zodiac_module_info*(*)(void);
void* symbol=library->symbol(ZODIAC_MODULE_ENTRY_NAME);
if(!symbol){
fail(diagnostics,"error: cannot read module "+in_quotes(shown)+": it has no "+ZODIAC_MODULE_ENTRY_NAME+" entry");
return false;
}
const zodiac_module_info* info=reinterpret_cast<EntryFunction>(symbol)();
if(!info){
fail(diagnostics,"error: cannot read module "+in_quotes(shown)+": it returned no module info");
return false;
}
if(!module_info_provides(*info,module_info_v1_size)){
fail(diagnostics,"error: cannot read module "+in_quotes(shown)+": it has an invalid info structure");
return false;
}
// The strings live in the library, so they are copied before it is unloaded.
ModuleLine line;
line.name=or_empty(info->name);
line.text.reserve(line.name.size()+shown.size()+32);
line.text.append(line.name).push_back(' ');
line.text.append(or_empty(info->version)).push_back(' ');
line.text.append(or_empty(info->license)).append(" (").append(shown).push_back(')');
lines.push_back(std::move(line));
return true;
}

int list_modules(const Options& options,Diagnostics& diagnostics)
{
if(!options.positional.empty()){
fail(diagnostics,"error: modules takes no arguments; run zodiac help");
return exit_usage_error;
}
if(!add_log_file(diagnostics,options.log_file))return exit_usage_error;
std::vector<ModuleLine> lines;
const std::vector<fs::path> dirs=search_dirs(options);
for(const InstalledModule& module:find_installed_modules(dirs))read_module(module.file,diagnostics,lines);
std::ranges::sort(lines,[](const ModuleLine& a,const ModuleLine& b){return a.name!=b.name?a.name<b.name:a.text<b.text;});
for(const ModuleLine& line:lines)platform::write_stdout_line(line.text);
return exit_ok;
}

bool write_file(const fs::path& file,std::string_view text,Diagnostics& diagnostics)
{
std::ofstream out(file,std::ios::binary|std::ios::trunc);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
out.close();
if(out)return true;
fail(diagnostics,"error: cannot write "+in_quotes(path_to_utf8(file)));
return false;
}

int create_project(const Options& options,Diagnostics& diagnostics)
{
if(options.positional.size()!=1){
fail(diagnostics,options.positional.empty()?"error: new needs a directory; run zodiac help":"error: new takes one directory; run zodiac help");
return exit_usage_error;
}
if(!add_log_file(diagnostics,options.log_file))return exit_usage_error;
const std::string& shown=options.positional.front();
const fs::path directory=path_from_utf8(shown);
std::error_code error;
const bool exists=fs::exists(directory,error);
if(exists){
if(!fs::is_directory(directory,error)){
fail(diagnostics,"error: "+in_quotes(shown)+" is not a directory");
return exit_usage_error;
}
if(fs::directory_iterator(directory,error)!=fs::directory_iterator()){
fail(diagnostics,"error: directory "+in_quotes(shown)+" is not empty");
return exit_usage_error;
}
if(error){
fail(diagnostics,"error: cannot read directory "+in_quotes(shown)+": "+error_text(error));
return exit_usage_error;
}
}
else{
error.clear();
fs::create_directories(directory,error);
if(error){
fail(diagnostics,"error: cannot create directory "+in_quotes(shown)+": "+error_text(error));
return exit_usage_error;
}
}
// The game name is the directory's own name, also for ".", "..", trailing separators and relative paths.
fs::path absolute=fs::absolute(directory,error).lexically_normal();
if(!absolute.has_filename())absolute=absolute.parent_path();
const std::string name=path_to_utf8(absolute.filename());
if(!write_file(directory/"zodiac.toml",project_file_text(name.empty()?std::string_view("game"):std::string_view(name),"main.zs",{},{}),diagnostics))return exit_usage_error;
if(!write_file(directory/"main.zs",main_script,diagnostics))return exit_usage_error;
platform::write_stdout_line("created project "+in_quotes(shown));
return exit_ok;
}
}

int run(std::span<const std::string> args)
{
platform::disable_error_dialogs();
platform::setup_utf8_console();
Diagnostics diagnostics;
bool console_ready=false;
try{
const Options options=parse_arguments(args);
// The console sink goes first: a log file that cannot be opened is reported to the sinks added before it.
diagnostics.add_sink(std::make_unique<ConsoleSink>());
console_ready=true;
if(!options.error.empty()){
fail(diagnostics,options.error);
return exit_usage_error;
}
if(options.command.empty()){
fail(diagnostics,"error: no command given; run zodiac help");
return exit_usage_error;
}
if(options.output_given&&options.command!="build"){
fail(diagnostics,"error: --output applies to build only; run zodiac help");
return exit_usage_error;
}
if(options.no_window&&options.command!="run"&&options.command!="build"){
fail(diagnostics,"error: --no-window applies to run and build only; run zodiac help");
return exit_usage_error;
}
if(!options.config.empty()&&options.command!="run"&&options.command!="build"){
fail(diagnostics,"error: --config applies to run and build only; run zodiac help");
return exit_usage_error;
}
if(options.command=="help")return print_help();
if(options.command=="version")return print_version();
if(options.command=="run")return run_project(options,diagnostics);
if(options.command=="build")return build_release(options,diagnostics);
if(options.command=="modules")return list_modules(options,diagnostics);
if(options.command=="new")return create_project(options,diagnostics);
fail(diagnostics,"error: unknown command "+in_quotes(options.command)+"; run zodiac help");
return exit_usage_error;
}
catch(...){
// Nothing may reach std::terminate: on Windows that could show an abort dialog.
std::string message="error: internal error: ";
try{
try{
throw;
}
catch(const std::exception& exception){
message.append(exception.what());
}
catch(...){
message.append("unknown exception");
}
}
catch(...){
message="error: internal error";
}
if(console_ready)diagnostics.report(ZODIAC_LOG_ERROR,message);
else platform::write_stderr_line(message);
return 1;
}
}
}
