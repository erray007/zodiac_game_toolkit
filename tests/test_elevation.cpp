// The command-line quoting helper (platform/command_line.hpp) against the CommandLineToArgvW rules, on every platform and, on Windows,
// against CommandLineToArgvW itself; and the script functions is_admin and restart_as_admin (core/library/library.hpp) through a
// ScriptRuntime. restart_as_admin is only called where it cannot start anything: elevated (CI runners are), from a global variable's
// initializer, or on POSIX, which has no elevation. A real UAC prompt is never shown, and no elevated game is ever started.
// In a ZODIAC_TEST_HOOKS tree, test_hook_elevation stands in for the platform, so the paths an elevated runner never takes run too:
// started (main ends normally, the modules shut down after it, nothing after the call runs), declined, failed, and the guard's
// refusals (a global initializer, another thread, a call nested in main by a sort's comparator).
// The restart guard: its name everywhere; on Windows, a process holding it (restart_guard_probe) and this one waiting for it, and the
// arguments a copy gets, rebuilt from the probe's __wargv for a command line the UCRT and CommandLineToArgvW read differently.
#include <core/diagnostics.hpp>
#include <core/library/library.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <doctest/doctest.h>
#include <platform/command_line.hpp>
#include <platform/elevation.hpp>
#include <angelscript.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs=std::filesystem;
// The arguments a command line holds by the rules append_argument inverts (CommandLineToArgvW and the UCRT, after the program
// name): written from the documentation, independent of the helper.
std::vector<std::string> parse_arguments(std::string_view line)
{
std::vector<std::string> out;
std::size_t i=0;
for(;;){
while(i<line.size()&&(line[i]==' '||line[i]=='\t'))++i;
if(i==line.size())return out;
std::string argument;
bool quoted=false;
for(;i<line.size();){
const char c=line[i];
if(c=='\\'){
std::size_t run=0;
while(i<line.size()&&line[i]=='\\'){
++run;
++i;
}
if(i<line.size()&&line[i]=='"'){
argument.append(run/2,'\\');
if(run%2){
argument.push_back('"');
++i;
}
}
else argument.append(run,'\\');
continue;
}
if(c=='"'){
// Inside quotes, "" is a literal quote (the UCRT's rule; append_argument never writes it).
if(quoted&&i+1<line.size()&&line[i+1]=='"'){
argument.push_back('"');
i+=2;
continue;
}
quoted=!quoted;
++i;
continue;
}
if(!quoted&&(c==' '||c=='\t'))break;
argument.push_back(c);
++i;
}
out.push_back(std::move(argument));
}
}
std::string quoted_argument(std::string_view argument)
{
std::string line;
zodiac::platform::append_argument(line,argument);
return line;
}
std::string joined(const std::vector<std::string>& arguments)
{
std::size_t room=0;
for(const std::string& argument:arguments)room+=zodiac::platform::argument_room(argument.size());
std::string line;
line.reserve(room);
const std::size_t capacity=line.capacity();
for(std::size_t i=0;i<arguments.size();++i){
if(i)line.push_back(' ');
zodiac::platform::append_argument(line,std::string_view(arguments[i]));
}
// argument_room is enough: the line never grew past what was reserved.
CHECK(line.capacity()==capacity);
return line;
}
// Spaces, tabs, quotes and backslashes in every arrangement the rules treat differently, an empty argument, and non-ASCII text.
const std::vector<std::string> kArguments{
"", "plain", "two words", " ", "\t", "tab\there", "new\nline", "vertical\vtab", "quo\"te", "\"", "\"\"", "\\", "\\\\", "back\\slash",
"trail\\", "trail space\\", "trail space\\\\", "a\\\"b", "a\\\\\"b", "\\\"", "\\\\\"", "\"\\", "C:\\Program Files\\Zodiac\\",
"--config=C:\\x y\\z.toml", "\\\\server\\share\\dir", "\\\\server\\share dir\\", "say \"hi\" \\\"there\\\"",
"\xC5\x9F\xC3\xB6valye \xC4\x9F\xC3\xBC\xC5\x9F", "I\xC5\x9EIK.ogg"};

