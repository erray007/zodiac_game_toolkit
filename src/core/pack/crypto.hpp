// The cryptography of packs (spec 8.2 of the library-and-packs design): AEAD_CHACHA20_POLY1305 of RFC 8439, Argon2id for
// script pack passwords, keyed BLAKE2b-256 for per-pack file keys, BLAKE2b-256 hashes and the operating system's random bytes.
// BLAKE2b, Argon2id and the reference ChaCha20 are Monocypher 4.0.2's (cmake/Dependencies.cmake, THIRD_PARTY_LICENSES); Poly1305 is
// poly1305.cpp's, after poly1305-donna-64 (THIRD_PARTY_LICENSES).
// Contract:
// - seal encrypts plain into cipher and writes the 16-byte tag: the Poly1305 key is the first 32 bytes of the ChaCha20 block at
//   counter 0, the data is ChaCha20 from counter 1, and the tag covers aad, zero padding to 16 bytes, cipher, zero padding,
//   then the two lengths as little-endian 64-bit numbers (RFC 8439 2.8).
//   Preconditions of seal, which the caller guarantees (Task 7's pack writer relies on them): cipher.size() == plain.size(),
//   at most kMaxSealBytes, and cipher either exactly plain (in-place) or not overlapping it at all. Breaking one is a bug in
//   Zodiac's own code, and seal then aborts the process before it writes anything, so it never produces a record that
//   authenticates but holds wrong data.
// - open checks the tag over aad and cipher in constant time before it decrypts anything. On success it writes the plaintext
//   and returns true. On failure it returns false and nothing unauthenticated is ever readable:
//   - a wrong key, nonce, aad, cipher or tag: plain is zeroed;
//   - plain.size() != cipher.size(), or more than kMaxSealBytes: plain is zeroed when it shares no byte with cipher, and
//     nothing is written otherwise (exactly in place included), so the caller's const cipher is never overwritten;
//   - plain partially overlapping cipher (sharing bytes without being exactly cipher): nothing is written at all, since zeroing
//     plain would destroy part of cipher; plain keeps the caller's own bytes, none of them decrypted.
//   Exactly in-place (plain is cipher) and disjoint buffers are both allowed.
// - derive_password_key is Argon2id (version 0x13) with one lane, kib KiB of memory and passes passes, the password as the
//   Argon2 password and the salt as its salt, giving 32 bytes. kib must be from kMinKdfKib to kMaxKdfKib and passes from 1 to
//   kMaxKdfPasses, and the password at most 4 GiB - 1 bytes; otherwise it throws std::invalid_argument. It allocates kib KiB
//   (std::bad_alloc when that fails) and wipes it before freeing it.
// - derive_file_key is BLAKE2b-256 keyed with master over "zodiac pack key v1" (18 bytes, no terminator) then the salt.
// - derive_index_key is BLAKE2b-256 keyed with file_key over "zodiac pack index v1" (20 bytes, no terminator): the key a
//   pack's indexes are sealed under, apart from its chunks' file key, so the two nonce spaces never meet.
// - hash is unkeyed BLAKE2b-256.
// - random_bytes fills the span from platform::fill_entropy; std::runtime_error("the entropy source failed") when it fails.
// - Secrets are never logged. Every key, Poly1305 key, keystream buffer and work area this code creates in memory it names is
//   wiped before it goes out of scope. Two things are not:
//   - The caller's own Key copies: the keys it passes in and the ones derive_password_key, derive_file_key and
//     derive_index_key return are the caller's to wipe (crypto::wipe) when it is done with them.
//   - Registers and compiler spills: the vectorized rounds (batch4, batch8) hold key-derived state words in vector registers,
//     and the compiler may spill some of them to the stack; no portable code can wipe those slots (Monocypher's scalar code has
//     the same limit).
// - Everything is thread-safe: no function keeps state between calls except the ChaCha20 dispatch, chosen once.
// The detail namespace is the keystream behind seal and open, for test_crypto and bench_files:
// - A keystream function XORs size bytes of in (zeros when in is null) with the ChaCha20 keystream of key and nonce from block
//   counter onwards into out, the counter running on into the first nonce word as a 64-bit number past 0xFFFFFFFF, as
//   Monocypher's does (the AEAD never gets there). chacha20_reference is Monocypher's crypto_chacha20_ietf; with
//   ZODIAC_CRYPTO_SIMD (x86-64 only) chacha20_sse2 (4 blocks at a time) and chacha20_avx2 (8 blocks, CPUs with AVX2 only) are
//   the vectorized ones of chacha20_simd.cpp.
// - A Keystream is such a function and its lead: the bytes from counter 0 seal and open take in one call (the Poly1305 key
//   is the first 32, the rest XOR the first data bytes), its natural batch, so a short message costs one batch.
// - dispatched() is the one seal and open use, chosen once: AVX2 when the CPU has it, else SSE2, else the reference.
// - seal_with and open_with are seal and open on a given keystream (bench_files measures the reference against the
//   dispatched one with them).
// - poly1305 is the Poly1305 seal and open use, after poly1305-donna-64 (poly1305.hpp; Task 13 measured it against Monocypher's in
//   bench_files' poly1305_chunk); poly1305_monocypher is Monocypher's crypto_poly1305, kept as test_crypto's cross-check on random
//   inputs and bench_files' reference.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zodiac::crypto {
using Key=std::array<std::uint8_t,32>;
using Nonce=std::array<std::uint8_t,12>;
using Tag=std::array<std::uint8_t,16>;

