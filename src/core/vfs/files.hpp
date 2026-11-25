// The script's files (core-types spec 8.7): the file object and the file helpers, over a vfs::Vfs.
//
// register_files(engine, vfs) needs array<string> and date_time, so it comes after register_string and register_time. The engine
// keeps a small library object (freed with the engine) that the global functions bind to with asCALL_THISCALL_ASGLOBAL; vfs must
// outlive the engine. Every path is a VFS path (core/vfs/path.hpp), and each <path> in a text is the path as given, through one_line.
//
// Failure texts (vfs::failure_text): a path rule's, an ambiguity's or a read-only pack's own text, else `cannot <verb> "<path>":
// <reason>`, the verb by what failed: read (reading, "r", "r+" on a missing file, the from side of copy_file and move_file), write
// ("w" and "a", "r+" otherwise, write, sync, the commit in close, write_file, append_file, the to side of copy_file and move_file), delete
// (delete_file, delete_directory, and removing from after move_file's copy) or create (create_directory).
//
// file, a reference type: file f; (or file@ f = file();)
// - bool open(const string &in path, const string &in mode): modes "r" (read), "w" (create or empty), "a" (create or keep; every
//   write goes to the end), "r+" (an existing file, read and written), each with "b" for binary ("rb", "wb", "ab", "r+b" or "rb+").
//   The write modes read as well (as C's "w+" and "a+"). A mode not in that list throws `"<mode>" is not a file mode` and leaves an
//   open file as it was; otherwise an open file is closed first. A failed open returns false and sets error.
// - string read() (the rest), string read(int64 count) (count bytes, fewer at the end; "" for a count of 0 or less), string read_line()
//   (up to the next "\n" or "\r\n", without it; "" at the end). Text mode reads "\r\n" as "\n" and skips a UTF-8 BOM at position 0:
//   a read there that takes bytes steps over it first (read(0) moves nothing), so position counts the BOM once anything is read. A
//   read(count) whose last byte is the "\r" of a "\r\n" takes the "\n" too, so no line end is ever split. A lone "\r" stays. Binary
//   mode returns the bytes as they are. Writing never converts, so the bytes are the same everywhere.
// - bool write(const string &in data): at position (at the end in "a"), growing the file; false and error set when it fails (the
//   reason from the write target: "disk full", "access denied", ...).
// - int64 size, int64 position (bytes from the start of the file), bool eof (position >= size), all 0, 0 and true when closed;
//   bool seek(int64 position): false (nothing changed) outside 0 to size.
// - bool sync(): flushes what was written so far to the device (FlushFileBuffers; fdatasync, fsync on macOS); true at once for a
//   file opened "r"; false and error (`cannot write "<path>": <reason>`) when the flush fails.
// - void close() (a write target's commit failure sets error), bool is_open, string error: the last failure's text, cleared by
//   every successful open, write, sync and seek.
// - Reading, writing, syncing or seeking a closed file throws `file is not open`; writing a file opened "r" throws
//   `file is not open for writing`; a failed read (data that fails authentication included) throws `cannot read "<path>": <reason>`.
// - Reads go through one 64 KiB buffer per object, filled at 64 KiB-aligned offsets (allocated by the first small read, kept across
//   opens): a read of 64 KiB or more past what the buffer holds goes straight into the result string. read and read(count) build
//   their result in place (String::filled), so they copy no byte twice; read_line copies each line once out of the buffer (pieces
//   of a line longer than the buffer are appended). Writes keep the buffer in step. Not internally synchronized: one thread at a
//   time per object.
//
// Helpers (the failure rule of 8.7: normal failures return false or -1, reads of what must exist throw):
// - bool file_exists(path), bool directory_exists(path): queries; they never touch last_file_error.
// - string read_file(path): the bytes as they are; throws `cannot read "<path>": <reason>` (or the path rule's own text).
// - bool write_file(path, data, bool sync = false), bool append_file(path, data), bool delete_file(path),
//   bool copy_file(from, to, bool sync = false), bool move_file(from, to, bool sync = false): every one overwrites. write_file is
//   Vfs::write_file and copy_file Vfs::copy: the file they replace is always the old one or the complete new one (a temporary
//   file renamed over it), so a failed save never leaves half a file. Being a new file, it is no longer a hard link of any other
//   name the old one had (those keep the old bytes), and a dangling link at the path is replaced by the file rather than its
//   target being created; a link to an existing file is followed, and that file is replaced. By default the result survives a
//   crash of the game but not necessarily of the system; sync = true flushes the new bytes to the device before the rename (and,
//   POSIX, the folder after it), at the cost of a device flush. move_file's sync concerns only the copy it falls back to between
//   volumes or sources: a rename moves no data. With sync = true a false result may come after the new file has already replaced
//   the old one (the folder's flush after the rename, or a pack's flush after its commit, failed): a script must not take false
//   to mean that the old bytes are still there, only that the new ones may not have reached the device.
//   append_file adds to the file in place (a failure may leave part of the added bytes). move_file is Vfs::move (a rename where
//   one source or the system can, else a copy, then a delete). One file on both sides (one entry under two spellings, or one host
//   file under two mounts or a host path) is a success that changes nothing.
// - Temporary files of a crash are removed by name date, a day either way from the clock of the machine that scans: on a folder
//   shared between machines, one whose clock is more than a day off may see its live temporary files removed by another, and its
//   write then fails, the target kept (no data is lost).
// - The file object's "w" stays a write in place (the file is emptied at open): it is a stream that a script writes, seeks in and
//   reads back while it is open, as C's "w+", and others may read it meanwhile; write_file is the way to replace a file at once.
// - int64 file_size(path): -1 when it is not a file; date_time file_modified(path): the local civil time (date_time::now()'s clock:
//   platform::utc_to_local_civil_ms of the entry's UTC milliseconds) of a file or folder; throws like read_file.
// - bool create_directory(path, bool recursive = true), bool delete_directory(path, bool recursive = false).
// - array<string>@ list_files(path, const string &in pattern = "*"), array<string>@ list_directories(path): the names (Vfs::list:
//   '*' and '?' matched case-insensitively by fold_name, sorted by fold, then bytes); empty, with last_file_error set, on failure.
// - string last_file_error(): the calling thread's last helper failure (thread_local). The helpers that return bool, and the lists,
//   set it on failure and clear it on success.
// Each throws std::runtime_error naming the declaration when the engine refuses one.
//
// register_pack(engine, vfs) (src/core/pack/pack_binding.cpp; spec 8.3 of the library-and-packs design) comes after
// register_files: pack, a reference type (pack p; or pack@ p = pack();), over pack::PackStorage (core/pack/pack_storage.hpp).
// Each <name> in a text is the entry name as given, through one_line; an entry name is a VFS file path of the root mount
// (pack_format.hpp's canonical_entry_name: "sounds\a.ogg" is "sounds/a.ogg"), matched by fold_name.
// - bool open(path, key, bool create = false): the pack file at VFS path path with the password key (Argon2id). It must be a
//   plain file of a folder mount (or a host path): one inside a pack fails with `"<path>" is inside a pack; a pack file must
//   be a plain file`. create makes the pack when nothing is there (in the newest writable mount covering path); without it a
//   missing file fails with `cannot read "<path>": file not found`. A wrong key or a damaged pack fails with `wrong key or
//   damaged pack`, a pack of a newer format with `unsupported pack (newer format)`, a system failure with `cannot read|write
//   "<path>": <reason>`, a pack another program has open for writing with `"<path>" is in use by another program`. An open
//   pack is closed first. A pack found through a read-only mount, or a file the system will not
//   let it write, is read only: its changes fail with `cannot write "<path>": access denied`.
// - void close() (also on release and before a new open): flushes (a failure sets error), removes the mounts this object made
//   and drops this object's hold on the storage; other pack objects, their mounts and files opened through any mount keep it
//   open until the last one goes.
//   bool is_open, string error: the last failure's text, cleared by every success.
// - bool exists(name) (a file entry; deletion markers and invalid names are not), array<string>@ list() (the file entries'
//   names sorted by bytes), int count, int64 size(name) (-1 when there is no such file).
// - bool add_file(vfs_path, name) (the file's bytes, read in batches, and its modification time), bool add(name, data) (any
//   bytes, now as its time), bool remove(name): false and error set on failure: `"<name>" is not a valid entry name`,
//   `cannot read "<vfs_path>": <reason>`, `cannot write "<path>": <reason>`, `cannot delete "<name>" from the pack: file not
//   found` (remove of a name with no file entry, a deletion marker included: exists is false for it too).
// - string read(name): throws `cannot read "<name>" from the pack: <reason>` (file not found, damaged data, ...) or
//   `"<name>" is not a valid entry name`. bool extract(name, vfs_path): Vfs::write_from, so vfs_path is its old file or the
//   whole entry; false with that read text, or `cannot write "<vfs_path>": <reason>`.
// - bool compact(), bool flush(), int64 wasted_bytes.
// - On a pack that is not open: read throws and the changes fail with `pack is not open`; exists is false, list empty, count
//   and wasted_bytes 0, size -1.
// - bool mount_pack(pack@ p, const string &in at = "/", bool writable = false): mounts p's pack at VFS folder at (any folder,
//   data:/ and its subfolders included; never a host path), in front of everything mounted before it there (pack_source.hpp,
//   the resolution and write rules of vfs.hpp): entry names are paths below at. false with p.error set: `pack is not open`,
//   the path rule's text, `cannot write "<path>": access denied` (writable on a pack p cannot change), `already mounted
//   there` (p has a mount at that folder already). void unmount_pack(pack@ p) removes every mount p made; lookups through
//   them stop at once, while files already open read on. A null handle throws `Null pointer access`.
// The pack object is not internally synchronized (one thread at a time); the storage under it is.
#pragma once

class asIScriptEngine;

namespace zodiac {
namespace vfs {
class Vfs;
}
void register_files(asIScriptEngine* engine,vfs::Vfs& vfs);
void register_pack(asIScriptEngine* engine,vfs::Vfs& vfs);
}
