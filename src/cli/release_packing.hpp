// zodiac::cli's release packing for zodiac build (spec 8.4 of the library-and-packs design): which assets go into the build pack,
// the pack itself and the release manifest patches are made against (Task 11).
//
// split_assets(project, assets): assets (absolute, under project.root, as expand_assets gives them, the output folder's already
// left out) in two lists, both keeping their order: plain (copied next to the executable as plan 3 does) for each asset
// packed_in_release refuses (core/project_file.hpp: [pack] enabled false, a [pack] exclude match, a *.zpk file), packed for the rest.
//
// write_release_pack(project, key, split, pack_file, build_id):
// - Creates the pack pack_file (any file of that name is removed first: it is the build's own temporary file) through
//   pack::PackStorage, sealed with key as a raw key (kdf 0), so every chunk and index follows the storage's nonce rule; no byte of
//   it is written anywhere else. Each packed asset becomes one entry named by its project-relative path ('/'), with its source
//   file's modification time (milliseconds, rounded down as the folder source rounds them), so file_modified gives the same in
//   dev and in the release. The bytes are streamed from the file in the storage's batches and hashed (BLAKE2b-256) as they pass;
//   no asset is ever held whole in memory. The metadata is `build=<build_id>`, where build_id (set here) is 32 lowercase hex
//   digits of fresh random bytes.
// - Writes the manifest to <root>/.zodiac/release.manifest.tmp: `zodiac-release-manifest 1`, `build <build_id>`,
//   `next_patch <n>` (n from the current .zodiac/release.manifest when there is one, else 1), then one line per packed asset,
//   sorted by path bytes: `<64 hex digits of its BLAKE2b-256> <size> <path>`, every line ending in "\n". commit_release_manifest
//   moves it over .zodiac/release.manifest once the release itself is in place, so a build that fails later never changes the
//   manifest patches compare against; discard_release_manifest removes it otherwise.
// - Errors, each one line: `error: cannot read "<asset>": <reason>` (an asset that cannot be opened or read, or changes size while it
//   is packed), `error: cannot write "<pack_file>": <reason>`, `error: "<path>" is not a valid file name` (an asset path the VFS
//   rules refuse, so no release could open it: a Windows device name, a name ending in '.' or ' '), `error: "<path>" matches both
//   "<a>" and "<b>"; rename one of them` (two assets one name by fold_name, which one pack entry cannot hold),
//   `error: .zodiac/release.manifest is damaged; restore it from your backup or delete it` (an earlier manifest whose first three
//   lines are not as above), `error: cannot read|write ".zodiac/release.manifest": <reason>`. pack_file may be left behind on a
//   failure; the caller removes its temporary files.
// append_pack(executable, pack_file, payload_crc): appends the bytes of pack_file to the file executable in 1 MiB pieces, then
// zero bytes so the trailer ends on a multiple of pack::kTrailerAlignment, then pack::trailer_bytes(the pack's offset, its size,
// payload_crc), and flushes the file. `error: cannot read "<pack_file>": <reason>` or `error: cannot write "<executable>": <reason>`.
// Notes for the user documentation (Task 14): a packed asset is found by its project-relative path, case-insensitively, exactly as
// in zodiac run; .zodiac/release.manifest records the last release build, and patches (zodiac build --patch) are made against it.
// Threads: main thread.
#pragma once
#include <core/pack/crypto.hpp>
#include <core/project_file.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace zodiac::cli {
struct AssetSplit{
std::vector<std::filesystem::path> packed;
std::vector<std::filesystem::path> plain;
};
AssetSplit split_assets(const Project& project,const std::vector<std::filesystem::path>& assets);
std::expected<void,std::string> write_release_pack(const Project& project,const crypto::Key& key,const AssetSplit& split,const std::filesystem::path& pack_file,std::string& build_id);
std::expected<void,std::string> append_pack(const std::filesystem::path& executable,const std::filesystem::path& pack_file,std::uint32_t payload_crc);
std::expected<void,std::string> commit_release_manifest(const Project& project);
void discard_release_manifest(const Project& project) noexcept;
}
