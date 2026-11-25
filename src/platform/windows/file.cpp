// Files and folders on Windows (see platform/file.hpp): CreateFileW with every share flag, positional ReadFile/WriteFile through an
// OVERLAPPED offset on a synchronous handle, FindFirstFileExW listings (basic information, large fetch), and Win32 error codes mapped to
// errno numbers, FileIdInfo identities and CopyFileExW copies. Paths of 248 characters and more get the \\?\ (or \\?\UNC\) prefix, which lifts MAX_PATH; callers pass absolute,
// lexically normal paths, which is what that prefix needs.
// Windows 10 is Zodiac's minimum; the toolchain's headers default to Windows 7 and hide FILE_ID_INFO (Windows 8).
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <platform/file.hpp>
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <new>
#include <string_view>

namespace zodiac::platform {
namespace {
namespace fs=std::filesystem;
constexpr DWORD share_all=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
// The largest single ReadFile/WriteFile request: well below 4 GiB, so the DWORD counts never wrap.
constexpr std::size_t io_chunk=0x40000000u;
HANDLE handle_of(std::intptr_t value) noexcept
{
return reinterpret_cast<HANDLE>(value);
}
int errno_of(DWORD error) noexcept
{
switch(error){
case ERROR_FILE_NOT_FOUND:
case ERROR_PATH_NOT_FOUND:
case ERROR_INVALID_NAME:
case ERROR_INVALID_DRIVE:
case ERROR_BAD_NETPATH:
case ERROR_BAD_NET_NAME:
case ERROR_BAD_PATHNAME:
case ERROR_NOT_READY:
case ERROR_DELETE_PENDING:
return ENOENT;
case ERROR_ACCESS_DENIED:
case ERROR_SHARING_VIOLATION:
case ERROR_LOCK_VIOLATION:
case ERROR_WRITE_PROTECT:
case ERROR_NETWORK_ACCESS_DENIED:
case ERROR_CANNOT_MAKE:
return EACCES;
case ERROR_DIRECTORY:
return ENOTDIR;
case ERROR_FILE_EXISTS:
case ERROR_ALREADY_EXISTS:
return EEXIST;
case ERROR_DIR_NOT_EMPTY:
return ENOTEMPTY;
case ERROR_DISK_FULL:
case ERROR_HANDLE_DISK_FULL:
return ENOSPC;
case ERROR_NOT_ENOUGH_MEMORY:
case ERROR_OUTOFMEMORY:
return ENOMEM;
case ERROR_FILENAME_EXCED_RANGE:
return ENAMETOOLONG;
case ERROR_INVALID_PARAMETER:
return EINVAL;
case ERROR_NOT_SAME_DEVICE:
return EXDEV;
default:
return EIO;
}
}
std::error_code errno_code(int value) noexcept
{
return std::error_code(value,std::generic_category());
}
std::error_code last_error() noexcept
{
return errno_code(errno_of(GetLastError()));
}
// The identity of an open handle: FileIdInfo's volume serial number and 128-bit id, or, where the file system has none (FAT and
// some network file systems refuse FileIdInfo), GetFileInformationByHandle's serial number and 64-bit index.
std::expected<FileIdentity,std::error_code> identity_of(HANDLE handle) noexcept
{
FILE_ID_INFO id;
if(GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof id)){
FileIdentity identity;
identity.volume=id.VolumeSerialNumber;
std::memcpy(&identity.high,id.FileId.Identifier+8,8);
std::memcpy(&identity.low,id.FileId.Identifier,8);
return identity;
}
BY_HANDLE_FILE_INFORMATION info;
if(!GetFileInformationByHandle(handle,&info))return std::unexpected(last_error());
FileIdentity identity;
identity.volume=info.dwVolumeSerialNumber;
identity.low=(static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
return identity;
}
// The text Win32 receives for a path: the path itself when short (Win32 accepts '/' there), else a \\?\ copy with '\' separators.
// It points into the path it was made from, which must outlive it.
class NativePath{
public:
explicit NativePath(const fs::path&&)=delete;
explicit NativePath(const fs::path& path)
{
const std::wstring& text=path.native();
text_=text.c_str();
// 248 is CreateDirectoryW's limit (MAX_PATH less room for an 8.3 name); every other call allows 260.
if(text.size()<248||text.starts_with(L"\\\\?\\")||text.starts_with(L"//?/"))return;
std::wstring preferred=text;
std::ranges::replace(preferred,L'/',L'\\');
if(preferred.starts_with(L"\\\\"))long_.append(L"\\\\?\\UNC\\").append(std::wstring_view(preferred).substr(2));
else if(preferred.size()>=3&&preferred[1]==L':'&&preferred[2]==L'\\')long_.append(L"\\\\?\\").append(preferred);
else return;
text_=long_.c_str();
}
NativePath(const NativePath&)=delete;
NativePath& operator=(const NativePath&)=delete;
const wchar_t* c_str() const noexcept{return text_;}
private:
std::wstring long_;
const wchar_t* text_;
};
std::int64_t unix_ns(FILETIME time) noexcept
{
// FILETIME counts 100 ns ticks since 1601-01-01; 116444736000000000 of them precede 1970.
const std::uint64_t ticks=(static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;
if(ticks>static_cast<std::uint64_t>(INT64_MAX))return INT64_MAX;
const std::int64_t since=static_cast<std::int64_t>(ticks)-116444736000000000;
constexpr std::int64_t limit=INT64_MAX/100;
if(since>limit)return INT64_MAX;
if(since< -limit)return INT64_MIN;
return since*100;
}
// A UTF-8 copy of a file name, or false when the name is not valid UTF-16 (a lone surrogate), which no UTF-8 path could name.
bool utf8_name(const wchar_t* name,std::string& out)
{
const int length=static_cast<int>(std::wcslen(name));
if(length==0)return false;
const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name,length,nullptr,0,nullptr,nullptr);
if(size<=0)return false;
out.resize_and_overwrite(static_cast<std::size_t>(size),[name,length](char* buffer,std::size_t count){
return static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name,length,buffer,static_cast<int>(count),nullptr,nullptr));
});
return out.size()==static_cast<std::size_t>(size);
}
// A symbolic link or a junction; other reparse points (cloud placeholders, deduplicated files) are ordinary files and folders.
bool is_link_entry(DWORD attributes,DWORD tag) noexcept
{
return (attributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0&&(tag==IO_REPARSE_TAG_SYMLINK||tag==IO_REPARSE_TAG_MOUNT_POINT);
}
// FILE_RENAME_INFO as Windows 10 lays it out (the flags share a union with ReplaceIfExists), whatever the headers' target version.
struct RenameInfo{
DWORD flags;
HANDLE root;
DWORD length;
WCHAR name[1];
};
constexpr DWORD rename_replace_if_exists=0x1;
constexpr DWORD rename_posix_semantics=0x2;
constexpr auto file_rename_info_ex=static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
}

