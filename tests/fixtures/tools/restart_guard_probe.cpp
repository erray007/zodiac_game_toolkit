// Test fixture (Windows only, a wide wmain like zodiac.exe's) for test_elevation's restart guard and restart arguments:
// - restart_guard_probe hold <name> <ms>: takes the restart guard <name> with hold_restart_guard, as restart_elevated does before
//   its launch, keeps it for <ms> milliseconds and exits without giving it back, so the system releases it as abandoned, as when a
//   game that restarted itself ends. Exit code 3 when the guard cannot be taken.
// - restart_guard_probe params <arguments...>: writes, in UTF-8, one line "arg:<argument>" for every argument the C runtime gave
//   wmain (__wargv), then "line:<restart_parameters>", the command line restart_elevated would give the elevated copy.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/elevation.hpp>
#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>

namespace {
void write_utf8(std::wstring_view text)
{
std::string bytes;
if(!text.empty()){
const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
bytes.resize(static_cast<std::size_t>(count));
WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),bytes.data(),count,nullptr,nullptr);
}
bytes.push_back('\n');
DWORD written=0;
WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
}
}

int wmain(int argc,wchar_t** argv)
{
if(argc>=4&&std::wstring_view(argv[1])==L"hold"){
void* guard=zodiac::platform::hold_restart_guard(argv[2]);
if(!guard)return 3;
Sleep(static_cast<DWORD>(std::wcstoul(argv[3],nullptr,10)));
// Not released: the process's end abandons it.
return 0;
}
if(argc>=2&&std::wstring_view(argv[1])==L"params"){
for(int i=1;i<argc;++i)write_utf8(std::wstring(L"arg:")+argv[i]);
std::wstring line;
if(!zodiac::platform::restart_parameters(line))return 4;
write_utf8(L"line:"+line);
return 0;
}
return 2;
}
