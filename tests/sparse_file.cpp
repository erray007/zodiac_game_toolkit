// zodiac_test::make_sparse (contract in sparse_file.hpp).
#include "sparse_file.hpp"
#include <doctest/doctest.h>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zodiac_test {
void make_sparse(const std::filesystem::path& file,std::uint64_t offset,std::string_view text)
{
std::filesystem::create_directories(file.parent_path());
#ifdef _WIN32
const HANDLE handle=CreateFileW(file.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
REQUIRE(handle!=INVALID_HANDLE_VALUE);
DWORD returned=0;
const bool sparse=DeviceIoControl(handle,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)!=0;
LARGE_INTEGER end;
end.QuadPart=static_cast<LONGLONG>(offset);
const bool sized=sparse&&SetFilePointerEx(handle,end,nullptr,FILE_BEGIN)&&SetEndOfFile(handle);
DWORD written=0;
const bool wrote=sized&&WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
CloseHandle(handle);
REQUIRE(sparse);
REQUIRE(sized);
REQUIRE(wrote);
DWORD high=0;
const DWORD low=GetCompressedFileSizeW(file.c_str(),&high);
REQUIRE((low!=INVALID_FILE_SIZE||GetLastError()==NO_ERROR));
const std::uint64_t allocated=(static_cast<std::uint64_t>(high)<<32)|low;
#else
const int descriptor=::open(file.c_str(),O_RDWR|O_CREAT|O_TRUNC,0600);
REQUIRE(descriptor>=0);
const bool sized=ftruncate(descriptor,static_cast<off_t>(offset))==0;
const bool wrote=sized&&pwrite(descriptor,text.data(),text.size(),static_cast<off_t>(offset))==static_cast<ssize_t>(text.size());
struct stat info{};
const bool stated=fstat(descriptor,&info)==0;
::close(descriptor);
REQUIRE(sized);
REQUIRE(wrote);
REQUIRE(stated);
const std::uint64_t allocated=static_cast<std::uint64_t>(info.st_blocks)*512;
#endif
// The hole takes no space: at most a few blocks hold the text.
REQUIRE(allocated<(std::uint64_t{1}<<20));
}
}
