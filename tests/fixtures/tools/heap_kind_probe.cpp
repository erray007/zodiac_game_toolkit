// Test tool (Windows): which heap a program's process heap is, the Segment Heap or the NT heap (Task 5c fix round 1: every shipped executable
// and every game asks for the Segment Heap in its application manifest, src/platform/windows/application.manifest).
//   heap_kind_probe [--require-segment] <program> [arguments...]
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
if(argc>first&&std::wcscmp(argv[first],L"--require-segment")==0){
require_segment=true;
++first;
}
if(argc<=first){
std::fprintf(stderr,"usage: heap_kind_probe [--require-segment] <program> [arguments...]\n");
return 2;
}
std::wstring line;
for(int i=first;i<argc;++i)append_quoted(line,argv[i]);
SetEnvironmentVariableW(L"_NO_DEBUG_HEAP",L"1");
STARTUPINFOW startup{};
startup.cb=sizeof startup;
PROCESS_INFORMATION process{};
if(!CreateProcessW(nullptr,line.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS,nullptr,nullptr,&startup,&process)){
std::fprintf(stderr,"heap_kind_probe: cannot start %ls (error %lu)\n",argv[first],GetLastError());
return 2;
}
HeapInfo info;
bool at_breakpoint=false;
for(bool running=true;running;){
DEBUG_EVENT event{};
if(!WaitForDebugEvent(&event,60000)){
std::fprintf(stderr,"heap_kind_probe: %ls sent no debug event within 60 s\n",argv[first]);
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
std::fprintf(stderr,"heap_kind_probe: could not read the process heap of %ls\n",argv[first]);
return 2;
}
const DWORD build=windows_build();
const char* kind=info.signature==0xDDEEDDEEu?"segment":info.signature==0xFFEEFFEEu?"nt":"unknown";
std::printf("%ls: heap %s %08x, Windows build %lu, NtGlobalFlag 0x%x\n",argv[first],kind,info.signature,build,info.global_flag);
if(!require_segment)return 0;
if(build<segment_heap_build){
std::printf("Windows build %lu is older than %lu (Windows 10 2004): heapType is ignored there, so the NT heap is expected (report only)\n",build,segment_heap_build);
return 0;
}
return info.signature==0xDDEEDDEEu?0:1;
}