inline constexpr std::uint32_t kDefaultKdfKib=8192;
inline constexpr std::uint32_t kDefaultKdfPasses=3;
inline constexpr std::uint32_t kMinKdfKib=8;
inline constexpr std::uint32_t kMaxKdfKib=65536;
inline constexpr std::uint32_t kMaxKdfPasses=8;
// RFC 8439's limit: the 32-bit block counter starts at 1 for the data.
inline constexpr std::uint64_t kMaxSealBytes=(std::uint64_t{1}<<38)-64;

void seal(const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> plain,std::span<std::byte> cipher,Tag& tag) noexcept;
[[nodiscard]] bool open(const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> cipher,const Tag& tag,std::span<std::byte> plain) noexcept;
[[nodiscard]] Key derive_password_key(std::string_view password,std::span<const std::uint8_t,16> salt,std::uint32_t kib,std::uint32_t passes);
[[nodiscard]] Key derive_file_key(const Key& master,std::span<const std::uint8_t,16> salt) noexcept;
[[nodiscard]] Key derive_index_key(const Key& file_key) noexcept;
[[nodiscard]] std::array<std::uint8_t,32> hash(std::span<const std::byte> data) noexcept;
// hash() in pieces: update with the data in any number of spans, then digest() gives hash() of their concatenation (once; the
// state is wiped then, and by the destructor). A file is hashed while it is streamed, never held whole.
class Hasher{
public:
Hasher() noexcept;
~Hasher();
Hasher(const Hasher&)=delete;
Hasher& operator=(const Hasher&)=delete;
void update(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::array<std::uint8_t,32> digest() noexcept;
private:
// Monocypher's crypto_blake2b_ctx (crypto.cpp checks the size and alignment), kept opaque so monocypher.h stays in crypto.cpp.
alignas(8) std::byte state_[224];
};
void random_bytes(std::span<std::byte> out);
// Zeroes size bytes at secret in a way the optimizer cannot remove (Monocypher's crypto_wipe).
void wipe(void* secret,std::size_t size) noexcept;

namespace detail {
using KeystreamFunction=void(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const std::uint8_t* nonce,std::uint32_t counter) noexcept;
KeystreamFunction chacha20_reference;
#if defined(ZODIAC_CRYPTO_SIMD)
KeystreamFunction chacha20_sse2;
KeystreamFunction chacha20_avx2;
// Whether this CPU runs chacha20_avx2.
bool cpu_has_avx2() noexcept;
#endif
struct Keystream {
KeystreamFunction* run;
std::size_t lead;
const char* name;
};
inline constexpr std::size_t kMaxLead=512;
inline constexpr Keystream reference{&chacha20_reference,64,"reference"};
#if defined(ZODIAC_CRYPTO_SIMD)
inline constexpr Keystream sse2{&chacha20_sse2,256,"sse2"};
inline constexpr Keystream avx2{&chacha20_avx2,512,"avx2"};
#endif
const Keystream& dispatched() noexcept;
void seal_with(const Keystream& stream,const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> plain,std::span<std::byte> cipher,Tag& tag) noexcept;
[[nodiscard]] bool open_with(const Keystream& stream,const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> cipher,const Tag& tag,std::span<std::byte> plain) noexcept;
void poly1305(std::uint8_t tag[16],const std::uint8_t* message,std::size_t size,const std::uint8_t key[32]) noexcept;
void poly1305_monocypher(std::uint8_t tag[16],const std::uint8_t* message,std::size_t size,const std::uint8_t key[32]) noexcept;
}
}
