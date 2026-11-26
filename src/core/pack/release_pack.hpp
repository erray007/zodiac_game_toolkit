// zodiac::pack's release side (spec 8.4 of the library-and-packs design): where a release finds its build pack, and the start-up
// mount that puts it at "/" before any script code runs. zodiac build (src/cli/release_packing.cpp) writes what this reads.
//
// Embedded layout (pack_mode embedded): the linked executable, then the pack (pack_format.hpp; its offsets are relative to its own
// first byte), then zero bytes so the trailer ends on a multiple of 8, then the 32-byte trailer, then whatever a signing tool
// appends after it (a PE attribute certificate table, which Authenticode does not hash; everything before it, the pack and the
// trailer included, is hashed, so the signature covers the pack).
// - Trailer (little-endian): u64 pack_offset (from the file's start) | u64 pack_size | u32 payload_crc | u32 crc32 (core/payload.hpp)
//   of the first 20 bytes | "ZDTRAIL1". The payload itself stays where the link put it, found through its linked symbols; the
//   trailer only locates the pack and names the payload it belongs to: payload_crc is payload_checksum of that payload's bytes,
//   so a pack appended to another build's executable is refused. trailer_bytes encodes one.
// - image_end(file): for a PE image (MZ, a PE signature, a PE32 or PE32+ optional header with at least 5 data directories) whose
//   security data directory (index 4: a file offset and a size) is not empty and ends exactly at the end of the file, that
//   directory's offset; otherwise the file's size. Nothing outside the file is read; any malformed header gives the file's size.
// - find_embedded_pack(file, payload_crc): the trailer that ends at image_end(file), or after up to 7 zero bytes before it (a
//   signer that aligns its table itself), whose magic and CRC hold, whose payload_crc is payload_crc and whose pack lies wholly
//   before the trailer. The result is the window PackStorage::open takes. A file too short for a trailer is refused without a
//   read outside it. The error names what is wrong (never shown to players: mount_release reports the damaged text).
// - release_pack_key(payload): the pack key. It is the one function through which a release reads its key, so the protection
//   plan can replace the payload's plain copy with a hidden form without touching anything else. Until that plan, the key sits
//   in the payload of every packed executable in clear: the pack's encryption only keeps casual readers out (obfuscation),
//   since anyone who has the shipped executable can extract the key and read or change every pack and patch of the game.
// - mount_release(vfs, payload, payload_crc, executable, diagnostics): mount_game_folders(vfs, the executable's folder, the game
//   name, payload.host_paths, release mode), then, unless pack_mode is none, opens the build pack (embedded: through the trailer,
//   read only; external: "<game>.zpk" in the executable's folder) and mounts it at "/" in front of the folder, writable when
//   pack_writable (an external pack the system will not let the game write is mounted read only instead, so writes to its names
//   fail with the read-only pack text rather than at commit). A read-only external pack is opened through a window over the
//   whole file, so it takes no lock and any number of copies of the game may run at once; a writable one takes the pack's lock
//   for the game's life. Then mount_patches(vfs, the key, payload.build_id, the executable's folder, the data folder
//   mount_game_folders mounted at data:/ (none when it is not mounted), diagnostics). The storage's closing flush reports a
//   failure to diagnostics. Returns 0, or 2 after one of:
//   - `error: the game data is damaged (pack: wrong key or damaged pack)`: no valid trailer, a trailer of another build, or a pack
//     the key does not open (or that fails its checks); `(pack: unsupported pack (newer format))` for a pack of a newer format;
//   - `error: the game data is damaged (pack "<game>.zpk" is missing)`;
//   - `error: the game data is damaged (pack "<game>.zpk" is from another build)`: the external pack's metadata has no line
//     `build=<payload.build_id>` (an embedded pack of another build has a trailer of another payload and is refused as damaged);
//   - `error: the game data is damaged (invalid build id)`: a pack mode without a build id;
//   - `error: "<game>.zpk" is in use by another program`: a writable pack another running copy of the game holds;
//   - `error: cannot read "<file>": <reason>` (the executable's or the pack's file name) for any other system failure;
//   - `error: the game data is damaged (invalid pack key)` or `(invalid pack or file setting)` for a payload whose pack fields
//     contradict each other (a key that is not 32 bytes with a pack, writable without external);
//   - `error: cannot mount the game data: out of memory`.
// - mount_patches(vfs, key, build_id, exe_dir, data_dir, diagnostics): the patch packs zodiac build --patch writes
//   (cli/release_packing.hpp), each cumulative against the full release build it was made for. It collects every file named
//   patch_<n>.zpk (n decimal, at least 1, no leading zeros, at most 2^64-1) in <exe_dir>/patches/ and <data_dir>/patches/
//   (an empty data_dir, or a missing folder, has none), orders them by n ascending, the data folder's after the executable
//   folder's at an equal n, and mounts each read only at "/" in that order, in front of the build pack (so the highest n
//   decides a name, and of two of one n the data folder's). Patches are matched to the release by their `base_build=` metadata
//   against build_id (payload.build_id, which the build pack's own `build=` line equals), never by `build=`; every check below
//   leaves the patch unmounted and the game going on:
//   - any other name in either patches/ folder (folders included), and a patch whose `patch=` metadata is not its n:
//     `warning: ignoring "<path>": patch packs are named patch_<n>.zpk`, in name order per folder;
//   - a patch the key does not open, or a damaged one: `error: cannot use patch "<path>": wrong key or damaged pack`
//     (`unsupported pack (newer format)` for a newer format; the system's reason when the file cannot be opened);
//   - a patch whose `base_build=` is not build_id (no such line included): `warning: patch "<path>" belongs to another release of
//     the game; it is not used`, so an old patch never overrides a newer full release;
//   - a folder that cannot be listed: `error: cannot use patch "<folder>": <reason>`; memory running out while mounting one:
//     `error: cannot use patch "<path>": out of memory`.
//   <path> is the patch's host path, one_line. A gap in the numbers is fine (each patch holds every change since the release);
//   two patches of one n are both mounted, the data folder's in front. Patches open through a window over the whole file: read
//   only, without a lock, so copies of the game run side by side. A patch's marker hides the older mounts' files of its name,
//   never a folder or a mount point (vfs.hpp), and a pack folder every file of which a patch deletes is gone.
// Notes for the user documentation (Task 14): until the protection plan, a release pack is obfuscation, not protection: its key is
// in clear in the executable it ships with. A writable release pack ([pack] writable) is mounted at "/", so every write to a
// name outside data:/ goes into the pack; a game started twice with a writable pack refuses the second start ("in use"). Patches
// go in a patches/ folder next to the executable, or in data:/patches/ (where a game's own updater may save them); a patch
// holds every change since its full release, so only the newest needs shipping, and a new full release ignores the old ones.
// With a writable release pack, a name a patch holds cannot be written any more (read-only pack text), since the patch is in
// front of the pack.
// Threads: the main thread, before the script runs.
#pragma once
#include <core/diagnostics.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/payload.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/file.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zodiac::pack {
inline constexpr std::size_t kTrailerSize=32;
inline constexpr char kTrailerMagic[]="ZDTRAIL1";
// The trailer of an embedded pack ends on a multiple of this.
inline constexpr std::uint64_t kTrailerAlignment=8;
// The payload's own stored CRC-32 (its last 4 bytes, little-endian), which parse_payload has checked; 0 for fewer than 4 bytes.
std::uint32_t payload_checksum(std::string_view payload_bytes) noexcept;
std::string trailer_bytes(std::uint64_t offset,std::uint64_t size,std::uint32_t payload_crc);
std::uint64_t image_end(const platform::File& file);
std::expected<PackLocation,std::string> find_embedded_pack(const platform::File& file,std::uint32_t payload_crc);
crypto::Key release_pack_key(const GamePayload& payload) noexcept;
int mount_release(vfs::Vfs& vfs,const GamePayload& payload,std::uint32_t payload_crc,const std::filesystem::path& executable,Diagnostics& diagnostics);
void mount_patches(vfs::Vfs& vfs,const crypto::Key& key,std::string_view build_id,const std::filesystem::path& exe_dir,const std::filesystem::path& data_dir,Diagnostics& diagnostics);
// "patch_<n>.zpk" for n, the name of a patch pack (zodiac build --patch writes it, mount_patches reads it); the number such a
// name gives, nullopt for any other name.
std::string patch_file_name(std::uint64_t n);
std::optional<std::uint64_t> patch_number(std::string_view file_name) noexcept;
}
