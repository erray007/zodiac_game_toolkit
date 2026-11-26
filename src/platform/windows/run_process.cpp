// Windows implementation of zodiac::platform::run_process (see platform/process.hpp).
//
// The command line is built with the quoting rules CommandLineToArgvW / the MSVC runtime parse (platform/command_line.hpp), so every
// UTF-8 argument reaches the child verbatim. Standard output and standard error share one inherited pipe that is read until EOF.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/process.hpp>
#include <platform/command_line.hpp>
#include <core/text.hpp>
#include <windows.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
struct Handle {
HANDLE value=nullptr;
Handle()=default;
explicit Handle(HANDLE handle) noexcept : value(handle) {}
Handle(const Handle&)=delete;
Handle& operator=(const Handle&)=delete;
Handle(Handle&& other) noexcept : value(std::exchange(other.value,nullptr)) {}
Handle& operator=(Handle&& other) noexcept
{
if(this!=&other){
reset();
value=std::exchange(other.value,nullptr);
}
return *this;
}
~Handle() { reset(); }
void reset() noexcept
{
if(value!=nullptr&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);
value=nullptr;
}
};

bool to_wide(const std::string& text,std::wstring& result)
{
result.clear();
if(text.empty())return true;
const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
if(count<=0)return false;
result.resize(static_cast<std::size_t>(count));
return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count)==count;
}

// The system's UTF-8 description of a GetLastError code, through the one helper every message uses.
std::string last_error_text(DWORD code)
{
return zodiac::error_text(std::error_code(static_cast<int>(code),std::system_category()));
}

std::string normalize_line_breaks(std::string&& text)
{
std::string result;
result.reserve(text.size());
for(std::size_t i=0;i<text.size();++i){
if(text[i]=='\r'&&i+1<text.size()&&text[i+1]=='\n')continue;
result+=text[i];
}
return result;
}
}

namespace zodiac::platform {
std::expected<ProcessResult,std::string> run_process(std::span<const std::string> argv)
{
if(argv.empty())return std::unexpected("no program to run");
std::wstring program;
if(!to_wide(argv[0],program))return std::unexpected("the program path is not valid UTF-8");
std::wstring command_line;
for(std::size_t i=0;i<argv.size();++i){
std::wstring argument;
if(!to_wide(argv[i],argument))return std::unexpected("an argument is not valid UTF-8");
if(i>0)command_line+=L' ';
// argv[0] is always quoted so a path with spaces stays one token.
if(i==0){
command_line+=L'"';
command_line+=argument;
command_line+=L'"';
} else zodiac::platform::append_argument(command_line,std::wstring_view(argument));
}

SECURITY_ATTRIBUTES inheritable{};
inheritable.nLength=sizeof(inheritable);
inheritable.bInheritHandle=TRUE;
Handle read_end;
Handle write_end;
if(!CreatePipe(&read_end.value,&write_end.value,&inheritable,0))return std::unexpected("cannot create the output pipe: "+last_error_text(GetLastError()));
if(!SetHandleInformation(read_end.value,HANDLE_FLAG_INHERIT,0))return std::unexpected("cannot configure the output pipe: "+last_error_text(GetLastError()));
// The child reads from an empty device instead of inheriting the caller's console input.
Handle null_input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inheritable,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
if(null_input.value==INVALID_HANDLE_VALUE){
null_input.value=nullptr;
return std::unexpected("cannot open the null device: "+last_error_text(GetLastError()));
}

STARTUPINFOW startup{};
startup.cb=sizeof(startup);
startup.dwFlags=STARTF_USESTDHANDLES;
startup.hStdInput=null_input.value;
startup.hStdOutput=write_end.value;
startup.hStdError=write_end.value;
PROCESS_INFORMATION information{};
if(!CreateProcessW(program.c_str(),command_line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,nullptr,nullptr,&startup,&information))
return std::unexpected("cannot start "+argv[0]+": "+last_error_text(GetLastError()));
Handle process(information.hProcess);
Handle thread(information.hThread);
// Only the child keeps the write end now, so reading ends when the child (and anything it spawned) closes it.
write_end.reset();
null_input.reset();

std::string output;
char buffer[4096];
for(;;){
DWORD read=0;
if(!ReadFile(read_end.value,buffer,sizeof(buffer),&read,nullptr)||read==0)break;
output.append(buffer,read);
}
WaitForSingleObject(process.value,INFINITE);
DWORD code=0;
if(!GetExitCodeProcess(process.value,&code))return std::unexpected("cannot read the exit code of "+argv[0]+": "+last_error_text(GetLastError()));
return ProcessResult{static_cast<int>(code),normalize_line_breaks(std::move(output))};
}
}
