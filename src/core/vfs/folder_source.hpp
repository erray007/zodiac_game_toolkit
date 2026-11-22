// zodiac::vfs::FolderSource: a host folder as a VFS source, with case-insensitive lookup on every platform.
//
// Lookup: the exact name first (one stat or open of the whole path). On a miss the path is walked segment by segment through a
// per-folder cache of fold_name -> real names: the exact name wins, one case-insensitive match is taken, several are ambiguous
// (FileError::ambiguous; ambiguous_names tells which). On Windows, from the first segment with a '~' or a non-ASCII byte on, a path
// is walked through the cache instead of tried exactly (the plain ASCII folders before it still are), so an 8.3 short name
// (LONGFI~1.OGG) or a letter NTFS's own upcase table folds differently from Unicode's is not found there either, exactly as on Linux.
//
// The cache: one entry per folder, keyed by the folder's fold and guarded by a mutex, with the names sorted by fold, then real.
// - It is checked against the folder's modification time on every use and rescanned when the time differs.
// - A scan less than racy_window_ns away from the folder's time (either way, so a clock skew cannot pin it) is racy: a change in
//   the same tick of the file system's clock (FAT's 2 s, a coarse kernel tick) could keep the time unchanged, so a racy entry is
//   only trusted for racy_window_ns after its scan. A folder that keeps changing is therefore rescanned at most once per window,
//   and another process's change is seen within the window at the latest.
// - This source's own writes (a file or folder made or removed) update the entry in place, the one name inserted or erased, and
//   store the folder's time after the write, when the entry matched the folder's time just before it; otherwise they drop the names.
//   Making N files in a folder costs no rescan per file.
// - Every change bumps the entry's generation, and a scan stores its result only when the generation is still the one it started
//   from, so a scan that began before a write never puts stale names back.
// Names that are not valid_name (they could never be asked for) are left out of the cache and of listings.
// Writes reuse an existing case-insensitive match (writing "SAVE.TXT" over "save.txt" changes save.txt, on Linux as on Windows),
// and create a new file with the name as given.
//
// With create_root (data:/), a missing root folder reads as an empty folder, and the first write (a file or a folder) creates it
// with its parents. A read-only source refuses every change with access_denied.
#pragma once
#include <core/vfs/vfs.hpp>
#include <platform/file.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zodiac::vfs {
namespace detail {
// The FileError for a failed system call (not_found, not_a_directory, is_directory, access_denied, not_empty; io for anything else).
FileError file_error(const std::error_code& error) noexcept;
EntryKind entry_kind(platform::NodeKind kind) noexcept;
// Nanoseconds to milliseconds, rounded down.
std::int64_t to_ms(std::int64_t ns) noexcept;
platform::FileAccess access_of(WriteMode mode) noexcept;
std::shared_ptr<Reader> make_file_reader(platform::File file);
std::unique_ptr<WriteTarget> make_file_writer(platform::File file);
// Removes path and, for a folder, everything in it; links are removed, never followed.
std::error_code remove_tree(const std::filesystem::path& path);
// Creates path and every missing folder above it; success when it is already a folder.
std::error_code make_directories(const std::filesystem::path& path);
}
class FolderSource final:public Source{
public:
static constexpr std::int64_t racy_window_ns=2'000'000'000;
// A temporary file (temp_prefix) of a write or copy is taken for one a crash left behind, and removed when a writable source
// scans its folder, once its modification time is this old: a day, far beyond any copy (the time a folder listing shows may lag
// a file being written, and CopyFileExW may give the new file its source's time while it copies).
static constexpr std::int64_t stale_temp_ns=86'400'000'000'000;
// root is an absolute folder; it is not checked here.
FolderSource(std::filesystem::path root,bool read_only,bool create_root=false);
std::expected<EntryInfo,FileError> stat(std::string_view relative) override;
std::expected<std::shared_ptr<Reader>,FileError> open_read(std::string_view relative) override;
std::expected<std::unique_ptr<WriteTarget>,FileError> open_write(std::string_view relative,WriteMode mode) override;
std::expected<void,FileError> list(std::string_view relative,std::vector<EntryInfo>& out) override;
std::expected<void,FileError> remove(std::string_view relative) override;
std::expected<void,FileError> make_directory(std::string_view relative) override;
std::expected<void,FileError> remove_directory(std::string_view relative,bool recursive) override;
// platform::replace_file, so readers of a replaced file keep reading it. to reuses an existing case-insensitive match as writes do;
// from and to naming one entry is a success that changes nothing. false (copy instead) when the system cannot rename across them.
std::expected<bool,MoveError> rename(std::string_view from,std::string_view to) override;
// The real host path (an existing case-insensitive match reused, its stored spelling kept, as writes do); for_write makes the
// root when it is missing (create_root) and refuses a read-only source with access_denied.
std::expected<std::optional<std::filesystem::path>,FileError> host_file(std::string_view relative,bool for_write) override;
// Drops the cached names of relative's folder.
void forget(std::string_view relative) override;
bool read_only() const noexcept override{return read_only_;}
bool real_spelling(std::string_view relative,std::string& real) override;
std::optional<std::pair<std::string,std::string>> ambiguous_names(std::string_view relative) override;
const std::filesystem::path& root() const noexcept{return root_;}
// How many times a folder has been listed for the cache (tests prove writes do not rescan).
std::uint64_t scans() const noexcept{return scans_.load(std::memory_order_relaxed);}
private:
struct Name{
std::string fold;
std::string real;
platform::NodeKind kind=platform::NodeKind::none;
};
// One scanned folder: its real path below the root, the modification time its names belong to, when it was scanned, and its names
// sorted by fold, then real.
struct Names{
std::string real;
std::int64_t modified_ns=0;
std::int64_t scanned_ns=0;
bool racy=false;
std::vector<Name> names;
};
struct CacheEntry{
std::shared_ptr<const Names> names;
std::uint64_t generation=0;
};
struct Resolved{
std::string real;
platform::NodeInfo node;
// For a missing leaf (node kind none) found by the walk: the parent folder's time its cached names belong to.
std::optional<std::int64_t> parent_modified_ns;
};
// What one segment matched in a folder: found (name set), missing, or ambiguous.
struct Match{
const Name* name=nullptr;
bool ambiguous=false;
};
std::filesystem::path host_path(std::string_view real) const;
// The real path a write of relative (resolved as found) goes to: found.real, except on Windows, where an exact hit spells an
// existing entry as asked and the stored spelling is looked up, so a rename or copy over it never changes its name.
std::string write_name(std::string_view relative,const Resolved& found);
// The real path and the node of relative. With leaf_may_miss, a missing last segment (below an existing folder) gives the folder's
// real path joined with the segment as given and node kind none.
std::expected<Resolved,FileError> resolve(std::string_view relative,bool leaf_may_miss);
std::expected<std::shared_ptr<const Names>,FileError> folder_names(const std::string& real_folder);
static Match match(const Names& names,std::string_view segment);
// After this source made (added) or removed the entry leaf of kind in real_folder: updates the cached names in place when they
// belonged to before_ns, the folder's time just before the change, and drops them otherwise.
void note_change(std::string_view real_folder,std::string_view leaf,platform::NodeKind kind,bool added,std::optional<std::int64_t> before_ns);
// The folder's current time, when it can be read.
std::optional<std::int64_t> folder_time(std::string_view real_folder) const;
void invalidate(std::string_view real_folder);
void invalidate_tree(std::string_view real_folder);
// For create_root: creates the root folder when it is missing.
std::expected<void,FileError> ensure_root();
std::filesystem::path root_;
bool read_only_;
bool create_root_;
// Unique for the process's lifetime (never reused like an address), so a thread's recorded ambiguity can only match its own source.
std::uint64_t id_;
std::mutex cache_mutex_;
std::unordered_map<std::string,CacheEntry> cache_;
std::atomic<std::uint64_t> scans_{0};
};
}