#ifdef _WIN32
std::wstring wide(std::string_view text)
{
if(text.empty())return {};
const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
REQUIRE(count>0);
std::wstring out(static_cast<std::size_t>(count),L'\0');
REQUIRE(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count)==count);
return out;
}
// The token query is_admin must agree with, made here directly.
bool token_is_elevated()
{
HANDLE token=nullptr;
REQUIRE(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token));
TOKEN_ELEVATION elevation{};
DWORD size=0;
const BOOL read=GetTokenInformation(token,TokenElevation,&elevation,sizeof elevation,&size);
CloseHandle(token);
REQUIRE(read);
if(elevation.TokenIsElevated)return true;
BYTE sid[SECURITY_MAX_SID_SIZE];
DWORD sid_size=sizeof sid;
REQUIRE(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,sid,&sid_size));
BOOL member=FALSE;
REQUIRE(CheckTokenMembership(nullptr,sid,&member));
return member!=FALSE;
}
// How many processes this one has started that are still listed.
int child_processes()
{
const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
REQUIRE(snapshot!=INVALID_HANDLE_VALUE);
PROCESSENTRY32W entry{};
entry.dwSize=sizeof entry;
int children=0;
const DWORD self=GetCurrentProcessId();
for(BOOL more=Process32FirstW(snapshot,&entry);more;more=Process32NextW(snapshot,&entry))if(entry.th32ParentProcessID==self)++children;
CloseHandle(snapshot);
return children;
}
#else
bool token_is_elevated(){return geteuid()==0;}
int child_processes(){return 0;}
#endif

// A fresh project folder under the system temp folder, removed on destruction.
class TempProject{
public:
TempProject(std::string_view script,std::string_view toml_rest)
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/("zodiac_elevation_test_"+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
fs::create_directories(dir_);
write("zodiac.toml","[game]\nmain = \"main.zs\"\n"+std::string(toml_rest));
write("main.zs",script);
}
~TempProject()
{
std::error_code ec;
fs::remove_all(dir_,ec);
}
TempProject(const TempProject&)=delete;
TempProject& operator=(const TempProject&)=delete;
const fs::path& dir() const{return dir_;}
private:
void write(const char* name,std::string_view text) const
{
std::ofstream out(dir_/name,std::ios::binary);
out.write(text.data(),static_cast<std::streamsize>(text.size()));
REQUIRE(out.good());
}
fs::path dir_;
};
class MemorySink final:public zodiac::DiagnosticSink{
public:
explicit MemorySink(std::vector<std::string>& lines):lines_(lines){}
void write(zodiac_log_level,std::string_view line) noexcept override{lines_.emplace_back(line);}
private:
std::vector<std::string>& lines_;
};
struct Ran{
int build=-1;
int run=-1;
// Every line, the runtime's shutdown included; lines_at_run_end of them came before run() returned.
std::vector<std::string> lines;
std::size_t lines_at_run_end=0;
};
// Builds and runs script as the main.zs of a fresh project whose zodiac.toml goes on with toml_rest, with the test modules.
Ran run_script(std::string_view script,std::string_view toml_rest="")
{
const TempProject project(script,toml_rest);
const auto loaded=zodiac::load_project(project.dir());
INFO(loaded.error_or(""));
REQUIRE(loaded.has_value());
Ran ran;
zodiac::Diagnostics diagnostics;
diagnostics.add_sink(std::make_unique<MemorySink>(ran.lines));
{
zodiac::ScriptRuntime runtime(diagnostics,zodiac::ModuleSearchPaths{{fs::path(ZODIAC_TEST_MODULE_DIR)}});
ran.build=runtime.build(*loaded);
if(ran.build==0)ran.run=runtime.run();
ran.lines_at_run_end=ran.lines.size();
}
return ran;
}
std::string joined_lines(const std::vector<std::string>& lines)
{
std::string text;
for(const std::string& line:lines)text.append(line).push_back('\n');
return text;
}
}

