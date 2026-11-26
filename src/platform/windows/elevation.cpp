// zodiac::platform::is_elevated and restart_elevated on Windows (contract in platform/elevation.hpp).
// Every function used here is in advapi32, shell32 and kernel32, which every game imports already (the captured release link line,
// SHGetFolderPathW, RtlGenRandom); user32 is only looked up when the process has loaded it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/elevation.hpp>
#include <platform/command_line.hpp>
#include <platform/process.hpp>
#include <windows.h>
#include <shellapi.h>
#include <cstddef>
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

// Every argument after the program name, each quoted by append_argument, separated by spaces.
bool arguments(std::wstring& line)
{
int count=0;
LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&count);
if(!argv)return false;
std::size_t room=0;
for(int i=1;i<count;++i)room+=argument_room(std::wstring_view(argv[i]).size());
bool built=true;
try{
line.reserve(room);
for(int i=1;i<count;++i){
if(i>1)line.push_back(L' ');
append_argument(line,std::wstring_view(argv[i]));
}
}
catch(...){
built=false;
}
LocalFree(argv);
return built;
}
}

bool is_elevated() noexcept
{
static const bool elevated=query_elevated();
return elevated;
}

Elevation restart_elevated() noexcept
{
if(is_elevated())return Elevation::already;
try{
const std::filesystem::path executable=executable_path();
if(executable.empty())return Elevation::failed;
std::wstring parameters;
if(!arguments(parameters))return Elevation::failed;
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
if(ShellExecuteExW(&info))return Elevation::started;
return GetLastError()==ERROR_CANCELLED?Elevation::declined:Elevation::failed;
}
catch(...){
return Elevation::failed;
}
}
}
