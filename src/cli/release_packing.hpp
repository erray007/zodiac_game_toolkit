// zodiac::cli's release packing for zodiac build (spec 8.4 of the library-and-packs design): which assets go into the build pack,
// the pack itself and the release manifest patches are made against (Task 11).
//
// split_assets(project, assets): assets (absolute, under project.root, as expand_assets gives them, the output folder's already
// left out) in two lists, both keeping their order: plain (copied next to the executable as plan 3 does) for each asset
// packed_in_release refuses (core/project_file.hpp: [pack] enabled false, a [pack] exclude match, a *.zpk file), packed for the rest.
//
// new_build_id(): 32 lowercase hex digits of fresh random bytes (`error: cannot make a build id: the entropy source failed`). zodiac
// build makes it before the link, so the payload carries it (GamePayload::build_id) and the release refuses a pack of another build.
// write_release_pack(project, key, split, pack_file, build_id):
// - Creates the pack pack_file (any file of that name is removed first: it is the build's own temporary file) through
//   pack::PackStorage, sealed with key as a raw key (kdf 0), so every chunk and index follows the storage's nonce rule; no byte of
//   it is written anywhere else. Each packed asset becomes one entry named by its project-relative path ('/'), with its source
//   file's modification time (milliseconds, rounded down as the folder source rounds them), so file_modified gives the same in
//   dev and in the release. The bytes are streamed from the file in the storage's batches and hashed (BLAKE2b-256) as they pass;
//   no asset is ever held whole in memory. The metadata is `build=<build_id>`.
// - Writes the manifest to <root>/.zodiac/release.manifest.tmp: `zodiac-release-manifest 1`, `build <build_id>`,
//   `next_patch <n>` (n from the current .zodiac/release.manifest when there is one, else 1), then one line per packed asset,
//   sorted by path bytes: `<64 hex digits of its BLAKE2b-256> <size> <path>`, then one per plain asset of split (hashed, streamed,
//   as the packed ones; a path holding a line break is left out): `plain <64 hex digits> <size> <path>`, every line ending in
//   "\n". commit_release_manifest
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
// write_patch(project, key, output) is zodiac build --patch (spec 8.4): a patch pack against the last full release build, made
// without compiling or linking anything (patches carry asset files only; the caller says so).
// - `error: --patch needs [pack] enabled` when [pack] enabled is false; `error: no release manifest in .zodiac; build a full
//   release first` without .zodiac/release.manifest; the damaged text for a manifest whose lines are not as described below
//   (a line of a kind not listed, a path given twice by fold, a deleted entry line without its patched line, ...).
// - The manifest after a full build holds the release's packed assets (`<hash> <size> <path>`, as above). After a patch it also
//   holds, for every path a patch has ever given a deletion marker, `deleted marker <path>` or `deleted entry <path>` (the state
//   the newest patch gave it), and for every entry the newest patch holds `patched <64 hex digits of its BLAKE2b-256> <size>
//   <path>`. Those lines are kept for the life of the release (a full build writes a new manifest without them). Together they
//   are what the game sees now: a path's newest patch state (marker, or entry with its hash and size) when a patch holds it,
//   else the release's entry.
// - The current assets are expand_assets' (those inside output left out, compared as the file system does) split by split_assets;
//   names are checked as write_release_pack checks them (not a valid file name, two of one fold). A path the manifest knows (a
//   release entry or a patched path) that is still declared but no longer packed (moved to [pack] exclude, or packing it is off
//   otherwise) is `error: "<path>" moved out of the pack; that needs a full release build`: a patch cannot turn a packed file
//   plain (its entry or marker would hide the plain file).
// - Patches are cumulative against the release, never against the previous patch: patch n holds every packed asset whose path,
//   size or hash differs from the release's entry, or that the release lacks; a deletion marker (spelled as the manifest has the
//   path) for every path the manifest knows that is not declared at all any more; and again every path an earlier patch holds,
//   as the entry the project has now (bytes equal to the release's included) or as a marker, so no older patch's entry or marker
//   ever shows through it, whichever patches lie next to the game.
// - Something has changed when one path's desired state (an entry with its current path spelling, size and hash, a marker, or
//   nothing) differs from what the game sees now; only then is a patch written. Hashes are made only where size and spelling
//   agree, streamed through one 1 MiB buffer; nothing is held whole in memory, and an asset written into the patch is read again
//   as write_release_pack reads one (hashed on the way, for its patched line).
// - Plain assets: a patch carries packed assets only. Every plain asset now (packed_in_release refuses it) that has no plain line
//   of that path, size and hash (new, or changed since the release; hashed only where path and size agree) gets, in path order,
//   `warning: "<path>" is not packed; patches carry packed assets only, so ship it with the patch or build a full release`, in
//   PatchResult::warnings, whether a patch is written or not; never an error. A manifest written before plain lines were
//   recorded has none, so every plain asset is warned about until the next full build. The release's plain lines are kept.
// - With nothing changed nothing is written: PatchResult{n, 0, 0, false, warnings}. Otherwise <output>/patches/patch_<n>.zpk
//   (pack::patch_file_name, n the manifest's next_patch) is made through pack::PackStorage under key as a raw key (the nonce
//   rule holds for every chunk and index), entries by project-relative path with their files' modification times and markers in
//   path order, metadata `base_build=<the manifest's build id>` and `patch=<n>`; as <name>.tmp first, then renamed over any file
//   of that name. The manifest's .tmp is written before that rename and committed after it (next_patch n+1 and the lines
//   above), so a failure leaves the manifest as it was (the next patch then takes the same number again, harmlessly).
//   PatchResult: number n, changed the entries and deleted the markers the patch holds, written true.
// - Other errors: `error: cannot read "<asset>": <reason>`, `error: cannot write "<file>": <reason>` (the patch, its folder, or
//   ".zodiac/release.manifest"), `error: cannot read ".zodiac/release.manifest": <reason>`. Temporary files go on every failure.
// Notes for the user documentation (Task 14): a packed asset is found by its project-relative path, case-insensitively, exactly as
// in zodiac run; .zodiac/release.manifest records the last release build, and patches (zodiac build --patch) are made against it.
// A patch holds every change since the full release build, so shipping the newest patch alone is enough; scripts and modules
// never change in a patch, and an asset moved out of the pack, or newly excluded, needs a full release build. Patch n re-ships
// every path changed (or deleted) since the release, so patches grow until the next full build, which starts again from
// nothing. A plain asset that is new or changed is not in any patch: ship it by hand next to the patch, or build a full release.
// Threads: main thread.
#pragma once
#include <core/pack/crypto.hpp>
#include <core/project_file.hpp>
#include <cstddef>
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
std::expected<std::string,std::string> new_build_id();
std::expected<void,std::string> write_release_pack(const Project& project,const crypto::Key& key,const AssetSplit& split,const std::filesystem::path& pack_file,const std::string& build_id);
std::expected<void,std::string> append_pack(const std::filesystem::path& executable,const std::filesystem::path& pack_file,std::uint32_t payload_crc);
std::expected<void,std::string> commit_release_manifest(const Project& project);
void discard_release_manifest(const Project& project) noexcept;
struct PatchResult{
int number;
std::size_t changed;
std::size_t deleted;
bool written;
// The plain-asset warnings (see write_patch), for the caller to report.
std::vector<std::string> warnings;
};
std::expected<PatchResult,std::string> write_patch(const Project& project,const crypto::Key& key,const std::filesystem::path& output);
}
