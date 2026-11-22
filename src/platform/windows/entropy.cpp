// fill_entropy on Windows (see platform/entropy.hpp): RtlGenRandom (advapi32's SystemFunction036), which every game links
// already (advapi32 is in the captured release link line), and which needs no provider handle. In ZODIAC_TEST_HOOKS trees
// only, the table hash seed hook reads its variable with GetEnvironmentVariableA, which needs no C runtime initialization.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/entropy.hpp>
#include <windows.h>
#include <ntsecapi.h>
#if defined(ZODIAC_TEST_HOOKS)
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#endif

namespace zodiac::platform {
bool fill_entropy(void* buffer,std::size_t size) noexcept
{
auto* bytes=static_cast<unsigned char*>(buffer);
while(size>0){
const ULONG chunk=static_cast<ULONG>(size>0x40000000u?0x40000000u:size);
if(!RtlGenRandom(bytes,chunk))return false;
bytes+=chunk;
size-=chunk;
}
return true;
}

#if defined(ZODIAC_TEST_HOOKS)
[[gnu::used,gnu::noinline]] bool test_hook_table_hash_seed(std::uint64_t& seed) noexcept
{
char text[32];
const DWORD size=GetEnvironmentVariableA("ZODIAC_TABLE_HASH_SEED",text,sizeof text);
if(size==0)return false;
const std::from_chars_result read=size<sizeof text?std::from_chars(text,text+size,seed):std::from_chars_result{text,std::errc::invalid_argument};
if(read.ec!=std::errc{}||read.ptr!=text+size){
std::fprintf(stderr,"ZODIAC_TABLE_HASH_SEED is not a whole number from 0 to 18446744073709551615\n");
std::abort();
}
return true;
}
#endif
}
