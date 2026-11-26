// zodiac::ScriptRuntime owns one AngelScript engine with Zodiac's array, string, dictionary and exception helpers,
// loads a project's modules into it, builds the project's scripts and runs main(). Every error goes to the Diagnostics it was given.
//
// build(): loads and registers the static and dynamic modules (all as shared libraries in dev mode, searched in the constructor's
// directories, then <project root>/plugins; the release configuration, ZODIAC_RELEASE, searches the constructor's directories only), then compiles main_script. #include paths resolve relative to the including file;
// a UTF-8 BOM is skipped. Script paths in messages are relative to Project::root with forward slashes. Returns 0, 1 (script error) or 2 (module error).
// save_bytecode(): after a successful build (or load_bytecode), the compiled module as AngelScript bytecode, with debug info (sections, lines) unless stripped.
// The bytecode is only valid against an identical registered interface: the same modules, registered in the same order (see module_order()).
// load_bytecode(): loads and registers modules exactly as build() does (names in linked use those infos, which must outlive the runtime), then loads bytecode
// into the script module and finds main() the same way. When check is given, it runs after the modules are loaded and before any of them registers,
// with the loaded infos in registration order (also when the project requests none); a non-zero result (reported by check itself) is returned and nothing more happens.
// Returns 0, 1 (a global variable failed to initialize, a script error reported as build() reports it) or 2 (module error, including bytecode that does not
// match the registered interface or is incomplete, such as a module replaced by one with the same version but another interface:
// "error: the game data does not match the modules it was built with").
// Never reads outside bytecode, which only has to stay alive during the call.
// run(): executes "int main()" or "void main()" and returns its value (0 for void), or 1 on an uncaught exception. Never exits the process.
// When main() ended through restart_as_admin starting the elevated copy (core/library/library.hpp), it returns 0 and reports nothing.
// Runtime errors are reported identically from source and from bytecode with debug info; from stripped bytecode, a location without a line ends with " (line unknown)".
// enter_root() and exit_code_of() are the start-up rules zodiac run and the release stub share.
// Threads: construct, build, load_bytecode and run on the main thread.
#pragma once
#include <core/host.hpp>
#include <core/library/library.hpp>
#include <core/module_registry.hpp>
#include <core/vfs/vfs.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

class asIScriptContext;
class asIScriptEngine;
class asIScriptFunction;
class CScriptBuilder;
struct asSMessageInfo;

namespace zodiac {
class Diagnostics;
struct Project;
class ScriptRuntime{
public:
ScriptRuntime(Diagnostics& diagnostics,ModuleSearchPaths search_paths);
~ScriptRuntime();
ScriptRuntime(const ScriptRuntime&)=delete;
ScriptRuntime& operator=(const ScriptRuntime&)=delete;
Host& host() noexcept{return host_;}
// The game's virtual file system. Its log reports to the Diagnostics; the caller mounts it (vfs::mount_project, vfs::mount_game_folders) before build or load_bytecode.
vfs::Vfs& vfs() noexcept{return vfs_;}
using ModuleCheck=std::function<int(std::span<const zodiac_module_info* const> loaded)>;
// check, when given, runs as in load_bytecode: after the modules are loaded and before any of them registers (zodiac build's static link checks).
// compile_failed, when given, is set false at the start and true only when the exit is 1 because the scripts did not compile (a section could not be added
// or the build failed); it stays false for a missing or misdeclared main(), a failed global initializer, exit 0 and exit 2.
int build(const Project& project,const ModuleCheck& check={},bool* compile_failed=nullptr);
std::expected<std::string,std::string> save_bytecode(bool strip_debug_info) const;
int load_bytecode(const Project& project,std::string_view bytecode,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check={});
// The loaded module names in registration order; invalidated by the next build or load_bytecode.
std::span<const std::string> module_order() const noexcept{return registry_.ordered_names();}
// The loaded module infos, parallel to module_order(); same lifetime.
std::span<const zodiac_module_info* const> modules() const noexcept{return registry_.ordered();}
int run();
private:
friend struct RuntimeCallbacks;
void on_message(const asSMessageInfo& info) noexcept;
int on_include(const char* include,const char* from,CScriptBuilder& builder);
int add_section(const std::filesystem::path& file,const std::string& section,CScriptBuilder& builder,std::string_view requested_by,std::string_view requested_name);
int load_modules(const Project& project,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check);
// Resets the state shared by build and load_bytecode and loads the project's modules. Returns 0, 2 or check's result.
int prepare(const Project& project,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check);
// Sets main_ from the script module "main", or reports why it cannot. Returns 0 or 1.
int find_main();
std::string section_name(const std::filesystem::path& file) const;
void report_exception();
Diagnostics& diagnostics_;
ModuleSearchPaths search_paths_;
// Teardown contract: the destructor releases the context, discards the script module and runs a full garbage collection (so no script
// object outlives its module), calls registry_.shutdown_all(), shuts the engine down, then host_ and registry_ (last, unloading module code) are destroyed in reverse declaration order.
ModuleRegistry registry_;
// Destroyed after the engine is shut down, so no script or module call can reach it afterwards.
vfs::Vfs vfs_;
asIScriptEngine* engine_;
Host host_;
asIScriptContext* context_=nullptr;
// is_admin and restart_as_admin (core/library/library.hpp): restart_as_admin acts on context_ only.
SystemLibrary system_{&context_};
asIScriptFunction* main_=nullptr;
std::filesystem::path root_;
std::string main_section_;
std::unordered_set<std::string> included_;
// True when the script module came from bytecode saved without debug info.
bool debug_info_stripped_=false;
// While set, engine messages are kept here instead of being reported (load_bytecode decides afterwards whether they are meaningful).
std::vector<std::pair<zodiac_log_level,std::string>>* held_messages_=nullptr;
};
// Makes root (dev: the project root, release: the executable's folder) the process's working directory, so relative file access behaves the same in both.
// Returns 0, or 2 after reporting "error: cannot use "<root>" as the working directory: <reason>".
int enter_root(const std::filesystem::path& root,Diagnostics& diagnostics);
// The process exit code for a value returned by run(): the value when it is 0-255 (a process status is 0-255 on every platform, POSIX masks it to 8 bits),
// otherwise 1 after reporting "warning: main() returned <value>; exit codes must be 0-255, using 1".
int exit_code_of(int result,Diagnostics& diagnostics);
}
