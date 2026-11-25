// The binary layout of a pack file (spec 8.2 of the library-and-packs design) as pure encoding and decoding: no file, no key.
// PackStorage (pack_storage.hpp) does the reading, writing and cryptography. Every integer is little-endian.
//
// - Header, kHeaderSize (64) bytes: the magic "ZDPACK\x1a\n", u32 version (1), u32 flags (bit 0 compression, reserved: no
//   flag is defined yet), u8 chunk_log2, u8 kdf (0 a raw 256-bit key, 1 Argon2id), u16 0, u32 kdf_kib, u32 kdf_passes, the
//   16-byte salt, then 20 zero bytes. decode_header gives HeaderError::newer for a version above 1, and HeaderError::damaged
//   for a wrong magic, version 0, a reserved byte that is not zero, chunk_log2 outside kMinChunkLog2..kMaxChunkLog2, an
//   unknown kdf, or kdf settings that do not fit it: a raw key has kdf_kib and kdf_passes 0, Argon2id has kdf_kib from
//   crypto::kMinKdfKib to crypto::kMaxKdfKib (64 MiB) and kdf_passes from 1 to crypto::kMaxKdfPasses (8), so a crafted
//   header can never make an opener allocate or work more than that. The flags come back as they are: the opener refuses
//   set ones only once the index authenticates (pack_storage.hpp), so a damaged flags byte reads as damage.
// - Chunks: an entry of size bytes is stored as ceil(size / 2^chunk_log2) chunks, each its ciphertext followed by its
//   16-byte tag; stored_size gives the bytes on disk. chunk_nonce(index, id) is u32 chunk index then u64 entry id. At most
//   2^32 chunks per entry (the nonce's chunk index), which stored_size enforces.
// - Index plaintext: u64 generation, u64 wasted_bytes, u32 meta_len, the meta bytes, u32 count, then count entries of
//   u16 name_len, the name, u8 flags (bit 0 a deletion marker; no other bit is defined), u64 id, u64 size, u64 offset,
//   i64 modified_utc_ms. write_index_head and write_entry encode it in place; decode_index parses it, checking only the
//   structure (every length inside the bytes, no undefined flag, nothing left over); validate_index checks the meaning.
// - Footer, kFooterSize (40) bytes: u64 index_offset, u64 index_length (ciphertext and tag), the 12-byte index nonce, u32
//   CRC-32 (payload.hpp's crc32) of the preceding 28 bytes, then the magic "ZDPKEND1". inspect_footer classifies 40 bytes
//   that end at a given offset (FooterState below).
#pragma once
#include <core/pack/crypto.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zodiac::pack {
inline constexpr std::size_t kHeaderSize=64;
inline constexpr std::size_t kFooterSize=40;
inline constexpr std::size_t kTagSize=16;
inline constexpr std::uint32_t kFormatVersion=1;
inline constexpr std::uint8_t kMinChunkLog2=12;
inline constexpr std::uint8_t kMaxChunkLog2=24;
inline constexpr std::uint8_t kDefaultChunkLog2=16;
inline constexpr std::string_view kHeaderMagic{"ZDPACK\x1a\n",8};
inline constexpr std::string_view kFooterMagic{"ZDPKEND1",8};
// The fixed fields of an index plaintext: generation, wasted bytes, meta_len and count.
inline constexpr std::size_t kIndexFixedSize=8+8+4+4;
// The smallest index record (no metadata, no entry) with its tag.
inline constexpr std::uint64_t kMinIndexLength=kIndexFixedSize+kTagSize;
// The fixed fields of one index entry: name_len, flags, id, size, offset and modified_utc_ms.
inline constexpr std::size_t kEntryFixedSize=2+1+8+8+8+8;
inline constexpr std::size_t kMaxNameLength=0xFFFF;
inline constexpr std::uint8_t kEntryDeleted=1;