TEST_CASE("append_argument quotes exactly what the CommandLineToArgvW rules need")
{
CHECK(quoted_argument("plain")=="plain");
CHECK(quoted_argument("")=="\"\"");
CHECK(quoted_argument("two words")=="\"two words\"");
CHECK(quoted_argument("tab\there")=="\"tab\there\"");
CHECK(quoted_argument("quo\"te")=="\"quo\\\"te\"");
CHECK(quoted_argument("\"")=="\"\\\"\"");
// Backslashes are literal when no quote follows them, also in front of the end of an unquoted argument.
CHECK(quoted_argument("back\\slash")=="back\\slash");
CHECK(quoted_argument("trail\\")=="trail\\");
CHECK(quoted_argument("\\\\server\\share\\dir")=="\\\\server\\share\\dir");
// In a quoted argument, the run in front of the closing quote is doubled, and so is a run in front of an escaped quote.
CHECK(quoted_argument("trail space\\")=="\"trail space\\\\\"");
CHECK(quoted_argument("C:\\Program Files\\Zodiac\\")=="\"C:\\Program Files\\Zodiac\\\\\"");
CHECK(quoted_argument("a\\\"b")=="\"a\\\\\\\"b\"");
CHECK(quoted_argument("\\\\\"")=="\"\\\\\\\\\\\"\"");
CHECK(quoted_argument("\xC5\x9F\xC3\xB6valye")=="\xC5\x9F\xC3\xB6valye");
}

TEST_CASE("append_argument round trips every argument through the rules, alone and on one line")
{
for(const std::string& argument:kArguments){
CAPTURE(argument);
const std::vector<std::string> parsed=parse_arguments(quoted_argument(argument));
REQUIRE(parsed.size()==1);
CHECK(parsed[0]==argument);
}
const std::string line=joined(kArguments);
CHECK(parse_arguments(line)==kArguments);
// Each argument next to every other one, both ways round, so no argument's end runs into the next one's start.
for(const std::string& first:kArguments)for(const std::string& second:kArguments){
const std::vector<std::string> pair{first,second};
CHECK(parse_arguments(joined(pair))==pair);
}
}

#ifdef _WIN32
TEST_CASE("append_argument agrees with CommandLineToArgvW on UTF-16 lines")
{
std::wstring line=L"\"C:\\some dir\\game.exe\"";
std::vector<std::wstring> expected;
for(const std::string& argument:kArguments){
expected.push_back(wide(argument));
line.push_back(L' ');
zodiac::platform::append_argument(line,std::wstring_view(expected.back()));
}
int count=0;
LPWSTR* argv=CommandLineToArgvW(line.c_str(),&count);
REQUIRE(argv!=nullptr);
std::vector<std::wstring> parsed;
for(int i=1;i<count;++i)parsed.emplace_back(argv[i]);
const std::wstring program=count>0?std::wstring(argv[0]):std::wstring();
LocalFree(argv);
CHECK(program==L"C:\\some dir\\game.exe");
REQUIRE(parsed.size()==expected.size());
for(std::size_t i=0;i<parsed.size();++i){
CAPTURE(kArguments[i]);
CHECK(parsed[i]==expected[i]);
}
}
#endif

TEST_CASE("is_admin returns what a direct query of the process token says")
{
const bool elevated=token_is_elevated();
CHECK(zodiac::platform::is_elevated()==elevated);
const Ran ran=run_script("int main(){return is_admin()?1:0;}\n");
INFO(joined_lines(ran.lines));
REQUIRE(ran.build==0);
CHECK(ran.run==(elevated?1:0));
CHECK(ran.lines.empty());
}

TEST_CASE("restart_as_admin in an elevated process returns true and starts nothing")
{
if(!token_is_elevated()){
#ifdef _WIN32
// Not elevated (a developer's machine): the call would show a real UAC prompt, so only the elevated runners (CI) run this.
MESSAGE("skipped: the process is not elevated");
return;
#else
// No portable elevation: false, and the script runs on.
const Ran ran=run_script("int main(){return restart_as_admin()?7:3;}\n");
INFO(joined_lines(ran.lines));
CHECK(ran.run==3);
CHECK(ran.lines.empty());
return;
#endif
}
const int before=child_processes();
// The call returns to main(), which runs on: nothing was started, nothing ended the run.
const Ran ran=run_script("int main(){bool again=restart_as_admin();bool admin=is_admin();return again&&admin?7:3;}\n");
INFO(joined_lines(ran.lines));
REQUIRE(ran.build==0);
CHECK(ran.run==7);
CHECK(ran.lines.empty());
CHECK(child_processes()==before);
}

