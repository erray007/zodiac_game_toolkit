// zodiac::platform::is_elevated, restart_elevated and the restart guard on Windows (contract in platform/elevation.hpp).
// Every function used here is in advapi32, shell32 and kernel32, which every game imports already (the captured release link line,
// SHGetFolderPathW, RtlGenRandom); user32 is only looked up when the process has loaded it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/elevation.hpp>
#include <platform/command_line.hpp>
#include <platform/process.hpp>
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zodiac::platform {
namespace {
bool query_elevated() noexcept
{
HANDLE token=nullptr;
if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){
TOKEN_ELEVATION elevation{};
DWORD size=0;
const BOOL read=GetTokenInformation(token,TokenElevation,&elevation,sizeof elevation,&size);
CloseHandle(token);
if(read&&elevation.TokenIsElevated)return true;
}
// With UAC off an administrator's token is the full one: the Administrators group is enabled in it (CheckTokenMembership looks at
// enabled groups only, so the filtered token of a UAC administrator, where the group is deny-only, does not count).
BYTE sid[SECURITY_MAX_SID_SIZE];
DWORD sid_size=sizeof sid;
if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,sid,&sid_size))return false;
BOOL member=FALSE;
return CheckTokenMembership(nullptr,sid,&member)&&member;
}

// The working directory when the process started, read by this static initializer before main can change it (enter_root does);
// empty when it cannot be read, and the copy then starts in the system's default folder.
std::wstring start_directory() noexcept
{
try{
std::wstring directory(MAX_PATH,L'\0');
for(;;){
const DWORD length=GetCurrentDirectoryW(static_cast<DWORD>(directory.size()),directory.data());
if(length==0)return {};
if(length<directory.size()){
directory.resize(length);
return directory;
}
// Too small: length is the size needed, its terminating null included.
directory.resize(length);
}
}
catch(...){
return {};
}
}
const std::wstring g_start_directory=start_directory();

// The window the UAC prompt belongs to: the foreground window when it is this process's own, else the console window (null without one).
HWND prompt_owner() noexcept
{
if(const HMODULE user32=GetModuleHandleW(L"user32.dll")){
using Foreground=HWND(WINAPI*)();
using WindowProcess=DWORD(WINAPI*)(HWND,LPDWORD);
const auto foreground=reinterpret_cast<Foreground>(reinterpret_cast<void*>(GetProcAddress(user32,"GetForegroundWindow")));
const auto window_process=reinterpret_cast<WindowProcess>(reinterpret_cast<void*>(GetProcAddress(user32,"GetWindowThreadProcessId")));
if(foreground&&window_process){
if(const HWND window=foreground()){
DWORD process=0;
window_process(window,&process);
if(process==GetCurrentProcessId())return window;
}
}
}
return GetConsoleWindow();
}

// Appends argv[1..count) to line, each quoted by append_argument, separated by spaces, with one reservation.
void append_arguments(std::wstring& line,const wchar_t* const* argv,int count)
{
std::size_t room=0;
for(int i=1;i<count;++i)room+=argument_room(std::wstring_view(argv[i]).size());
line.reserve(line.size()+room);
for(int i=1;i<count;++i){
if(i>1)line.push_back(L' ');
append_argument(line,std::wstring_view(argv[i]));
}
}

// The guard's name for this process's executable; empty when its path cannot be read.
std::wstring own_guard_name()
{
const std::filesystem::path executable=executable_path();
if(executable.empty())return {};
return restart_guard_name(std::wstring_view(executable.native()));
}
}

bool is_elevated() noexcept
{
static const bool elevated=query_elevated();
return elevated;
}

