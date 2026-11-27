// Internal to src/core/pack (crypto.cpp, whose AEAD tag it computes; test_crypto checks it): Poly1305 (RFC 8439 2.5) after poly1305-donna-64 by Andrew Moon
// (github.com/floodyberry/poly1305-donna, commit e6ad6e0; MIT or public domain, THIRD_PARTY_LICENSES).
// Contract:
// - Poly1305 holds one computation: the constructor takes the 32-byte one-time key (r is clamped as RFC 8439 2.5.1 says, s kept
//   for the end), blocks takes whole 16-byte blocks (bytes a multiple of 16), each with the 2^128 bit set, and finish writes the
//   16-byte tag and wipes the state (clear.hpp). The AEAD's input (aad, zero padding, cipher, zero padding, the two lengths) is
//   whole blocks only, so crypto.cpp's tag never needs anything else.
// - last_partial is the end of a message whose length is not a multiple of 16: the last bytes, then 0x01 and zeros, and no 2^128
//   bit, as RFC 8439 2.5.1 says (donna's poly1305_finish). crypto::detail::poly1305 (crypto.hpp) is the whole function over
//   any message on it.
// - The limbs are donna-64's: h and r in three limbs of 44, 44 and 42 bits, 64 x 64 -> 128-bit products (unsigned __int128,
//   which clang has on every 64-bit target Zodiac builds for). The block function, the final carry, the h - p selection (constant
//   time) and the addition of s are donna's arithmetic unchanged; the class around them is Zodiac's.
// - Why (Task 13's Poly1305 ruling, bench-files-study 46020098583): Monocypher's Poly1305 was 39-61 % of a pack's sequential
//   64 KiB read (median 52 % on Windows, 54 % on Linux), over the 10 % bar; this one takes a median 0.904 of its time on Windows and
//   0.859 on Linux (0.82-0.93 on most runners, 1.035-1.037 on the EPYC 9V45 under Windows).
// - Secrets: finish wipes r, s and h. Registers and compiler spills inside blocks cannot be wiped portably (crypto.hpp's note).
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(__SIZEOF_INT128__)
#error "Poly1305 (poly1305.hpp) needs unsigned __int128"
#endif

namespace zodiac::crypto::detail {
class Poly1305{
public:
explicit Poly1305(const std::uint8_t key[32]) noexcept;
~Poly1305();
Poly1305(const Poly1305&)=delete;
Poly1305& operator=(const Poly1305&)=delete;
// Whole blocks of message bytes, each with its 2^128 bit; bytes must be a multiple of 16.
void blocks(const std::uint8_t* message,std::size_t bytes) noexcept{run(message,bytes,std::uint64_t{1}<<40);}
// The last block of a message whose length is not a multiple of 16: its bytes (1 to 15), then 0x01 and zeros, no 2^128 bit.
void last_partial(const std::uint8_t* message,std::size_t bytes) noexcept;
void finish(std::uint8_t tag[16]) noexcept;
private:
void run(const std::uint8_t* message,std::size_t bytes,std::uint64_t high_bit) noexcept;
std::uint64_t r_[3];
std::uint64_t h_[3];
std::uint64_t pad_[2];
};
}