TEST_CASE("restart_as_admin from a global variable's initializer starts nothing")
{
// Initializers run while zodiac build compiles too, so there it only reports the elevation it already has.
const int before=child_processes();
const Ran ran=run_script("bool g_again=restart_as_admin();\nint main(){return g_again?1:0;}\n");
INFO(joined_lines(ran.lines));
REQUIRE(ran.build==0);
CHECK(ran.run==(token_is_elevated()?1:0));
CHECK(ran.lines.empty());
CHECK(child_processes()==before);
}

TEST_CASE("arguments with quotes doubled inside quotes round trip as the C runtime received them")
{
// What the UCRT makes of "a""b c" (a"b c: "" inside quotes is one literal quote, and the quoted part goes on) and of other lines
// CommandLineToArgvW reads otherwise: append_argument writes each with \" only, which both parsers read alike.
const std::vector<std::string> received{"a\"b c","\"","\"\"","x\"\"y","\" \"","end\"\\"};
for(const std::string& argument:received){
CAPTURE(argument);
CHECK(parse_arguments(quoted_argument(argument))==std::vector<std::string>{argument});
}
CHECK(parse_arguments(joined(received))==received);
#ifdef _WIN32
const std::wstring line=L"x.exe "+wide(joined(received));
int count=0;
LPWSTR* argv=CommandLineToArgvW(line.c_str(),&count);
REQUIRE(argv!=nullptr);
std::vector<std::wstring> parsed;
for(int i=1;i<count;++i)parsed.emplace_back(argv[i]);
LocalFree(argv);
std::vector<std::wstring> expected;
for(const std::string& argument:received)expected.push_back(wide(argument));
CHECK(parsed==expected);
#endif
}

TEST_CASE("the restart guard's name is the executable's, folded, and nothing else")
{
const std::wstring name=zodiac::platform::restart_guard_name(std::wstring_view(L"C:\\Games\\Kara \u015Fehir\\game.exe"));
const std::wstring prefix=L"Local\\zodiac-restart-";
REQUIRE(name.size()==prefix.size()+16);
CHECK(name.starts_with(prefix));
for(std::size_t i=prefix.size();i<name.size();++i){
CAPTURE(i);
CHECK(((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')));
}
// Deterministic, the same for another ASCII case, different for another executable or a non-ASCII letter's other case.
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L"C:\\Games\\Kara \u015Fehir\\game.exe"))==name);
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L"c:\\GAMES\\kara \u015FEHIR\\Game.EXE"))==name);
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L"C:\\Games\\Kara \u015Fehir\\game2.exe"))!=name);
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L"C:\\Games\\Kara \u015Eehir\\game.exe"))!=name);
// The value itself is pinned: FNV-1a 64 over the UTF-16 units, low byte first (here of "A": 0x41, 0x00).
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L"a"))==L"Local\\zodiac-restart-0908a207b5a15704");
CHECK(zodiac::platform::restart_guard_name(std::wstring_view(L""))==L"Local\\zodiac-restart-cbf29ce484222325");
// Narrow text gives the same name for the same units.
const std::string narrow=zodiac::platform::restart_guard_name(std::string_view("C:\\x.exe"));
CHECK(narrow==zodiac::platform::restart_guard_name(std::string_view("c:\\X.EXE")));
const std::wstring wide_name=zodiac::platform::restart_guard_name(std::wstring_view(L"C:\\x.exe"));
CHECK(std::wstring(narrow.begin(),narrow.end())==wide_name);
}

