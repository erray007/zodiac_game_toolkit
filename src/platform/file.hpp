// The operating system's files and folders for the virtual file system (core/vfs), implemented per platform in
// src/platform/<os>/file.cpp. Paths are std::filesystem paths (built from UTF-8 with path_from_utf8); names come back as UTF-8.
// - File: one open file, move-only. Reads and writes are positional (pread/pwrite, ReadFile/WriteFile with an OVERLAPPED offset),
//   so the file position is never used: read_at is safe from several threads on one File, and different Files are independent.
//   Windows opens every file with FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, so an open file never blocks another reader,
//   a writer, a rename or a delete (POSIX never blocks those). Long Windows paths (248 characters and more) get the \\?\ prefix.
// - Errors: open, stat_path, list_directory and the folder functions report std::error_code values of std::generic_category (errno
//   numbers; Windows maps its own codes to them), and read_at returns the negated errno number. error_kind() classifies any code,
//   std::filesystem's system_category ones included.
// - stat_path follows links (on Windows a reparse point is opened to read its target). Times are nanoseconds since
//   1970-01-01T00:00:00 UTC, clamped to the int64 range.
// - replace_file: moves from over to, replacing it at once; readers of the old file keep reading it (POSIX rename semantics,
//   SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS|POSIX_SEMANTICS) on Windows, MoveFileExW(REPLACE_EXISTING|
//   WRITE_THROUGH) where the file system lacks it). A link at from is moved itself, never what it leads to (the handle is opened
//   with FILE_FLAG_OPEN_REPARSE_POINT on Windows, as POSIX rename does).
// - user_data_folder: where per-user game data lives: %APPDATA%, else the roaming application data folder, on Windows;
//   $XDG_DATA_HOME when it is absolute, else $HOME/.local/share, elsewhere. Empty when there is none. Nothing is created.
// Every function is thread-safe; a File object is used by one thread at a time except for read_at and size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace zodiac::platform {
// read and read_write open an existing file; create_truncate creates it or empties it; create_or_open creates it or keeps its bytes;
// create_new creates it and fails with EEXIST when anything is there (O_EXCL, CREATE_NEW: never through a link).
// Every mode but read opens the file for writing too. Opening a folder fails with EISDIR in every mode.
enum class FileAccess{read,read_write,create_truncate,create_or_open,create_new};
// Equal for two paths or handles of one file (links and other spellings included), different for different files.
struct FileIdentity{
std::uint64_t volume=0;
std::uint64_t high=0;
std::uint64_t low=0;
friend bool operator==(const FileIdentity&,const FileIdentity&)=default;
};
class File{
public:
File() noexcept=default;
File(File&& other) noexcept:handle_(std::exchange(other.handle_,invalid_handle)){}
File& operator=(File&& other) noexcept
{
if(this!=&other){
close();
handle_=std::exchange(other.handle_,invalid_handle);
}
return *this;
}
~File(){close();}
File(const File&)=delete;
File& operator=(const File&)=delete;
static std::expected<File,std::error_code> open(const std::filesystem::path& path,FileAccess access);
bool is_open() const noexcept{return handle_!=invalid_handle;}
// Fills buffer from offset on as far as the file goes: the bytes read (fewer only at the end), 0 at or past the end, or a
// negated errno number when nothing could be read.
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) const noexcept;
// Writes every byte at offset (growing the file as needed); the system's error when it fails (ENOSPC for a full disk, EIO when
// the system reports no progress without an error).
std::expected<void,std::error_code> write_at(std::uint64_t offset,std::span<const std::byte> bytes) noexcept;
// The file's current size; 0 when the system cannot tell.
std::uint64_t size() const noexcept;
std::expected<void,std::error_code> truncate(std::uint64_t size) noexcept;
// The file's identity: the volume and the file's number on it (st_dev and st_ino; the volume serial number and the 128-bit file
// id of FileIdInfo, or the 64-bit file index where the file system has no 128-bit id).
std::expected<FileIdentity,std::error_code> identity() const noexcept;
// Flushes the file's data and metadata to the device.
bool sync() noexcept;
void close() noexcept;
private:
static constexpr std::intptr_t invalid_handle=-1;
explicit File(std::intptr_t handle) noexcept:handle_(handle){}
// A HANDLE on Windows, a file descriptor elsewhere.
std::intptr_t handle_=invalid_handle;
};
enum class NodeKind{none,file,directory,other};
struct NodeInfo{
NodeKind kind=NodeKind::none;
std::uint64_t size=0;
std::int64_t modified_utc_ns=0;
};
std::expected<NodeInfo,std::error_code> stat_path(const std::filesystem::path& path);
struct DirectoryEntry{
// UTF-8 on Windows; the bytes as stored elsewhere. A Windows name that is not valid UTF-16 is left out, since no UTF-8 path can name it.
std::string name;
// The kind of what the entry leads to (links followed on POSIX; on Windows the entry's own attributes, which name a link to a folder a directory).
NodeKind kind=NodeKind::none;
// A symbolic link or other reparse point: removing it removes the link, never what it leads to.
bool link=false;
};
// True when path itself is a symbolic link or a junction (not followed); false when it is not or the system cannot tell.
bool is_link(const std::filesystem::path& path);
// Appends the entries of folder path to out, without "." and "..", in the system's order.
std::error_code list_directory(const std::filesystem::path& path,std::vector<DirectoryEntry>& out);
// Creates one folder; EEXIST when something is already there.
std::error_code make_directory(const std::filesystem::path& path);
// Removes a file or a link (a link to a folder included, never its target).
std::error_code remove_file(const std::filesystem::path& path);
// Removes an empty folder; ENOTEMPTY otherwise.
std::error_code remove_directory(const std::filesystem::path& path);
// cross_device: a rename between volumes (EXDEV, ERROR_NOT_SAME_DEVICE), which only a copy can do. disk_full: ENOSPC
// (ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL).
enum class ErrorKind{not_found,not_a_directory,is_directory,access_denied,exists,not_empty,cross_device,disk_full,other};
ErrorKind error_kind(const std::error_code& code) noexcept;
bool replace_file(const std::filesystem::path& from,const std::filesystem::path& to,std::error_code& error);
// The identity of the file at path (links followed); a folder has one too.
std::expected<FileIdentity,std::error_code> file_identity(const std::filesystem::path& path);
struct CopyResult{
// The copy failed; from_failed tells whether it was from (it cannot be opened or read) rather than to.
std::error_code error;
bool from_failed=false;
// from and to are one file (their open handles have one identity): nothing was written. An identity that cannot be read is an
// error, never taken for "different", so the source is never truncated on a guess.
bool same=false;
// Windows: the bytes were copied, but resetting the time and attributes, or removing the copied Zone.Identifier stream, failed.
std::error_code metadata;
};
// Copies the bytes of the regular file from over to (created or replaced) with the system's own copy: CopyFileExW on Windows,
// copy_file_range elsewhere (then sendfile, then read and write, where the kernel or file system declines or stops early; only the
// last decides that the file has ended). Both files are opened first and compared by identity, and to is emptied only after that.
// Only the bytes are meant to carry over, as with a copy written byte by byte: to gets the current time as its modification time,
// and Windows' copy of the time, of the read-only, hidden and system attributes and of the Zone.Identifier stream (the mark of the
// web) is undone; other alternate data streams are copied as CopyFileExW copies them. Nothing is promised about to after a failure.
// CopyFileExW refuses a hidden or system target with access denied. With create_new, to is made by the copy itself and must not
// exist (EEXIST otherwise): O_EXCL, COPY_FILE_FAIL_IF_EXISTS; no identity is compared then, since a file the copy made cannot be
// from. This is how the Vfs writes the temporary file it then renames over the target.
CopyResult copy_file(const std::filesystem::path& from,const std::filesystem::path& to,bool create_new=false);
// POSIX: gives path the permission bits of model (a file a replacement takes the place of), so a replaced file keeps them. Windows:
// nothing (a new file takes its folder's inherited ACL, as one written in place keeps its own). Success when model is missing.
std::error_code keep_permissions(const std::filesystem::path& model,const std::filesystem::path& path);
std::filesystem::path user_data_folder();
}
