// Files and folders on POSIX systems (see platform/file.hpp): open with O_CLOEXEC (and O_NONBLOCK, so a FIFO can never block an open;
// it changes nothing for regular files), pread/pwrite retried on EINTR and short counts, readdir listings that trust d_type and stat
// only links and unknown entries, rename for replace_file, st_dev and st_ino for identities, and copy_file_range (then sendfile, then
// read and write) for copy_file.
#include <platform/file.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sendfile.h>
#endif

namespace zodiac::platform {
namespace {
namespace fs=std::filesystem;
std::error_code errno_code(int value) noexcept
{
return std::error_code(value,std::generic_category());
}
std::int64_t unix_ns(const struct stat& info) noexcept
{
#if defined(__APPLE__)
const struct timespec& time=info.st_mtimespec;
#else
const struct timespec& time=info.st_mtim;
#endif
constexpr std::int64_t limit=INT64_MAX/1000000000;
const auto seconds=static_cast<std::int64_t>(time.tv_sec);
if(seconds>=limit)return INT64_MAX;
if(seconds<= -limit)return INT64_MIN;
return seconds*1000000000+static_cast<std::int64_t>(time.tv_nsec);
}
FileIdentity identity_of(const struct stat& info) noexcept
{
FileIdentity identity;
identity.volume=static_cast<std::uint64_t>(info.st_dev);
identity.low=static_cast<std::uint64_t>(info.st_ino);
return identity;
}
NodeKind kind_of(mode_t mode) noexcept
{
return S_ISREG(mode)?NodeKind::file:S_ISDIR(mode)?NodeKind::directory:NodeKind::other;
}
// Closes a directory stream on every exit.
struct DirectoryStream{
DIR* dir;
~DirectoryStream(){if(dir)::closedir(dir);}
};
}

std::expected<File,std::error_code> File::open(const fs::path& path,FileAccess access)
{
int flags=O_CLOEXEC|O_NONBLOCK;
switch(access){
case FileAccess::read:
flags|=O_RDONLY;
break;
case FileAccess::read_write:
flags|=O_RDWR;
break;
case FileAccess::create_truncate:
flags|=O_RDWR|O_CREAT|O_TRUNC;
break;
case FileAccess::create_or_open:
flags|=O_RDWR|O_CREAT;
break;
case FileAccess::create_new:
case FileAccess::create_private:
flags|=O_RDWR|O_CREAT|O_EXCL;
break;
}
const mode_t mode=access==FileAccess::create_private?0600:0666;
int fd;
do{
fd=::open(path.c_str(),flags,mode);
}while(fd<0&&errno==EINTR);
if(fd<0)return std::unexpected(errno_code(errno));
// A folder opens read-only on POSIX; a device, FIFO or socket is no file either.
struct stat info;
const bool stated=::fstat(fd,&info)==0;
if(!stated||!S_ISREG(info.st_mode)){
const int error=!stated?errno:S_ISDIR(info.st_mode)?EISDIR:EINVAL;
::close(fd);
return std::unexpected(errno_code(error));
}
return File(fd);
}

std::int64_t File::read_at(std::uint64_t offset,std::span<std::byte> buffer) const noexcept
{
std::size_t done=0;
while(done<buffer.size()){
const std::size_t chunk=std::min<std::size_t>(buffer.size()-done,0x40000000u);
const ssize_t got=::pread(static_cast<int>(handle_),buffer.data()+done,chunk,static_cast<off_t>(offset+done));
if(got<0){
if(errno==EINTR)continue;
if(done>0)break;
return -static_cast<std::int64_t>(errno);
}
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
return static_cast<std::int64_t>(done);
}

std::expected<void,std::error_code> File::write_at(std::uint64_t offset,std::span<const std::byte> bytes) noexcept
{
std::size_t done=0;
while(done<bytes.size()){
const std::size_t chunk=std::min<std::size_t>(bytes.size()-done,0x40000000u);
const ssize_t put=::pwrite(static_cast<int>(handle_),bytes.data()+done,chunk,static_cast<off_t>(offset+done));
if(put<0&&errno==EINTR)continue;
if(put<0)return std::unexpected(errno_code(errno));
if(put==0)return std::unexpected(errno_code(EIO));
done+=static_cast<std::size_t>(put);
}
return {};
}

std::uint64_t File::size() const noexcept
{
struct stat info;
return ::fstat(static_cast<int>(handle_),&info)==0?static_cast<std::uint64_t>(info.st_size):0;
}

std::expected<void,std::error_code> File::truncate(std::uint64_t size) noexcept
{
if(size>static_cast<std::uint64_t>(INT64_MAX))return std::unexpected(errno_code(EFBIG));
int result;
do{
result=::ftruncate(static_cast<int>(handle_),static_cast<off_t>(size));
}while(result!=0&&errno==EINTR);
if(result!=0)return std::unexpected(errno_code(errno));
return {};
}

std::expected<FileIdentity,std::error_code> File::identity() const noexcept
{
struct stat info;
if(::fstat(static_cast<int>(handle_),&info)!=0)return std::unexpected(errno_code(errno));
return identity_of(info);
}

bool File::sync() noexcept
{
return ::fsync(static_cast<int>(handle_))==0;
}

namespace {
int sync_descriptor(int fd) noexcept
{
int result;
do{
#ifdef __APPLE__
result=::fsync(fd);
#else
result=::fdatasync(fd);
#endif
}while(result!=0&&errno==EINTR);
return result==0?0:errno;
}
int copy_mode_of(int fd,const fs::path& model) noexcept
{
struct stat info;
if(::stat(model.c_str(),&info)!=0)return errno==ENOENT?0:errno;
int result;
do{
result=::fchmod(fd,info.st_mode&07777);
}while(result!=0&&errno==EINTR);
return result==0?0:errno;
}
}

std::expected<void,std::error_code> File::sync_data() noexcept
{
if(const int error=sync_descriptor(static_cast<int>(handle_)))return std::unexpected(errno_code(error));
return {};
}

std::error_code File::copy_mode(const fs::path& model) noexcept
{
const int error=copy_mode_of(static_cast<int>(handle_),model);
return error?errno_code(error):std::error_code();
}

void File::close() noexcept
{
if(handle_==invalid_handle)return;
::close(static_cast<int>(handle_));
handle_=invalid_handle;
}

std::expected<NodeInfo,std::error_code> stat_path(const fs::path& path)
{
struct stat info;
if(::stat(path.c_str(),&info)!=0)return std::unexpected(errno_code(errno));
NodeInfo node;
node.kind=kind_of(info.st_mode);
node.size=node.kind==NodeKind::file?static_cast<std::uint64_t>(info.st_size):0;
node.modified_utc_ns=unix_ns(info);
return node;
}

bool is_link(const fs::path& path)
{
struct stat info;
return ::lstat(path.c_str(),&info)==0&&S_ISLNK(info.st_mode);
}

std::error_code list_directory(const fs::path& path,std::vector<DirectoryEntry>& out)
{
const DirectoryStream stream{::opendir(path.c_str())};
if(!stream.dir)return errno_code(errno);
const int dir_fd=::dirfd(stream.dir);
for(;;){
errno=0;
const dirent* entry=::readdir(stream.dir);
if(!entry){
if(errno!=0)return errno_code(errno);
return {};
}
const char* name=entry->d_name;
if(name[0]=='.'&&(name[1]=='\0'||(name[1]=='.'&&name[2]=='\0')))continue;
DirectoryEntry item;
item.name.assign(name);
switch(entry->d_type){
case DT_REG:
item.kind=NodeKind::file;
break;
case DT_DIR:
item.kind=NodeKind::directory;
break;
case DT_LNK:
case DT_UNKNOWN:{
struct stat info;
if(entry->d_type==DT_UNKNOWN){
if(::fstatat(dir_fd,name,&info,AT_SYMLINK_NOFOLLOW)!=0)continue;
item.link=S_ISLNK(info.st_mode);
if(!item.link){
item.kind=kind_of(info.st_mode);
break;
}
}
else item.link=true;
// A link that leads nowhere keeps kind none: it is still an entry, which removing its folder must remove.
if(::fstatat(dir_fd,name,&info,0)==0)item.kind=kind_of(info.st_mode);
break;
}
default:
item.kind=NodeKind::other;
break;
}
out.push_back(std::move(item));
}
}

std::error_code make_directory(const fs::path& path)
{
return ::mkdir(path.c_str(),0777)==0?std::error_code():errno_code(errno);
}

std::error_code remove_file(const fs::path& path)
{
return ::unlink(path.c_str())==0?std::error_code():errno_code(errno);
}

std::error_code remove_directory(const fs::path& path)
{
if(::rmdir(path.c_str())==0)return {};
// POSIX allows EEXIST for a folder that is not empty.
const int error=errno;
return errno_code(error==EEXIST?ENOTEMPTY:error);
}

ErrorKind error_kind(const std::error_code& code) noexcept
{
const std::error_condition condition=code.default_error_condition();
if(condition.category()!=std::generic_category())return ErrorKind::other;
switch(condition.value()){
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
if(std::rename(from.c_str(),to.c_str())==0)return true;
error=errno_code(errno);
return false;
}

std::expected<FileIdentity,std::error_code> file_identity(const fs::path& path)
{
struct stat info;
if(::stat(path.c_str(),&info)!=0)return std::unexpected(errno_code(errno));
return identity_of(info);
}

namespace {
// Closes a descriptor on every exit; close_checked reports a failed close of a written file.
struct Descriptor{
int fd=-1;
~Descriptor(){if(fd>=0)::close(fd);}
int close_checked() noexcept
{
const int result=::close(fd);
fd=-1;
return result==0||errno==EINTR?0:errno;
}
};
int open_retry(const fs::path& path,int flags,mode_t mode=0666) noexcept
{
int fd;
do{
fd=::open(path.c_str(),flags,mode);
}while(fd<0&&errno==EINTR);
return fd;
}
// The kernel's copy declines (no such call, another file system, a file system without it): the next way down is tried.
bool declined(int error) noexcept
{
#if ENOTSUP!=EOPNOTSUPP
if(error==ENOTSUP)return true;
#endif
return error==ENOSYS||error==EXDEV||error==EINVAL||error==EOPNOTSUPP||error==EPERM;
}
// Copies size bytes from in to out, both at their file offsets (0): copy_file_range, then sendfile, then read and write.
int copy_bytes(int in,int out,std::uint64_t size,bool& from_failed) noexcept
{
std::uint64_t done=0;
#ifdef __linux__
bool range=true;
bool send=true;
#endif
char buffer[65536];
while(done<size){
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(size-done,0x40000000u));
ssize_t moved;
#ifdef __linux__
if(range){
moved=::copy_file_range(in,nullptr,out,nullptr,wanted,0);
// 0 before the end: some kernels and file systems (procfs-like files, older overlay and network file systems) stop there
// without an error, so only the read and write loop below may decide that the file has ended.
if((moved<0&&errno!=EINTR&&declined(errno))||moved==0){
range=false;
continue;
}
}
else if(send){
moved=::sendfile(out,in,nullptr,wanted);
if((moved<0&&errno!=EINTR&&declined(errno))||moved==0){
send=false;
continue;
}
}
else
#endif
{
moved=::read(in,buffer,std::min(wanted,sizeof buffer));
if(moved<0&&errno!=EINTR){
from_failed=true;
return errno;
}
if(moved>0){
for(ssize_t written=0;written<moved;){
const ssize_t put=::write(out,buffer+written,static_cast<std::size_t>(moved-written));
if(put<0&&errno==EINTR)continue;
if(put<0)return errno;
if(put==0)return EIO;
written+=put;
}
}
}
if(moved<0){
if(errno==EINTR)continue;
return errno;
}
// Only read gets here with 0: the file got shorter since it was measured.
if(moved==0)break;
done+=static_cast<std::uint64_t>(moved);
}
return 0;
}
}

std::error_code discard_file(const fs::path& path)
{
return remove_file(path);
}

std::error_code sync_directory(const fs::path& path)
{
Descriptor folder{open_retry(path,O_RDONLY|O_CLOEXEC|O_DIRECTORY)};
if(folder.fd<0)return errno_code(errno);
int result;
do{
result=::fsync(folder.fd);
}while(result!=0&&errno==EINTR);
// Some file systems cannot sync a folder; the rename itself is then as durable as they make it.
if(result!=0&&errno!=EINVAL&&errno!=EROFS)return errno_code(errno);
return {};
}

CopyResult copy_file(const fs::path& from,const fs::path& to,const CopyOptions& options)
{
const bool create_new=options.create_new;
CopyResult result;
const auto fail=[&result](int error){
result.error=errno_code(error);
return result;
};
result.from_failed=true;
Descriptor in{open_retry(from,O_RDONLY|O_CLOEXEC|O_NONBLOCK)};
if(in.fd<0)return fail(errno);
struct stat info;
if(::fstat(in.fd,&info)!=0)return fail(errno);
if(!S_ISREG(info.st_mode))return fail(S_ISDIR(info.st_mode)?EISDIR:EINVAL);
result.from_failed=false;
// Opened without O_TRUNC and compared by the descriptors' identities first, so no path change in between can make the copy empty
// its own source.
// A new file that will take another's mode starts private (0600), so no one can open it in a wider mode meanwhile.
const bool private_file=create_new&&options.mode_of&&::access(options.mode_of->c_str(),F_OK)==0;
Descriptor out{open_retry(to,O_WRONLY|O_CREAT|O_CLOEXEC|O_NONBLOCK|(create_new?O_EXCL:0),private_file?0600:0666)};
if(out.fd<0)return fail(errno);
if(!create_new){
struct stat target;
if(::fstat(out.fd,&target)!=0)return fail(errno);
if(target.st_dev==info.st_dev&&target.st_ino==info.st_ino){
result.same=true;
return result;
}
if(!S_ISREG(target.st_mode))return fail(EINVAL);
int cut;
do{
cut=::ftruncate(out.fd,0);
}while(cut!=0&&errno==EINTR);
if(cut!=0)return fail(errno);
}
if(const int error=copy_bytes(in.fd,out.fd,static_cast<std::uint64_t>(info.st_size),result.from_failed))return fail(error);
if(options.mode_of)if(const int error=copy_mode_of(out.fd,*options.mode_of))return fail(error);
if(options.sync)if(const int error=sync_descriptor(out.fd))return fail(error);
if(const int error=out.close_checked())return fail(error);
return result;
}

fs::path user_data_folder()
{
// The XDG Base Directory rules: a relative $XDG_DATA_HOME is invalid and ignored.
if(const char* data=std::getenv("XDG_DATA_HOME");data&&data[0]=='/')return fs::path(data).lexically_normal();
if(const char* home=std::getenv("HOME");home&&home[0]!='\0')return (fs::path(home)/".local"/"share").lexically_normal();
return {};
}
}