bool restart_parameters(std::wstring& line)
{
// The arguments the C runtime gave main or wmain (the UCRT parses the command line for it). __wargv is set when the program's entry
// point is wide (zodiac's wmain, a release's wWinMain); a narrow main leaves it null.
if(const wchar_t* const* argv=__wargv;argv&&__argc>0){
append_arguments(line,argv,__argc);
return true;
}
int count=0;
LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&count);
if(!argv)return false;
bool built=true;
try{
append_arguments(line,argv,count);
}
catch(...){
built=false;
}
LocalFree(argv);
return built;
}

void* hold_restart_guard(const std::wstring& name) noexcept
{
// SYNCHRONIZE (waiting) for everyone, everything for the owner: the elevated copy may run as another user (other credentials at the
// prompt), and a waiter needs nothing more.
PSECURITY_DESCRIPTOR descriptor=nullptr;
if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)(A;;0x00100000;;;WD)",SDDL_REVISION_1,&descriptor,nullptr))return nullptr;
SECURITY_ATTRIBUTES attributes{};
attributes.nLength=sizeof attributes;
attributes.lpSecurityDescriptor=descriptor;
attributes.bInheritHandle=FALSE;
HANDLE mutex=CreateMutexW(&attributes,TRUE,name.c_str());
const DWORD error=GetLastError();
LocalFree(descriptor);
if(!mutex)return nullptr;
// Another restart of the same executable holds it already: this process does not own it, so it is no guard of its own.
if(error==ERROR_ALREADY_EXISTS){
CloseHandle(mutex);
return nullptr;
}
return mutex;
}

void release_restart_guard(void* guard) noexcept
{
if(!guard)return;
ReleaseMutex(static_cast<HANDLE>(guard));
CloseHandle(static_cast<HANDLE>(guard));
}

GuardWait wait_restart_guard(const std::wstring& name,unsigned timeout_ms) noexcept
{
const HANDLE mutex=OpenMutexW(SYNCHRONIZE,FALSE,name.c_str());
if(!mutex)return GuardWait::none;
const DWORD waited=WaitForSingleObject(mutex,timeout_ms);
GuardWait result=GuardWait::timed_out;
if(waited==WAIT_OBJECT_0||waited==WAIT_ABANDONED){
// The wait made this process the owner: given back at once, so the name goes with the last handle.
ReleaseMutex(mutex);
result=waited==WAIT_OBJECT_0?GuardWait::released:GuardWait::abandoned;
}
CloseHandle(mutex);
return result;
}

void wait_for_restart() noexcept
{
try{
const std::wstring name=own_guard_name();
if(!name.empty())wait_restart_guard(name,kRestartWaitMs);
}
catch(...){
}
}

Elevation restart_elevated() noexcept
{
if(is_elevated())return Elevation::already;
void* guard=nullptr;
try{
const std::filesystem::path executable=executable_path();
if(executable.empty())return Elevation::failed;
std::wstring parameters;
if(!restart_parameters(parameters))return Elevation::failed;
// Taken before the launch, so the copy finds it whenever it starts; a guard that cannot be made leaves the copy unguarded only.
guard=hold_restart_guard(restart_guard_name(std::wstring_view(executable.native())));
SHELLEXECUTEINFOW info{};
info.cbSize=sizeof info;
// NOASYNC: the launch is complete when the call returns, since the caller ends soon after. FLAG_NO_UI: a failure is the script's to
// report, never a message box.
info.fMask=SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
info.hwnd=prompt_owner();
info.lpVerb=L"runas";
info.lpFile=executable.c_str();
info.lpParameters=parameters.empty()?nullptr:parameters.c_str();
info.lpDirectory=g_start_directory.empty()?nullptr:g_start_directory.c_str();
info.nShow=SW_SHOWNORMAL;
if(ShellExecuteExW(&info)){
// Kept to the process's exit, which releases it (abandoned) once everything was written.
return Elevation::started;
}
const DWORD error=GetLastError();
release_restart_guard(guard);
return error==ERROR_CANCELLED?Elevation::declined:Elevation::failed;
}
catch(...){
release_restart_guard(guard);
return Elevation::failed;
}
}
}
