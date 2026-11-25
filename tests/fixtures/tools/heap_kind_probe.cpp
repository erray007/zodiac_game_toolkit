// Test tool (Windows): which heap a program's process heap is, the Segment Heap or the NT heap (Task 5c fix round 1: every shipped executable
// and every game asks for the Segment Heap in its application manifest, src/platform/windows/application.manifest.in).
//   heap_kind_probe [--require-segment] [--require-same-elevation] <program> [arguments...]
// It also prints whether the child's token is elevated, beside the probe's own; --require-same-elevation fails (1) when the child is
// elevated and the probe is not (the manifest's asInvoker keeps installer detection off a game named setup or update). A program that
// Windows wants to elevate cannot be started at all here (error 740, ERROR_ELEVATION_REQUIRED, exit 2).
// Starts the program under the debugging API (DEBUG_ONLY_THIS_PROCESS) with _NO_DEBUG_HEAP=1, so being debugged does not switch its heap to
// the debug heap. At the loader's initial breakpoint, after the process heap exists and before any of the program's own code runs, it reads
// the child's PEB (ProcessHeap at offset 0x30, NtGlobalFlag at 0xBC on x64) and the heap header's signature at offset 0x10 (0xDDEEDDEE for a
// segment heap, 0xFFEEFFEE for an NT heap), ends the child and prints one line:
//   <program>: heap segment|nt|unknown <signature>, Windows build <n>, NtGlobalFlag 0x<flags>
// Exit codes: 0 read (with --require-segment: and the heap is the Segment Heap, or Windows is older than build 19041, Windows 10 2004, which
// ignores the manifest's heapType; then the line says so); 1 --require-segment on build 19041 or later and the heap is not the Segment Heap;
// 2 bad arguments, or the program could not be started or read.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {
constexpr DWORD segment_heap_build=19041;

// Appends argument to line as CommandLineToArgvW reads it back: quoted, backslashes doubled before a quote and before the closing quote.
void append_quoted(std::wstring& line,const wchar_t* argument)
{
if(!line.empty())line+=L' ';
line+=L'"';
std::size_t backslashes=0;
for(const wchar_t* p=argument;*p;++p){
if(*p==L'\\'){
++backslashes;
continue;
}
if(*p==L'"')line.append(backslashes*2+1,L'\\');
else line.append(backslashes,L'\\');
backslashes=0;
line+=*p;
}
line.append(backslashes*2,L'\\');
line+=L'"';
}

// The program's name as UTF-8 for the output line (printf's %ls stops at the first character the C locale cannot convert, such as the
// ş of a kit folder).
std::string utf8(const wchar_t* text)
{
const int count=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
if(count<=1)return {};
std::string result(static_cast<std::size_t>(count)-1,'\0');
WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),count,nullptr,nullptr);
return result;
}

DWORD windows_build()
{
using RtlGetVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
const auto get=reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")));
OSVERSIONINFOW info{};
info.dwOSVersionInfoSize=sizeof info;
return get&&get(&info)==0?info.dwBuildNumber:0;
}

struct HeapInfo {
bool read=false;
std::uint32_t signature=0;
std::uint32_t global_flag=0;
};

// A process's token elevation: elevated or not, and its TokenElevationType (1 default, 2 full, 3 limited).
struct Elevation {
bool known=false;
bool elevated=false;
int type=0;
};
Elevation elevation_of(HANDLE process)
{
Elevation result;
HANDLE token=nullptr;
if(!OpenProcessToken(process,TOKEN_QUERY,&token))return result;
TOKEN_ELEVATION elevation{};
TOKEN_ELEVATION_TYPE type{};
DWORD size=0;
result.known=GetTokenInformation(token,TokenElevation,&elevation,sizeof elevation,&size)&&GetTokenInformation(token,TokenElevationType,&type,sizeof type,&size);
result.elevated=elevation.TokenIsElevated!=0;
result.type=static_cast<int>(type);
CloseHandle(token);
return result;
}
const char* elevation_text(const Elevation& elevation)
{
if(!elevation.known)return "unknown";
return elevation.elevated?"elevated":"not elevated";
}