std::expected<File,std::error_code> File::open(const fs::path& path,FileAccess access)
{
const NativePath name(path);
DWORD desired=GENERIC_READ;
DWORD disposition=OPEN_EXISTING;
switch(access){
case FileAccess::read:
break;
case FileAccess::read_write:
desired|=GENERIC_WRITE;
break;
case FileAccess::create_truncate:
desired|=GENERIC_WRITE;
disposition=CREATE_ALWAYS;
break;
case FileAccess::create_or_open:
desired|=GENERIC_WRITE;
disposition=OPEN_ALWAYS;
break;
case FileAccess::create_new:
case FileAccess::create_private:
desired|=GENERIC_WRITE;
disposition=CREATE_NEW;
break;
}
const HANDLE handle=CreateFileW(name.c_str(),desired,share_all,nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
if(handle!=INVALID_HANDLE_VALUE)return File(reinterpret_cast<std::intptr_t>(handle));
const DWORD error=GetLastError();
// A folder cannot be opened without FILE_FLAG_BACKUP_SEMANTICS, which reports it as access denied.
if(error==ERROR_ACCESS_DENIED){
const DWORD attributes=GetFileAttributesW(name.c_str());
if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return std::unexpected(errno_code(EISDIR));
}
return std::unexpected(errno_code(errno_of(error)));
}

std::int64_t File::read_at(std::uint64_t offset,std::span<std::byte> buffer) const noexcept
{
std::size_t done=0;
while(done<buffer.size()){
const DWORD chunk=static_cast<DWORD>(std::min(buffer.size()-done,io_chunk));
const std::uint64_t at=offset+done;
OVERLAPPED overlapped{};
overlapped.Offset=static_cast<DWORD>(at);
overlapped.OffsetHigh=static_cast<DWORD>(at>>32);
DWORD got=0;
if(!ReadFile(handle_of(handle_),buffer.data()+done,chunk,&got,&overlapped)){
const DWORD error=GetLastError();
if(error==ERROR_HANDLE_EOF||done>0)break;
return -static_cast<std::int64_t>(errno_of(error));
}
if(got==0)break;
done+=got;
}
return static_cast<std::int64_t>(done);
}

std::expected<void,std::error_code> File::write_at(std::uint64_t offset,std::span<const std::byte> bytes) noexcept
{
std::size_t done=0;
while(done<bytes.size()){
const DWORD chunk=static_cast<DWORD>(std::min(bytes.size()-done,io_chunk));
const std::uint64_t at=offset+done;
OVERLAPPED overlapped{};
overlapped.Offset=static_cast<DWORD>(at);
overlapped.OffsetHigh=static_cast<DWORD>(at>>32);
DWORD put=0;
if(!WriteFile(handle_of(handle_),bytes.data()+done,chunk,&put,&overlapped))return std::unexpected(last_error());
if(put==0)return std::unexpected(errno_code(EIO));
done+=put;
}
return {};
}

std::uint64_t File::size() const noexcept
{
LARGE_INTEGER size;
return GetFileSizeEx(handle_of(handle_),&size)?static_cast<std::uint64_t>(size.QuadPart):0;
}

std::expected<void,std::error_code> File::truncate(std::uint64_t size) noexcept
{
if(size>static_cast<std::uint64_t>(INT64_MAX))return std::unexpected(errno_code(EFBIG));
FILE_END_OF_FILE_INFO info;
info.EndOfFile.QuadPart=static_cast<LONGLONG>(size);
if(!SetFileInformationByHandle(handle_of(handle_),FileEndOfFileInfo,&info,sizeof info))return std::unexpected(last_error());
return {};
}

std::expected<FileIdentity,std::error_code> File::identity() const noexcept
{
return identity_of(handle_of(handle_));
}

std::expected<bool,std::error_code> File::lock_exclusive() noexcept
{
// The byte at 2^64 - 2: offsets past the end of a file may be locked, and no file ever has data there.
OVERLAPPED at{};
at.Offset=0xFFFFFFFEu;
at.OffsetHigh=0xFFFFFFFFu;
if(LockFileEx(handle_of(handle_),LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&at))return true;
if(GetLastError()==ERROR_LOCK_VIOLATION)return false;
return std::unexpected(last_error());
}

void File::unlock() noexcept
{
OVERLAPPED at{};
at.Offset=0xFFFFFFFEu;
at.OffsetHigh=0xFFFFFFFFu;
// Fails harmlessly (ERROR_NOT_LOCKED) when the byte is not locked.
UnlockFileEx(handle_of(handle_),0,1,0,&at);
}

bool File::sync() noexcept
{
return FlushFileBuffers(handle_of(handle_))!=0;
}

std::expected<void,std::error_code> File::sync_data() noexcept
{
if(!FlushFileBuffers(handle_of(handle_)))return std::unexpected(last_error());
return {};
}

std::error_code File::copy_mode(const fs::path&) noexcept
{
return {};
}

void File::close() noexcept
{
if(handle_==invalid_handle)return;
CloseHandle(handle_of(handle_));
handle_=invalid_handle;
}

std::expected<NodeInfo,std::error_code> stat_path(const fs::path& path)
{
const NativePath name(path);
WIN32_FILE_ATTRIBUTE_DATA data;
if(!GetFileAttributesExW(name.c_str(),GetFileExInfoStandard,&data))return std::unexpected(last_error());
DWORD attributes=data.dwFileAttributes;
FILETIME modified=data.ftLastWriteTime;
std::uint64_t size=(static_cast<std::uint64_t>(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
// The attributes describe a link itself; its target is read through a handle, which follows it.
if(attributes&FILE_ATTRIBUTE_REPARSE_POINT){
const HANDLE handle=CreateFileW(name.c_str(),FILE_READ_ATTRIBUTES,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
if(handle==INVALID_HANDLE_VALUE)return std::unexpected(last_error());
BY_HANDLE_FILE_INFORMATION info;
const BOOL got=GetFileInformationByHandle(handle,&info);
const std::error_code error=got?std::error_code():last_error();
CloseHandle(handle);
if(!got)return std::unexpected(error);
attributes=info.dwFileAttributes;
modified=info.ftLastWriteTime;
size=(static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
}
NodeInfo node;
node.kind=(attributes&FILE_ATTRIBUTE_DIRECTORY)?NodeKind::directory:(attributes&FILE_ATTRIBUTE_DEVICE)?NodeKind::other:NodeKind::file;
node.size=node.kind==NodeKind::file?size:0;
node.modified_utc_ns=unix_ns(modified);
return node;
}

bool is_link(const fs::path& path)
{
const NativePath name(path);
const DWORD attributes=GetFileAttributesW(name.c_str());
if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
// The reparse tag is only reported by a directory search on the path itself.
WIN32_FIND_DATAW data;
const HANDLE find=FindFirstFileExW(name.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,0);
if(find==INVALID_HANDLE_VALUE)return false;
FindClose(find);
return is_link_entry(data.dwFileAttributes,data.dwReserved0);
}

std::error_code list_directory(const fs::path& path,std::vector<DirectoryEntry>& out)
{
// NativePath keeps a pointer into the path it is given, so the pattern is a named object, never a temporary.
const fs::path pattern=path/L"*";
const NativePath name(pattern);
WIN32_FIND_DATAW data;
const HANDLE find=FindFirstFileExW(name.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,FIND_FIRST_EX_LARGE_FETCH);
if(find==INVALID_HANDLE_VALUE){
const DWORD error=GetLastError();
// No entry at all (a drive's empty root has no "." either).
if(error==ERROR_FILE_NOT_FOUND)return {};
return errno_code(errno_of(error));
}
std::error_code result;
try{
do{
const std::wstring_view file(data.cFileName);
if(file==L"."||file==L"..")continue;
DirectoryEntry entry;
if(!utf8_name(data.cFileName,entry.name))continue;
entry.kind=(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?NodeKind::directory:(data.dwFileAttributes&FILE_ATTRIBUTE_DEVICE)?NodeKind::other:NodeKind::file;
entry.link=is_link_entry(data.dwFileAttributes,data.dwReserved0);
out.push_back(std::move(entry));
}while(FindNextFileW(find,&data));
const DWORD error=GetLastError();
if(error!=ERROR_NO_MORE_FILES)result=errno_code(errno_of(error));
}
catch(...){
FindClose(find);
throw;
}
FindClose(find);
return result;
}

std::error_code make_directory(const fs::path& path)
{
const NativePath name(path);
return CreateDirectoryW(name.c_str(),nullptr)?std::error_code():last_error();
}

std::error_code remove_file(const fs::path& path)
{
const NativePath name(path);
if(DeleteFileW(name.c_str()))return {};
const std::error_code error=last_error();
// A link to a folder is itself a folder entry; RemoveDirectoryW removes the link and leaves its target alone.
const DWORD attributes=GetFileAttributesW(name.c_str());
if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){
return RemoveDirectoryW(name.c_str())?std::error_code():last_error();
}
if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return errno_code(EISDIR);
return error;
}

std::error_code remove_directory(const fs::path& path)
{
const NativePath name(path);
return RemoveDirectoryW(name.c_str())?std::error_code():last_error();
}

ErrorKind error_kind(const std::error_code& code) noexcept
{
int value=code.value();
if(code.category()==std::system_category())value=errno_of(static_cast<DWORD>(value));
else if(code.category()!=std::generic_category()){
const std::error_condition condition=code.default_error_condition();
if(condition.category()!=std::generic_category())return ErrorKind::other;
value=condition.value();
}
switch(value){
case ENOENT:
return ErrorKind::not_found;
case ENOTDIR:
return ErrorKind::not_a_directory;
case EISDIR:
return ErrorKind::is_directory;
case EACCES:
case EPERM:
return ErrorKind::access_denied;
case EEXIST:
return ErrorKind::exists;
case ENOTEMPTY:
return ErrorKind::not_empty;
case EXDEV:
return ErrorKind::cross_device;
case ENOSPC:
return ErrorKind::disk_full;
default:
return ErrorKind::other;
}
}

bool replace_file(const fs::path& from,const fs::path& to,std::error_code& error)
{
error.clear();
const NativePath source(from);
const NativePath target(to);
// The link itself, as POSIX rename moves it, never its target.
const HANDLE handle=CreateFileW(source.c_str(),DELETE|SYNCHRONIZE,share_all,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
if(handle!=INVALID_HANDLE_VALUE){
const std::size_t length=std::wcslen(target.c_str());
const std::size_t bytes=offsetof(RenameInfo,name)+(length+1)*sizeof(WCHAR);
// 8-byte units keep the HANDLE member aligned.
std::vector<std::uint64_t> buffer((bytes+7)/8);
auto* info=new(buffer.data()) RenameInfo{};
info->flags=rename_replace_if_exists|rename_posix_semantics;
info->root=nullptr;
info->length=static_cast<DWORD>(length*sizeof(WCHAR));
std::memcpy(reinterpret_cast<std::byte*>(buffer.data())+offsetof(RenameInfo,name),target.c_str(),(length+1)*sizeof(WCHAR));
const BOOL renamed=SetFileInformationByHandle(handle,file_rename_info_ex,info,static_cast<DWORD>(bytes));
CloseHandle(handle);
if(renamed)return true;
}
// File systems without POSIX renames (FAT, older Windows, some network shares).
if(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
error=last_error();
return false;
}

std::expected<FileIdentity,std::error_code> file_identity(const fs::path& path)
{
const NativePath name(path);
// No access rights: reading the id needs none, and a folder opens with FILE_FLAG_BACKUP_SEMANTICS.
const HANDLE handle=CreateFileW(name.c_str(),0,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
if(handle==INVALID_HANDLE_VALUE)return std::unexpected(last_error());
const auto identity=identity_of(handle);
CloseHandle(handle);
return identity;
}

namespace {
// Closes a handle on every exit.
struct OwnedHandle{
HANDLE handle;
~OwnedHandle(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
// After CopyFileExW: a fresh modification time, no read-only, hidden or system attribute and no Zone.Identifier stream, as a copy
// written byte by byte would have. The bytes are right whatever this reports.
std::atomic<bool> fail_reset{false};
std::error_code reset_copy(const fs::path& to,const NativePath& target)
{
if(fail_reset.exchange(false,std::memory_order_relaxed))return errno_code(EACCES);
std::error_code error;
{
const OwnedHandle handle{CreateFileW(target.c_str(),FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,share_all,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
if(handle.handle==INVALID_HANDLE_VALUE)return last_error();
FILE_BASIC_INFO info;
if(!GetFileInformationByHandleEx(handle.handle,FileBasicInfo,&info,sizeof info))return last_error();
FILETIME now;
GetSystemTimePreciseAsFileTime(&now);
info.LastWriteTime.LowPart=now.dwLowDateTime;
info.LastWriteTime.HighPart=static_cast<LONG>(now.dwHighDateTime);
info.ChangeTime=info.LastWriteTime;
// 0 leaves a time as it is.
info.CreationTime.QuadPart=0;
info.LastAccessTime.QuadPart=0;
info.FileAttributes&=~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM);
if(info.FileAttributes==0)info.FileAttributes=FILE_ATTRIBUTE_NORMAL;
if(!SetFileInformationByHandle(handle.handle,FileBasicInfo,&info,sizeof info))error=last_error();
}
// The mark of the web says where a download came from; a copy made by the game was not downloaded.
fs::path mark=to;
mark+=L":Zone.Identifier";
const NativePath stream(mark);
if(!DeleteFileW(stream.c_str())){
const DWORD code=GetLastError();
if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND&&!error)error=errno_code(errno_of(code));
}
return error;
}
}

void testing::fail_next_copy_reset() noexcept
{
fail_reset.store(true,std::memory_order_relaxed);
}

void testing::clear_copy_reset_failure() noexcept
{
fail_reset.store(false,std::memory_order_relaxed);
}

std::error_code discard_file(const fs::path& path)
{
const NativePath name(path);
// DeleteFileW refuses a read-only file (CopyFileExW copies the attribute from its source).
const DWORD attributes=GetFileAttributesW(name.c_str());
if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_READONLY))SetFileAttributesW(name.c_str(),attributes&~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
if(!DeleteFileW(name.c_str()))return last_error();
return {};
}

std::error_code sync_directory(const fs::path&)
{
return {};
}

CopyResult copy_file(const fs::path& from,const fs::path& to,const CopyOptions& options)
{
const bool create_new=options.create_new;
CopyResult result;
const NativePath source(from);
const NativePath target(to);
// Both opened (links followed) and compared by identity before anything is written; the handles stay open over the copy. A
// handle without access rights takes no part in sharing, so it blocks neither CopyFileExW nor anyone else.
const OwnedHandle in{CreateFileW(source.c_str(),0,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
if(in.handle==INVALID_HANDLE_VALUE){
result.error=last_error();
result.from_failed=true;
return result;
}
BY_HANDLE_FILE_INFORMATION about;
if(!GetFileInformationByHandle(in.handle,&about)){
result.error=last_error();
result.from_failed=true;
return result;
}
if(about.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
result.error=errno_code(EISDIR);
result.from_failed=true;
return result;
}
const auto from_identity=identity_of(in.handle);
if(!from_identity){
result.error=from_identity.error();
result.from_failed=true;
return result;
}
// A new file needs no comparison: CopyFileExW makes it itself (COPY_FILE_FAIL_IF_EXISTS), so it cannot be from.
const OwnedHandle out{create_new?INVALID_HANDLE_VALUE:CreateFileW(target.c_str(),0,share_all,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
if(out.handle!=INVALID_HANDLE_VALUE){
const auto to_identity=identity_of(out.handle);
if(!to_identity){
result.error=to_identity.error();
return result;
}
if(*to_identity==*from_identity){
result.same=true;
return result;
}
}
else if(const std::error_code error=last_error();!create_new&&error.value()!=ENOENT){
result.error=error;
return result;
}
if(!CopyFileExW(source.c_str(),target.c_str(),nullptr,nullptr,nullptr,create_new?COPY_FILE_FAIL_IF_EXISTS:0)){
result.error=last_error();
// CopyFileExW does not say which side failed: from is to blame when it is gone.
result.from_failed=GetFileAttributesW(source.c_str())==INVALID_FILE_ATTRIBUTES;
return result;
}
result.metadata=reset_copy(to,target);
if(options.sync){
// A read-only attribute the reset could not clear (copied from a read-only source) would refuse the write handle the flush
// needs; the copy is meant not to have it anyway.
if(const DWORD attributes=GetFileAttributesW(target.c_str());attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_READONLY))SetFileAttributesW(target.c_str(),attributes&~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
const OwnedHandle written{CreateFileW(target.c_str(),GENERIC_WRITE,share_all,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
if(written.handle==INVALID_HANDLE_VALUE||!FlushFileBuffers(written.handle))result.error=last_error();
}
return result;
}

fs::path user_data_folder()
{
std::wstring value(MAX_PATH,L'\0');
DWORD got=GetEnvironmentVariableW(L"APPDATA",value.data(),static_cast<DWORD>(value.size()));
if(got>=value.size()){
value.resize(got);
got=GetEnvironmentVariableW(L"APPDATA",value.data(),static_cast<DWORD>(value.size()));
}
if(got>0&&got<value.size()){
value.resize(got);
fs::path folder(std::move(value));
if(folder.is_absolute())return folder.lexically_normal();
}
wchar_t folder[MAX_PATH];
if(SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,folder)))return fs::path(folder).lexically_normal();
return {};
}
}
