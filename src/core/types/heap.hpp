// zodiac::heap: the C heap calls of Zodiac's script types (String's blocks and indexes, ScriptArray's objects and slots, the
// dictionary's objects, segments and indexes), in one place.
// Contract:
// - Every build except a Windows ZODIAC_TEST_HOOKS tree: inline calls of the C runtime and the global operator new, exactly as the
//   call sites made before (malloc, realloc, calloc, free; operator new and delete, aligned or not; the throwing forms throw
//   std::bad_alloc).
// - A Windows ZODIAC_TEST_HOOKS tree (bench diagnosis only, never a release preset or the kit): when the environment variable
//   ZODIAC_PRIVATE_HEAP is "1" at startup, they use a private heap of their own (HeapCreate, with the low-fragmentation heap on,
//   as the process heap has), so a bench can tell whether Zodiac's allocations disturb the add-ons' through the shared C runtime
//   heap. The engine's allocations (asAllocMem, the strings held in arrays) stay on the C runtime heap. The choice is made once,
//   before the first allocation, and never changes, so no block crosses heaps.
#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>

namespace zodiac::heap {
#if defined(ZODIAC_TEST_HOOKS)&&defined(_WIN32)
void* allocate(std::size_t size) noexcept;
void* allocate_zeroed(std::size_t count,std::size_t size) noexcept;
void* reallocate(void* block,std::size_t size) noexcept;
void release(void* block) noexcept;
void* allocate_aligned(std::size_t size,std::size_t alignment) noexcept;
void release_aligned(void* block,std::size_t alignment) noexcept;
// True when the private heap is in use (for the bench's header line).
bool private_heap() noexcept;
#else
inline void* allocate(std::size_t size) noexcept
{
return std::malloc(size);
}
inline void* allocate_zeroed(std::size_t count,std::size_t size) noexcept
{
return std::calloc(count,size);
}
inline void* reallocate(void* block,std::size_t size) noexcept
{
return std::realloc(block,size);
}
inline void release(void* block) noexcept
{
std::free(block);
}
inline void* allocate_aligned(std::size_t size,std::size_t alignment) noexcept
{
return ::operator new(size,std::align_val_t{alignment},std::nothrow);
}
inline void release_aligned(void* block,std::size_t alignment) noexcept
{
::operator delete(block,std::align_val_t{alignment});
}
inline bool private_heap() noexcept
{
return false;
}
#endif
// The throwing forms (operator new's contract).
inline void* allocate_or_throw(std::size_t size)
{
if(void* block=allocate(size?size:1))return block;
throw std::bad_alloc();
}
inline void* allocate_aligned_or_throw(std::size_t size,std::size_t alignment)
{
if(void* block=allocate_aligned(size?size:1,alignment))return block;
throw std::bad_alloc();
}
}