#if defined(ZODIAC_TEST_HOOKS)
namespace {
// Sets test_hook_elevation for one test and restores the platform afterwards.
class HookScope{
public:
explicit HookScope(zodiac::ElevationHook hook){zodiac::test_hook_elevation(hook);}
~HookScope(){zodiac::test_hook_elevation(zodiac::ElevationHook::off);}
HookScope(const HookScope&)=delete;
HookScope& operator=(const HookScope&)=delete;
};
}

TEST_CASE("restart_as_admin started through the elevation hook ends main normally, and the modules shut down after it")
{
const HookScope hook(zodiac::ElevationHook::started);
// The index past the end would raise a runtime error, and 5 would be the exit code, if anything after the call ran.
const Ran ran=run_script("ref_thing@ g_kept;\nint main(){@g_kept=ref_thing();bool admin=is_admin();if(admin)return 9;restart_as_admin();array<int> none;none[3]=1;return 5;}\n","[modules]\nstatic = [\"ref_object\"]\n");
INFO(joined_lines(ran.lines));
REQUIRE(ran.build==0);
CHECK(ran.run==0);
CHECK(ran.lines_at_run_end==0);
// The runtime shut down as after any main(): the script's object released, then the module's shutdown.
CHECK(ran.lines==std::vector<std::string>{"ref_object release","ref_object shutdown"});
CHECK(zodiac::test_hook_restart_requests()==1);
MESSAGE("elevation hook paths checked: started");
}

TEST_CASE("restart_as_admin declined or failed through the elevation hook returns false and main goes on")
{
for(const zodiac::ElevationHook kind:{zodiac::ElevationHook::declined,zodiac::ElevationHook::failed,zodiac::ElevationHook::not_elevated}){
CAPTURE(static_cast<int>(kind));
const HookScope hook(kind);
const Ran ran=run_script("int main(){if(is_admin())return 9;bool again=restart_as_admin();return again?1:7;}\n");
INFO(joined_lines(ran.lines));
REQUIRE(ran.build==0);
CHECK(ran.run==7);
CHECK(ran.lines.empty());
CHECK(zodiac::test_hook_restart_requests()==1);
}
MESSAGE("elevation hook paths checked: declined and failed");
}

TEST_CASE("restart_as_admin through the elevation hook starts nothing from a global initializer or a nested call")
{
const HookScope hook(zodiac::ElevationHook::started);
const Ran global=run_script("bool g_again=restart_as_admin();\nint main(){return g_again?1:7;}\n");
INFO(joined_lines(global.lines));
REQUIRE(global.build==0);
CHECK(global.run==7);
CHECK(global.lines.empty());
CHECK(zodiac::test_hook_restart_requests()==0);
// A sort's comparator runs nested in main()'s context: refused, so the sort, and main, finish.
const Ran nested=run_script("bool g_inside=true;\nbool by_value(const int &in a,const int &in b){g_inside=restart_as_admin();return a<b;}\nint main(){array<int> v={3,1,2};v.sort(by_value);if(v[0]!=1||v[2]!=3)return 4;return g_inside?1:7;}\n");
INFO(joined_lines(nested.lines));
REQUIRE(nested.build==0);
CHECK(nested.run==7);
CHECK(nested.lines.empty());
CHECK(zodiac::test_hook_restart_requests()==0);
MESSAGE("elevation hook paths checked: global initializer and nested call");
}

