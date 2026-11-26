// The command-line quoting helper (platform/command_line.hpp) against the CommandLineToArgvW rules, on every platform and, on Windows,
// against CommandLineToArgvW itself; and the script functions is_admin and restart_as_admin (core/library/library.hpp) through a
// ScriptRuntime. restart_as_admin is only called where it cannot start anything: elevated (CI runners are), from a global variable's
// initializer, or on POSIX, which has no elevation. A real UAC prompt is never shown, and no elevated game is ever started.
#include <core/diagnostics.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <doctest/doctest.h>
#include <platform/command_line.hpp>
#include <platform/elevation.hpp>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
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
explicit TempProject(std::string_view script)
{
static std::atomic<unsigned> counter{0};
dir_=fs::temp_directory_path()/("zodiac_elevation_test_"+std::to_string(counter.fetch_add(1))+"_"+std::to_string(reinterpret_cast<std::uintptr_t>(this)));
fs::create_directories(dir_);
write("zodiac.toml","[game]\nmain = \"main.zs\"\n");
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
std::vector<std::string> lines;
};
Ran run_script(std::string_view script)
{
const TempProject project(script);
const auto loaded=zodiac::load_project(project.dir());
INFO(loaded.error_or(""));
REQUIRE(loaded.has_value());
Ran ran;
zodiac::Diagnostics diagnostics;
diagnostics.add_sink(std::make_unique<MemorySink>(ran.lines));
zodiac::ScriptRuntime runtime(diagnostics,zodiac::ModuleSearchPaths{});
ran.build=runtime.build(*loaded);
if(ran.build==0)ran.run=runtime.run();
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