enum class Kdf:std::uint8_t{raw_key=0,argon2id=1};
struct Header{
std::uint32_t flags=0;
std::uint8_t chunk_log2=kDefaultChunkLog2;
Kdf kdf=Kdf::argon2id;
std::uint32_t kdf_kib=0;
std::uint32_t kdf_passes=0;
std::array<std::uint8_t,16> salt{};
};
using HeaderBytes=std::array<std::byte,kHeaderSize>;
enum class HeaderError{damaged,newer};
HeaderBytes encode_header(const Header& header) noexcept;
std::expected<Header,HeaderError> decode_header(std::span<const std::byte,kHeaderSize> bytes) noexcept;

// The bytes an entry of size bytes takes on disk in chunks of 2^chunk_log2; nullopt past 2^32 chunks or 2^64 bytes.
std::optional<std::uint64_t> stored_size(std::uint64_t size,std::uint8_t chunk_log2) noexcept;
crypto::Nonce chunk_nonce(std::uint32_t chunk_index,std::uint64_t entry_id) noexcept;

struct IndexEntry{
std::string name;
bool deleted=false;
std::uint64_t id=0;
std::uint64_t size=0;
std::uint64_t offset=0;
std::int64_t modified_utc_ms=0;
};
struct Index{
std::uint64_t generation=0;
std::uint64_t wasted_bytes=0;
std::string metadata;
std::vector<IndexEntry> entries;
};
// The plaintext bytes of the fixed fields and metadata, and of one entry.
constexpr std::size_t index_head_size(std::string_view metadata) noexcept{return kIndexFixedSize+metadata.size();}
constexpr std::size_t entry_size(const IndexEntry& entry) noexcept{return kEntryFixedSize+entry.name.size();}
// Write at out and return the end. The caller has checked metadata (at most 2^32 - 1 bytes), count and every name (at
// most kMaxNameLength bytes) against the format.
std::byte* write_index_head(std::byte* out,std::uint64_t generation,std::uint64_t wasted_bytes,std::string_view metadata,std::uint32_t count) noexcept;
std::byte* write_entry(std::byte* out,const IndexEntry& entry) noexcept;
// The whole plaintext of index; std::length_error when a name, the metadata or the count does not fit the format.
std::vector<std::byte> encode_index(const Index& index);
// nullopt when the structure is broken. Allocates the entries (std::bad_alloc).
std::optional<Index> decode_index(std::span<const std::byte> plain);
// True when every entry makes sense for a pack whose data region is [kHeaderSize, data_end): each name is its own
// canonical_entry_name, no two names have one fold_name, a deletion marker has id, size and offset 0, and a file has a
// non-zero id no other entry has and offset + stored_size inside the data region. With folds, it gets each entry's
// fold_name, in the order of the entries.
bool validate_index(const Index& index,std::uint8_t chunk_log2,std::uint64_t data_end,std::vector<std::string>* folds=nullptr);
// The entry name a script's name stands for: parse_path's relative form of a file path of the root mount ("sounds\a.ogg" is
// "sounds/a.ogg"), at most kMaxNameLength bytes; nullopt for anything else (a folder form, "data:", a host path, "..").
std::optional<std::string> canonical_entry_name(std::string_view name);

struct Footer{
std::uint64_t index_offset=0;
std::uint64_t index_length=0;
crypto::Nonce nonce{};
};
using FooterBytes=std::array<std::byte,kFooterSize>;
FooterBytes encode_footer(const Footer& footer) noexcept;
// What kFooterSize bytes that end at footer_offset + kFooterSize look like:
// - complete: the magic, the CRC, and an index of at least kMinIndexLength bytes from kHeaderSize on that ends exactly at
//   footer_offset (the footer is then filled in);
// - marked: not complete, but the magic or the CRC is right: a footer that was there and has been changed;
// - none: neither: no footer ends here (a truncated file or an interrupted append).
enum class FooterState{complete,marked,none};
FooterState inspect_footer(std::span<const std::byte,kFooterSize> bytes,std::uint64_t footer_offset,Footer& footer) noexcept;
}