TEST_CASE("restart_as_admin through the elevation hook acts on the main context only, never on another thread's")
{
const HookScope hook(zodiac::ElevationHook::started);
asPrepareMultithread();
asIScriptEngine* engine=asCreateScriptEngine();
REQUIRE(engine!=nullptr);
asIScriptContext* main_context=nullptr;
zodiac::SystemLibrary library{&main_context};
zodiac::register_system(engine,library);
asIScriptModule* module=engine->GetModule("m",asGM_ALWAYS_CREATE);
REQUIRE(module->AddScriptSection("m","bool f(){return restart_as_admin();}")>=0);
REQUIRE(module->Build()>=0);
asIScriptFunction* f=module->GetFunctionByDecl("bool f()");
REQUIRE(f!=nullptr);
main_context=engine->CreateContext();
REQUIRE(main_context!=nullptr);
int other_state=-1;
bool other_result=true;
std::thread([&]{
asIScriptContext* context=engine->CreateContext();
if(context&&context->Prepare(f)>=0){
other_state=context->Execute();
if(other_state==asEXECUTION_FINISHED)other_result=context->GetReturnByte()!=0;
}
if(context)context->Release();
asThreadCleanup();
}).join();
CHECK(other_state==asEXECUTION_FINISHED);
CHECK_FALSE(other_result);
CHECK_FALSE(library.restarting);
CHECK(zodiac::test_hook_restart_requests()==0);
// On the main context the call starts the copy and stops the context.
REQUIRE(main_context->Prepare(f)>=0);
CHECK(main_context->Execute()==asEXECUTION_ABORTED);
CHECK(library.restarting);
CHECK(zodiac::test_hook_restart_requests()==1);
main_context->Release();
engine->ShutDownAndRelease();
asUnprepareMultithread();
MESSAGE("elevation hook paths checked: another thread");
}
#endif

#ifdef _WIN32
namespace {
fs::path probe(){return fs::path(ZODIAC_TEST_RESTART_GUARD_PROBE);}
// A guard name no other test or process uses.
std::wstring fresh_guard_name()
{
static std::atomic<unsigned> counter{0};
const std::wstring executable=L"C:\\zodiac-test\\"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(counter.fetch_add(1))+L".exe";
return zodiac::platform::restart_guard_name(std::wstring_view(executable));
}
// Starts command_line (raw, as given) with standard output on a pipe. Returns the process handle; out_read gets the pipe's read end
// when wanted.
HANDLE start_raw(std::wstring command_line,HANDLE* out_read=nullptr)
{
SECURITY_ATTRIBUTES inheritable{};
inheritable.nLength=sizeof inheritable;
inheritable.bInheritHandle=TRUE;
HANDLE read_end=nullptr;
HANDLE write_end=nullptr;
REQUIRE(CreatePipe(&read_end,&write_end,&inheritable,0));
REQUIRE(SetHandleInformation(read_end,HANDLE_FLAG_INHERIT,0));
STARTUPINFOW startup{};
startup.cb=sizeof startup;
startup.dwFlags=STARTF_USESTDHANDLES;
startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
startup.hStdOutput=write_end;
startup.hStdError=write_end;
PROCESS_INFORMATION information{};
const BOOL started=CreateProcessW(probe().c_str(),command_line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&information);
CloseHandle(write_end);
if(!started)CloseHandle(read_end);
REQUIRE(started);
CloseHandle(information.hThread);
if(out_read)*out_read=read_end;
else CloseHandle(read_end);
return information.hProcess;
}
// Runs command_line to its end and returns its standard output.
std::string run_raw(const std::wstring& command_line)
{
HANDLE read_end=nullptr;
const HANDLE process=start_raw(command_line,&read_end);
std::string output;
char buffer[4096];
for(DWORD got=0;ReadFile(read_end,buffer,sizeof buffer,&got,nullptr)&&got>0;)output.append(buffer,got);
CloseHandle(read_end);
WaitForSingleObject(process,INFINITE);
DWORD code=1;
GetExitCodeProcess(process,&code);
CloseHandle(process);
CHECK(code==0);
return output;
}
// Waits until the probe has made the mutex name, so the test's wait starts while it is held.
void wait_until_held(const std::wstring& name)
{
for(int i=0;i<1000;++i){
if(const HANDLE mutex=OpenMutexW(SYNCHRONIZE,FALSE,name.c_str())){
CloseHandle(mutex);
return;
}
Sleep(10);
}
FAIL("the probe never took the guard");
}
}

