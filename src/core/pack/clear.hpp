// Internal to src/core/pack (crypto.cpp, chacha20_simd.cpp): the one wipe of large or hot secret buffers.
// Contract: clear(secret, size) zeroes size bytes at secret, and the optimizer cannot remove it as a dead store: after the memset an empty
// asm statement takes the pointer as an operand and clobbers memory, so the compiler must assume the bytes are read. It runs at memset's
// speed, unlike Monocypher's crypto_wipe (a volatile byte loop), which crypto.cpp keeps for small secrets.
#pragma once
#include <cstddef>
#include <cstring>

namespace zodiac::crypto::detail {
inline void clear(void* secret,std::size_t size) noexcept
{
std::memset(secret,0,size);
asm volatile(""::"r"(secret):"memory");
}
}
