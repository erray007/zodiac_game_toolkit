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
//   volumes or sources: a rename moves no data.
//   append_file adds to the file in place (a failure may leave part of the added bytes). move_file is Vfs::move (a rename where
//   one source or the system can, else a copy, then a delete). One file on both sides (one entry under two spellings, or one host
//   file under two mounts or a host path) is a success that changes nothing.
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
#pragma once

class asIScriptEngine;

namespace zodiac {
namespace vfs {
class Vfs;
}
void register_files(asIScriptEngine* engine,vfs::Vfs& vfs);
}