HeapInfo read_heap(HANDLE process)
{
HeapInfo info;
using QueryFn=LONG(NTAPI*)(HANDLE,PROCESSINFOCLASS,PVOID,ULONG,PULONG);
const auto query=reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess")));
PROCESS_BASIC_INFORMATION basic{};
if(!query||query(process,ProcessBasicInformation,&basic,sizeof basic,nullptr)!=0)return info;
const auto* peb=static_cast<const unsigned char*>(static_cast<const void*>(basic.PebBaseAddress));
std::uint64_t heap=0;
if(!ReadProcessMemory(process,peb+0x30,&heap,sizeof heap,nullptr)||!heap)return info;
if(!ReadProcessMemory(process,peb+0xBC,&info.global_flag,sizeof info.global_flag,nullptr))return info;
if(!ReadProcessMemory(process,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(heap)+0x10),&info.signature,sizeof info.signature,nullptr))return info;
info.read=true;
return info;
}
}

int wmain(int argc,wchar_t** argv)
{
int first=1;
bool require_segment=false;
bool require_same_elevation=false;
for(;argc>first;++first){
if(std::wcscmp(argv[first],L"--require-segment")==0)require_segment=true;
else if(std::wcscmp(argv[first],L"--require-same-elevation")==0)require_same_elevation=true;
else break;
}
if(argc<=first){
std::fprintf(stderr,"usage: heap_kind_probe [--require-segment] [--require-same-elevation] <program> [arguments...]\n");
return 2;
}
std::wstring line;
for(int i=first;i<argc;++i)append_quoted(line,argv[i]);
SetEnvironmentVariableW(L"_NO_DEBUG_HEAP",L"1");
STARTUPINFOW startup{};
startup.cb=sizeof startup;
PROCESS_INFORMATION process{};
if(!CreateProcessW(nullptr,line.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS,nullptr,nullptr,&startup,&process)){
const DWORD error=GetLastError();
// ERROR_ELEVATION_REQUIRED: Windows wants to elevate the program (its manifest, or installer detection without one).
std::fprintf(stderr,"heap_kind_probe: cannot start %s (error %lu%s)\n",utf8(argv[first]).c_str(),error,error==ERROR_ELEVATION_REQUIRED?", the program requires elevation":"");
return 2;
}
HeapInfo info;
Elevation child_elevation;
bool at_breakpoint=false;
for(bool running=true;running;){
DEBUG_EVENT event{};
if(!WaitForDebugEvent(&event,60000)){
std::fprintf(stderr,"heap_kind_probe: %s sent no debug event within 60 s\n",utf8(argv[first]).c_str());
TerminateProcess(process.hProcess,3);
return 2;
}
DWORD status=DBG_CONTINUE;
switch(event.dwDebugEventCode){
case CREATE_PROCESS_DEBUG_EVENT:
if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
break;
case LOAD_DLL_DEBUG_EVENT:
if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
break;
case EXCEPTION_DEBUG_EVENT:
// The loader's initial breakpoint: the process heap exists, none of the program's code has run. Read, then end the child there.
if(!at_breakpoint&&event.u.Exception.ExceptionRecord.ExceptionCode==EXCEPTION_BREAKPOINT){
at_breakpoint=true;
info=read_heap(process.hProcess);
child_elevation=elevation_of(process.hProcess);
TerminateProcess(process.hProcess,0);
}
else status=DBG_EXCEPTION_NOT_HANDLED;
break;
case EXIT_PROCESS_DEBUG_EVENT:
running=false;
break;
default:
break;
}
ContinueDebugEvent(event.dwProcessId,event.dwThreadId,status);
}
CloseHandle(process.hThread);
CloseHandle(process.hProcess);
if(!info.read){
std::fprintf(stderr,"heap_kind_probe: could not read the process heap of %s\n",utf8(argv[first]).c_str());
return 2;
}
const DWORD build=windows_build();
const char* kind=info.signature==0xDDEEDDEEu?"segment":info.signature==0xFFEEFFEEu?"nt":"unknown";
const Elevation own=elevation_of(GetCurrentProcess());
std::printf("%s: heap %s %08x, Windows build %lu, NtGlobalFlag 0x%x, %s (token type %d; the probe %s)\n",utf8(argv[first]).c_str(),kind,info.signature,build,info.global_flag,elevation_text(child_elevation),child_elevation.type,elevation_text(own));
std::fflush(stdout);
// The program runs as the user who starts it: not elevated beyond the probe (installer detection, or a manifest asking for more).
if(require_same_elevation&&(!child_elevation.known||(child_elevation.elevated&&!own.elevated)))return 1;
if(!require_segment)return 0;
if(build<segment_heap_build){
std::printf("Windows build %lu is older than %lu (Windows 10 2004): heapType is ignored there, so the NT heap is expected (report only)\n",build,segment_heap_build);
return 0;
}
return info.signature==0xDDEEDDEEu?0:1;
}
