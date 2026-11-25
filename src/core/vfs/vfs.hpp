// zodiac::vfs::Vfs: the virtual file system scripts (and, through the plugin ABI, plugins) open files through. Paths follow
// core/vfs/path.hpp; sources (folders, the host, later packs) are mounted at a mount point such as "/" or "data:/".
//
// Resolution: a path is served by the mounts of its mount ("" or "data:") whose mount point is the path or one of its folders, compared
// by fold_name segment by segment; the part below the mount point, spelled as given, goes to the source. Mounts are tried newest
// first: stat, open_read and remove take the first mount that has the entry (a mount that lacks it, or meets a file where a folder
// should be, passes the request on); open_write, create_directory and delete_directory go to the newest writable mount; rename,
// copy and move take both (see rename). When only
// read-only mounts cover a write, it fails with read_only_pack. list merges every covering mount's entries and the folders that
// lead to deeper mount points. A path that only leads to a mount point is a directory. "host:" paths (only parsed when host paths
// are on) go to the HostSource, which opens them exactly as given.
//
// Failures: Failure::code is the FileError; Failure::message is set for the failures with a text of their own:
// - the path rules (bad_path): `"<path>" leaves its mount`, `"<path>" is a host path; set [files] host_paths = true to use it`,
//   `"<path>" is not a valid file name`;
// - ambiguous: `"<path>" matches both "<a>" and "<b>"; rename one of them`, a and b the two real paths in the script's form
//   ("sounds/a.txt", "data:/A.txt"), sorted by bytes, as the failed lookup itself saw them; a source that cannot name them gives
//   `"<path>" matches more than one name; rename one of them`, so an ambiguity is never worded as a read failure;
// - not_empty has no text of its own: "directory not empty" (a folder removed without recursive that still holds entries);
// - read_only_pack: `"<path>" is in a read-only pack; save files in data:/ instead`.
// Every other failure leaves message empty; failure_text gives `cannot read|write|delete|create "<path>": <reason_text>` for
// those, the verb chosen by the caller (read_failure_text, write_failure_text).
// A file operation on a path in folder form ("", "sounds/") fails with is_directory before any mount is asked.
//
// Dev mode: a lookup whose spelling differs from the names on disk only by case (a FolderSource found it case-insensitively, or
// the file system itself ignores case) warns once per path through the log:
// `warning: "<path>" was found as "<real path>"; use the real name, other systems may not find it`. With a release pack check set
// (Task 10), the first write to each path of the root mount that the check names warns once:
// `warning: "<path>" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead`.
// A system copy (Windows) whose bytes are right but whose time, attributes or Zone.Identifier could not be reset warns:
// `warning: "<path>" was copied, but the time and attributes it took from its source could not be reset: <reason>`.
// Release mode (the default) never checks spelling.
//
// No C++ exception leaves any Vfs entry point. Anything thrown inside a file operation (an allocation failure, a logic error)
// becomes FileError::io, and in dev mode the log gets one line: `error: internal error in <operation> of "<path>": <what>`
// ("unknown exception" when it is no std::exception). mount and mount_folder return false, unmount returns 0 and changes nothing,
// and parse returns an error. The advisory callbacks (the log, the release pack check) are run so that a throw from them is
// reported the same way and never turns a finished operation into a failure.
//
// Threads: the file operations, mount and unmount may be called from any thread at any time. The mount table is copy-on-write
// behind a shared_mutex: a lookup copies a shared_ptr<const Table> under the shared lock and resolves without holding it, so a
// lookup sees the table of one moment and an unmount never waits for (or breaks) a read in progress; a reader keeps its source
// alive. The setters run before the Vfs is shared with other threads.
#pragma once
#include <core/vfs/path.hpp>
#include <zodiac/plugin.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zodiac::vfs {
// Append-only: a new code goes at the end, so no value ever moves.
enum class FileError:int{not_found=1,access_denied,damaged,is_directory,not_a_directory,read_only_pack,bad_path,ambiguous,io,not_empty,disk_full};
// "file not found", "access denied", "damaged data", "is a directory", "not a directory", "in a read-only pack", "invalid path",
// "ambiguous name", "input/output error", "directory not empty", "disk full".
std::string_view reason_text(FileError error) noexcept;
// deleted is a deletion marker (a patch pack hiding an older entry, Task 8): stat reports it, list leaves it out.
enum class EntryKind{none,file,directory,deleted};
struct EntryInfo{
EntryKind kind=EntryKind::none;
std::uint64_t size=0;
// Milliseconds since 1970-01-01T00:00:00 UTC, rounded down.
std::int64_t modified_utc_ms=0;
// From a listing, the entry's name as the source stores it; from stat, the last segment of the path as asked (on every platform,
// so a file system that ignores case answers like one that does not).
std::string name;
// From a listing: fold_name(name) when the source already has it (Vfs::list folds the name itself otherwise); empty from stat.
std::string fold;
};
enum class WriteMode{truncate,append,update};
enum class ListKind{files,directories};
// Reads one opened file. Used by one thread at a time; different readers may run concurrently, also on one file.
class Reader{
public:
virtual ~Reader()=default;
// Fills buffer from offset on: the bytes read (fewer only at the end), 0 at or past the end, or -int(FileError) on failure.
virtual std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer)=0;
// The size when the file was opened.
virtual std::uint64_t size() const=0;
};
// Writes one opened file (open_write). Used by one thread at a time. read_at reads what the file holds now, written bytes included
// (the "r+" mode of the script file object). commit() ends the writing: a folder's file has every byte already; a pack's write
// target stores its entry there (Task 8). Nothing is promised about bytes written after a failed write_at. A failed write_at,
// truncate or commit says why (disk_full, access_denied, io, ...).
class WriteTarget{
public:
virtual ~WriteTarget()=default;
virtual std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer)=0;
virtual std::expected<void,FileError> write_at(std::uint64_t offset,std::span<const std::byte> bytes)=0;
virtual std::uint64_t size() const=0;
virtual std::expected<void,FileError> truncate(std::uint64_t size)=0;
virtual std::expected<void,FileError> commit()=0;
// Flushes what has been written so far (and, after commit, the stored entry) to the device, so it survives a crash of the system:
// a folder's file is File::sync_data. A source must really flush here, or fail; it may never report a flush it did not make.
virtual std::expected<void,FileError> sync()=0;
};
// A failure about one of two paths (rename, copy, move): to is false for the first (from), true for the second (to).
struct MoveError{
FileError code=FileError::io;
bool to=false;
};
// Something mounted: relative paths are '/'-joined valid names (parse_path's relative), "" for the source's own root folder.
// Every function may be called from several threads at once.
class Source{
public:
virtual ~Source()=default;
virtual std::expected<EntryInfo,FileError> stat(std::string_view relative)=0;
virtual std::expected<std::shared_ptr<Reader>,FileError> open_read(std::string_view relative)=0;
// truncate creates or empties the file, append creates it or keeps its bytes, update needs an existing file. The folder it is in must exist.
virtual std::expected<std::unique_ptr<WriteTarget>,FileError> open_write(std::string_view relative,WriteMode mode)=0;
// Appends the entries of folder relative (names, kinds; sizes and times may be 0).
virtual std::expected<void,FileError> list(std::string_view relative,std::vector<EntryInfo>& out)=0;
virtual std::expected<void,FileError> remove(std::string_view relative)=0;
// Creates one folder; succeeds when it already exists. Its parent must exist.
virtual std::expected<void,FileError> make_directory(std::string_view relative)=0;
virtual std::expected<void,FileError> remove_directory(std::string_view relative,bool recursive)=0;
// Moves the file from over to (replacing a file there) at once: true when done; false when this source cannot, so the Vfs copies
// and removes instead (a source without renames, or two places the system cannot rename between). The default returns false.
virtual std::expected<bool,MoveError> rename(std::string_view from,std::string_view to);
// The host file behind relative, for the Vfs's own renames and copies between sources: with for_write the place a write of
// relative would create or replace (its folder must exist; a source that makes its root on the first write makes it here),
// otherwise the existing file. nullopt from a source that is no host folder (the default), or when this one has no such file
// to hand out; a failure as open_write or open_read would report it. A source without host files must make a truncating
// open_write's bytes replace the entry only at commit (a pack does), so Vfs::copy and Vfs::write_file never leave a part.
virtual std::expected<std::optional<std::filesystem::path>,FileError> host_file(std::string_view relative,bool for_write);
// Tells the source that the file relative was made, replaced or removed behind its back (through host_file), so whatever it
// caches about its folder is dropped. The default does nothing.
virtual void forget(std::string_view relative);
virtual bool read_only() const noexcept=0;
// When an entry exists at relative and its real spelling differs from relative (only by case), sets real to that spelling and
// returns true. Sources without case folding keep the default.
virtual bool real_spelling(std::string_view relative,std::string& real);
// After a lookup of relative failed with ambiguous: the two real paths (below the source's root) that matched first, sorted by bytes.
virtual std::optional<std::pair<std::string,std::string>> ambiguous_names(std::string_view relative);
};
struct Failure{
FileError code=FileError::io;
// The whole one-line text for the failures that have their own (see above); empty otherwise.
std::string message;
};
// What failed, for the text: `cannot read`, `cannot write`, `cannot delete`, `cannot create`.
enum class FileAction{read,write,remove,create};
// message when set, else `cannot <action> "<path>": <reason_text(code)>`, path through one_line.
std::string failure_text(FileAction action,std::string_view path,const Failure& failure);
inline std::string read_failure_text(std::string_view path,const Failure& failure){return failure_text(FileAction::read,path,failure);}
inline std::string write_failure_text(std::string_view path,const Failure& failure){return failure_text(FileAction::write,path,failure);}
// The FileError of a failed Reader::read_at or WriteTarget::read_at result (-int(FileError)); io for a value that names none.
FileError read_error(std::int64_t result) noexcept;
// A two-path failure (rename, copy, move): to tells which path it is about.
struct MoveFailure{
Failure failure;
bool to=false;
// move only: the copy succeeded, and removing from failed (from is still there, and so is the copy).
bool removing=false;
};
class HostSource;
class Vfs{
public:
using LogHandler=std::function<void(zodiac_log_level,std::string_view)>;
using PackCheck=std::function<bool(std::string_view relative)>;
Vfs();
~Vfs();
Vfs(const Vfs&)=delete;
Vfs& operator=(const Vfs&)=delete;
// False (nothing mounted) for a mount point that is not a valid VFS folder path (a host path included) or one where owner already
// has a mount. owner identifies the mount for unmount; nullptr makes it permanent.
bool mount(std::string_view at,std::shared_ptr<Source> source,bool writable,const void* owner);
// Removes every mount owner made (none for nullptr) and returns how many. Reads already open keep their sources.
std::size_t unmount(const void* owner);
// mount(at, a FolderSource for host, writable, owner); the source is read-only unless writable.
bool mount_folder(std::string_view at,const std::filesystem::path& host,bool writable,const void* owner=nullptr);
std::expected<EntryInfo,Failure> stat(std::string_view path);
std::expected<std::shared_ptr<Reader>,Failure> open_read(std::string_view path);
std::expected<std::unique_ptr<WriteTarget>,Failure> open_write(std::string_view path,WriteMode mode);
std::expected<void,Failure> remove(std::string_view path);
// Moves the file from over to at once: from as remove finds it (the first mount that has it; a read-only one fails with
// read_only_pack), to as open_write finds it (the newest writable mount covering it). One source renames itself (Source::rename);
// two sources that are both host folders, or a host path and a host folder, are renamed by the system (replace_file), after
// which both sources forget the paths. Whenever both sides are host files (in one source too) their identities are compared
// first (links followed): true when moved, or when from and to are one file (the same identity, a link to the other, or one entry
// under two spellings): nothing changes then. An identity that cannot be read fails with its side's error, never guessed. false
// when only a copy can do it (a source without host files, two volumes) and the two are known to be different files; nothing has
// changed then. A folder on either side is is_directory, named by MoveFailure::to.
std::expected<bool,MoveFailure> rename(std::string_view from,std::string_view to);
// Copies the bytes of file from over to (created or replaced), so that to is always either its old file or the complete copy:
// - to a host file (a folder or a host path): a new temporary file beside it (temp_prefix, never visible to scripts) is written,
//   by the system's copy (platform::copy_file with create_new) when from is a host file too, else from open_read in 256 KiB
//   pieces, and then renamed over to (through a link at to, to what it names); the temporary file is removed after any failure,
//   and the source of to forgets the path. A replaced file keeps its permission bits (POSIX).
// - to anything else: open_write (truncate) in 256 KiB pieces, then commit, which a source without host files makes the moment
//   the entry changes (Source::host_file).
// from and to being one file (by identity, when both are host files) is a success that changes nothing.
// With sync, the copy's bytes reach the device before the rename (and, POSIX, the folder's new entry after it), or before commit
// returns for a target without host files (WriteTarget::sync after commit). A failure of the folder's flush or of
// WriteTarget::sync comes after the replace: the new file is then in place, only perhaps not on the device yet.
std::expected<void,MoveFailure> copy(std::string_view from,std::string_view to,bool sync=false);
// Replaces file path with bytes as copy does: to a host file through a temporary file and a rename, else through open_write
// (truncate) and commit. Failures as open_write's. sync as for copy (a sync failure may come after the replace).
std::expected<void,Failure> write_file(std::string_view path,std::span<const std::byte> bytes,bool sync=false);
// Replaces file path with the bytes of from (read in 256 KiB pieces) as write_file does: to a host file through a temporary file
// and a rename, else through open_write (truncate) and commit; to stays its old file after any failure. MoveFailure::to is false
// for a failure of from, true for one of path. sync as for copy.
std::expected<void,MoveFailure> write_from(std::string_view path,Reader& from,bool sync=false);
// The host file behind path, for a client that opens it itself (a pack file): found as open_read finds it, or, with for_write,
// the place open_write would write (the newest writable mount covering it; the file need not exist). writable is the serving
// mount's writability (always true for a host path). nullopt when the source serving it has no host files (a pack). Failures as
// open_read's or open_write's, a folder form included (is_directory).
struct HostFile{
std::filesystem::path path;
bool writable=false;
};
std::expected<std::optional<HostFile>,Failure> host_file(std::string_view path,bool for_write);
// Tells every source covering path that its file was made or changed outside the Vfs (after host_file), so their cached
// listings are dropped. Never throws.
void forget(std::string_view path) noexcept;
// rename, else copy then remove(from). A copy that finds one file on both sides removes nothing. A failed remove after the copy
// is MoveFailure::removing (both files are then there).
// sync only concerns that copy: a rename moves no data.
std::expected<void,MoveFailure> move(std::string_view from,std::string_view to,bool sync=false);
// The names in folder path of the kind asked for that match pattern ('*' any run, '?' one character), name and pattern compared by
// fold_name; sorted by fold, then by bytes, without duplicates.
std::expected<std::vector<std::string>,Failure> list(std::string_view path,ListKind kind,std::string_view pattern="*");
// Creates the folder (and, when recursive, every missing folder above it). An existing folder is a success.
std::expected<void,Failure> create_directory(std::string_view path,bool recursive);
// Removes the folder (with everything in it when recursive; it must be empty otherwise). A mount's own folder is never removed (access_denied).
std::expected<void,Failure> delete_directory(std::string_view path,bool recursive);
void set_host_paths(bool enabled) noexcept{host_paths_.store(enabled,std::memory_order_relaxed);}
bool host_paths() const noexcept{return host_paths_.load(std::memory_order_relaxed);}
void set_dev_mode(bool enabled) noexcept{dev_mode_.store(enabled,std::memory_order_relaxed);}
bool dev_mode() const noexcept{return dev_mode_.load(std::memory_order_relaxed);}
void set_log(LogHandler log){log_=std::move(log);}
const LogHandler& log() const noexcept{return log_;}
void set_release_pack_check(PackCheck check){release_pack_check_=std::move(check);}
// Parses path under this Vfs's host path setting. Never throws: an internal failure is reported as `"<path>" is not a valid file name`.
std::expected<VfsPath,std::string> parse(std::string_view path) const noexcept;
// Called from inside a catch block: in dev mode, logs the exception being handled for operation on path. Never throws.
void report_internal_error(std::string_view operation,std::string_view path) const noexcept;
private:
struct Mount{
std::string mount;
std::string at;
std::string at_fold;
std::size_t at_segments=0;
std::shared_ptr<Source> source;
bool writable=false;
const void* owner=nullptr;
};
struct Table{
// Oldest first.
std::vector<Mount> mounts;
};
// A mount serving a path, and the part of the path below its mount point.
struct Target{
const Mount* mount=nullptr;
std::string_view sub;
};
// One side of a rename or copy: its source and the path below it, and its mount (none for a host path, which host_ serves).
struct End{
Source* source=nullptr;
const Mount* mount=nullptr;
std::string_view sub;
};
std::shared_ptr<const Table> snapshot() const;
// from's side, found as open_read finds it (as remove does when removing: a read-only mount then fails with read_only_pack), and
// to's side, found as open_write finds it. Both view p.
std::expected<End,Failure> reading_end(const Table& table,std::string_view path,const VfsPath& p,bool removing) const;
std::expected<End,Failure> writing_end(const Table& table,std::string_view path,const VfsPath& p) const;
Failure end_failure(std::string_view path,const End& end,FileError code) const;
// After to was written by a rename or copy: the dev-mode checks a write gets.
void written(std::string_view path,const VfsPath& p,const End& end) noexcept;
// Dev mode: a system copy whose bytes are right but whose metadata reset failed (platform::CopyResult::metadata). Never throws.
void warn_copy_metadata(std::string_view path,const std::error_code& error) const noexcept;
// copy; true when from and to turned out to be one file (nothing was written).
std::expected<bool,MoveFailure> copy_file(std::string_view from,std::string_view to,bool sync);
// Copies every byte of in into out in 256 KiB pieces, then commits (and syncs, with sync) it.
static std::expected<void,MoveFailure> pump(Reader& in,WriteTarget& out,bool sync);
// Calls visit(Target) for the mounts covering p, newest first, until it returns true; returns whether one did. Allocates nothing
// unless a mount point below the root has to be compared by fold.
template<class Visit>
static bool visit_covering(const Table& table,const VfsPath& p,Visit&& visit);
// True when p is a folder above some mount point of its mount (a path that only leads to mounts).
static bool leads_to_mount(const Table& table,const VfsPath& p);
std::expected<VfsPath,Failure> parse_failure(std::string_view path) const;
Failure ambiguous_failure(std::string_view path,const Target& target) const;
// Both are advisory and never throw: whatever their work or the callbacks throw is reported by report_internal_error.
void check_spelling(std::string_view path,const VfsPath& p,const Target& target) noexcept;
void check_release_pack(std::string_view path,const VfsPath& p) noexcept;
void warn(std::string_view line) const;
mutable std::shared_mutex table_mutex_;
std::shared_ptr<const Table> table_;
// Serializes mount and unmount, which build the next table outside table_mutex_ so lookups only ever wait for a pointer swap.
std::mutex writer_mutex_;
std::shared_ptr<HostSource> host_;
std::atomic<bool> host_paths_{false};
std::atomic<bool> dev_mode_{false};
LogHandler log_;
PackCheck release_pack_check_;
// Dev mode: 64-bit hashes of the paths whose spelling was checked, in a fixed open-addressed table of atomics, so the usual lookup
// (a path already checked) takes no lock and allocates nothing. Bounded: when a probe run is full its first slot is overwritten, and
// a forgotten path is only checked again (its warning, if any, is still given once: see spelling_warned_).
static constexpr std::size_t checked_slots=4096;
static constexpr std::size_t checked_probes=8;
std::unique_ptr<std::atomic<std::uint64_t>[]> checked_;
bool checked(std::uint64_t hash) const noexcept;
void remember_checked(std::uint64_t hash) noexcept;
// The paths warned about: misspelled ones (dev mode) and release pack writes. Only touched when there is something to warn about,
// and each holds at most max_warned paths (past that, a warning may repeat rather than the set grow).
static constexpr std::size_t max_warned=4096;
std::mutex warned_mutex_;
std::unordered_set<std::string> spelling_warned_;
std::unordered_set<std::string> pack_warned_;
};
}
