// zodiac::heap (see heap.hpp): the private heap of Windows ZODIAC_TEST_HOOKS trees. Nothing in any other build (a ZODIAC_BENCH_MIMALLOC
// tree's calls are all inline in heap.hpp).
#include <core/types/heap.hpp>
#if defined(ZODIAC_TEST_HOOKS)&&defined(_WIN32)&&!defined(ZODIAC_BENCH_MIMALLOC)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace zodiac::heap {
namespace {
// The heap every call uses: the private one when ZODIAC_PRIVATE_HEAP=1, otherwise null (the C runtime's). Decided by a
// constructor of the first priority a program may use, before any string, array or dictionary exists.
HANDLE heap_handle=nullptr;
[[gnu::constructor(101)]] void choose_heap() noexcept
{
char value[4];
if(GetEnvironmentVariableA("ZODIAC_PRIVATE_HEAP",value,sizeof value)!=1||value[0]!='1')return;
HANDLE heap=HeapCreate(0,0,0);
if(!heap)return;
ULONG low_fragmentation=2;
HeapSetInformation(heap,HeapCompatibilityInformation,&low_fragmentation,sizeof low_fragmentation);
heap_handle=heap;
}
}

void* allocate(std::size_t size) noexcept
{
return heap_handle?HeapAlloc(heap_handle,0,size?size:1):std::malloc(size);
}

void* allocate_zeroed(std::size_t count,std::size_t size) noexcept
{
if(!heap_handle)return std::calloc(count,size);
if(size&&count>SIZE_MAX/size)return nullptr;
return HeapAlloc(heap_handle,HEAP_ZERO_MEMORY,count*size?count*size:1);
}

void* reallocate(void* block,std::size_t size) noexcept
{
if(!heap_handle)return std::realloc(block,size);
if(!block)return HeapAlloc(heap_handle,0,size?size:1);
return HeapReAlloc(heap_handle,0,block,size?size:1);
}

void release(void* block) noexcept
{
if(!heap_handle){
std::free(block);
return;
}
if(block)HeapFree(heap_handle,0,block);
}

// The private heap aligns to 16 bytes: an aligned block is cut from a larger one, with the start of that one stored just
// before the aligned address.
void* allocate_aligned(std::size_t size,std::size_t alignment) noexcept
{
if(!heap_handle)return ::operator new(size,std::align_val_t{alignment},std::nothrow);
void* raw=HeapAlloc(heap_handle,0,size+alignment+sizeof(void*));
if(!raw)return nullptr;
auto address=reinterpret_cast<std::uintptr_t>(raw)+sizeof(void*);
address=(address+alignment-1)&~static_cast<std::uintptr_t>(alignment-1);
std::memcpy(reinterpret_cast<void*>(address-sizeof(void*)),&raw,sizeof raw);
return reinterpret_cast<void*>(address);
}

void release_aligned(void* block,std::size_t alignment) noexcept
{
if(!heap_handle){
::operator delete(block,std::align_val_t{alignment});
return;
}
if(!block)return;
void* raw;
std::memcpy(&raw,static_cast<char*>(block)-sizeof(void*),sizeof raw);
HeapFree(heap_handle,0,raw);
}

void* new_block(std::size_t size)
{
if(!heap_handle)return ::operator new(size);
if(void* block=HeapAlloc(heap_handle,0,size?size:1))return block;
throw std::bad_alloc();
}

void delete_block(void* block) noexcept
{
if(!heap_handle){
::operator delete(block);
return;
}
if(block)HeapFree(heap_handle,0,block);
}

bool private_heap() noexcept
{
return heap_handle!=nullptr;
}
}
#endif