TEST_CASE("a process waiting for the restart guard carries on once its owner process ends, or after the timeout")
{
using zodiac::platform::GuardWait;
using Clock=std::chrono::steady_clock;
// No guard: an ordinary start does not wait.
const std::wstring none=fresh_guard_name();
const Clock::time_point quick=Clock::now();
CHECK(zodiac::platform::wait_restart_guard(none,zodiac::platform::kRestartWaitMs)==GuardWait::none);
CHECK(Clock::now()-quick<std::chrono::seconds(1));
zodiac::platform::wait_for_restart();
// The owner process ends without giving it back (as a restarted game does): abandoned, which counts as done.
const std::wstring held=fresh_guard_name();
const HANDLE owner=start_raw(L"\""+probe().wstring()+L"\" hold "+held+L" 1500");
wait_until_held(held);
const Clock::time_point start=Clock::now();
CHECK(zodiac::platform::wait_restart_guard(held,zodiac::platform::kRestartWaitMs)==GuardWait::abandoned);
const auto waited=Clock::now()-start;
CHECK(waited>=std::chrono::milliseconds(300));
CHECK(waited<std::chrono::milliseconds(zodiac::platform::kRestartWaitMs));
CHECK(WaitForSingleObject(owner,10000)==WAIT_OBJECT_0);
DWORD code=1;
GetExitCodeProcess(owner,&code);
CloseHandle(owner);
CHECK(code==0);
// Gone with its last handle.
CHECK(zodiac::platform::wait_restart_guard(held,0)==GuardWait::none);
// An owner that does not end in time: the waiter carries on anyway.
const std::wstring slow=fresh_guard_name();
const HANDLE hung=start_raw(L"\""+probe().wstring()+L"\" hold "+slow+L" 20000");
wait_until_held(slow);
CHECK(zodiac::platform::wait_restart_guard(slow,200)==GuardWait::timed_out);
TerminateProcess(hung,0);
WaitForSingleObject(hung,10000);
CloseHandle(hung);
}

TEST_CASE("a restart guard given back, as after a declined or failed restart, releases its waiter at once")
{
using zodiac::platform::GuardWait;
const std::wstring name=fresh_guard_name();
void* guard=zodiac::platform::hold_restart_guard(name);
REQUIRE(guard!=nullptr);
// A second holder of the same name owns nothing.
CHECK(zodiac::platform::hold_restart_guard(name)==nullptr);
GuardWait seen=GuardWait::none;
std::thread waiter([&]{seen=zodiac::platform::wait_restart_guard(name,zodiac::platform::kRestartWaitMs);});
Sleep(200);
zodiac::platform::release_restart_guard(guard);
waiter.join();
CHECK(seen==GuardWait::released);
CHECK(zodiac::platform::wait_restart_guard(name,0)==GuardWait::none);
}

TEST_CASE("the elevated copy's arguments are the C runtime's, also where CommandLineToArgvW reads the line otherwise")
{
// "a""b c" is one argument a"b c to the UCRT, which gave wmain its arguments; CommandLineToArgvW would split it.
const std::string output=run_raw(L"\""+probe().wstring()+L"\" params \"a\"\"b c\" plain \"x\\\"y\" \"\" \"\\\\server\\share dir\\\\\" \u015F\u00F6valye");
std::vector<std::string> arguments;
std::string line;
std::size_t at=0;
while(at<output.size()){
const std::size_t end=output.find('\n',at);
const std::string text=output.substr(at,end-at);
if(text.starts_with("arg:"))arguments.push_back(text.substr(4));
else if(text.starts_with("line:"))line=text.substr(5);
if(end==std::string::npos)break;
at=end+1;
}
INFO(output);
CHECK(arguments==std::vector<std::string>{"params","a\"b c","plain","x\"y","","\\\\server\\share dir\\","\xC5\x9F\xC3\xB6valye"});
// The rebuilt line gives back exactly those arguments, to the rules and to CommandLineToArgvW.
CHECK(parse_arguments(line)==arguments);
int count=0;
const std::wstring wide_line=L"x.exe "+wide(line);
LPWSTR* argv=CommandLineToArgvW(wide_line.c_str(),&count);
REQUIRE(argv!=nullptr);
std::vector<std::wstring> parsed;
for(int i=1;i<count;++i)parsed.emplace_back(argv[i]);
LocalFree(argv);
std::vector<std::wstring> expected;
for(const std::string& argument:arguments)expected.push_back(wide(argument));
CHECK(parsed==expected);
}
#endif
