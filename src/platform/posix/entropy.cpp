// fill_entropy on POSIX systems (see platform/entropy.hpp): getrandom from the kernel's pool (retried on EINTR and short
// reads), falling back to /dev/urandom where the call is missing (ENOSYS) or not permitted. In ZODIAC_TEST_HOOKS trees only,
// the table hash seed hook.
#include <platform/entropy.hpp>
#include <cerrno>
#if defined(ZODIAC_TEST_HOOKS)
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#endif
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

namespace zodiac::platform {
namespace {
bool read_urandom(unsigned char* bytes,std::size_t size) noexcept
{
const int file=::open("/dev/urandom",O_RDONLY|O_CLOEXEC);
if(file<0)return false;
while(size>0){
const ssize_t got=::read(file,bytes,size);
if(got<0&&errno==EINTR)continue;
if(got<=0){
::close(file);
return false;
}
bytes+=got;
size-=static_cast<std::size_t>(got);
}
::close(file);
return true;
}
}

bool fill_entropy(void* buffer,std::size_t size) noexcept
{
auto* bytes=static_cast<unsigned char*>(buffer);
while(size>0){
const ssize_t got=::getrandom(bytes,size,0);
if(got<0&&errno==EINTR)continue;
if(got<0)return (errno==ENOSYS||errno==EPERM)&&read_urandom(bytes,size);
bytes+=got;
size-=static_cast<std::size_t>(got);
}
return true;
}

#if defined(ZODIAC_TEST_HOOKS)
// The C library sets up environ before any constructor runs, so getenv is safe here.
[[gnu::used,gnu::noinline]] bool test_hook_table_hash_seed(std::uint64_t& seed) noexcept
{
const char* text=std::getenv("ZODIAC_TABLE_HASH_SEED");
if(!text||!*text)return false;
const std::size_t size=std::strlen(text);
const std::from_chars_result read=std::from_chars(text,text+size,seed);
if(read.ec!=std::errc{}||read.ptr!=text+size){
std::fprintf(stderr,"ZODIAC_TABLE_HASH_SEED is not a whole number from 0 to 18446744073709551615\n");
std::abort();
}
return true;
}
#endif
}
