#include <core/vfs/vfs.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/host_source.hpp>
#include <platform/clock.hpp>
#include <platform/entropy.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <span>

namespace zodiac::vfs {
namespace {
bool fall_through(FileError error) noexcept
{
return error==FileError::not_found||error==FileError::not_a_directory;
}
std::size_t segment_count(std::string_view relative) noexcept
{
return relative.empty()?0:static_cast<std::size_t>(std::ranges::count(relative,'/'))+1;
}
// The end of the first count segments of relative (count at least 1), or npos when it has fewer.
std::size_t prefix_end(std::string_view relative,std::size_t count) noexcept
{
std::size_t at=0;
for(;;){
const std::size_t cut=relative.find('/',at);
if(--count==0)return cut==std::string_view::npos?relative.size():cut;
if(cut==std::string_view::npos)return std::string_view::npos;
at=cut+1;
}
}
std::string quote_path(std::string_view path)
{
std::string text;
text.reserve(path.size()+2);
text.push_back('"');
text.append(one_line(path)).push_back('"');
return text;
}
std::unexpected<Failure> failed(FileError code)
{
return std::unexpected(Failure{code,{}});
}
// Runs one file operation; anything it throws (an allocation failure, a logic error) becomes io, so no C++ exception reaches a script
// binding or a plugin call, and dev mode logs what it was. The Failure built here allocates nothing.
template<class Body>
auto guarded(const Vfs& vfs,std::string_view operation,std::string_view path,Body&& body) noexcept->decltype(body())
{
try{
return body();
}
catch(...){
vfs.report_internal_error(operation,path);
return failed(FileError::io);
}
}
std::unexpected<MoveFailure> move_failed(Failure failure,bool to)
{
return std::unexpected(MoveFailure{std::move(failure),to});
}
// guarded for the two-path operations.
template<class Body>
auto guarded_move(const Vfs& vfs,std::string_view operation,std::string_view path,Body&& body) noexcept->decltype(body())
{
try{
return body();
}
catch(...){
vfs.report_internal_error(operation,path);
return std::unexpected(MoveFailure{Failure{FileError::io,{}},false});
}
}
// Whether host files from_file and to_file (of the script's paths from and to) are one file (two mounts of one folder, a host
// path and a mount, a link to the other; links are followed). A missing to is another file; an identity that cannot be read
// otherwise is a failure of its side with its whole text (`cannot read` from, `cannot write` to), never a guess, so nothing is
// renamed over, replaced or removed on an unknown identity.
std::expected<bool,MoveFailure> same_file(std::string_view from,std::string_view to,const std::filesystem::path& from_file,const std::filesystem::path& to_file)
{
const auto first=platform::file_identity(from_file);
if(!first){
const FileError code=detail::file_error(first.error());
return move_failed(Failure{code,failure_text(FileAction::read,from,Failure{code,{}})},false);
}
const auto second=platform::file_identity(to_file);
if(!second){
if(platform::error_kind(second.error())==platform::ErrorKind::not_found)return false;
const FileError code=detail::file_error(second.error());
return move_failed(Failure{code,failure_text(FileAction::write,to,Failure{code,{}})},true);
}
return *first==*second;
}
// A free temporary name beside path: temp_prefix and 16 random hexadecimal digits (an invalid name to every script, listing and
// lookup, see valid_name).
std::filesystem::path temp_beside(const std::filesystem::path& path)
{
static std::atomic<std::uint64_t> fallback{0};
std::uint64_t bits=0;
if(!platform::fill_entropy(&bits,sizeof bits))bits=fallback.fetch_add(0x9E3779B97F4A7C15u,std::memory_order_relaxed)^static_cast<std::uint64_t>(platform::monotonic_us());
std::string name(temp_prefix);
constexpr char digits[]="0123456789abcdef";
for(int shift=60;shift>=0;shift-=4)name.push_back(digits[(bits>>shift)&0xF]);
name.append(".tmp");
return path.parent_path()/std::filesystem::path(name);
}
// The file a replacement takes the place of: what a link at target names (so a write through a link changes that file, as a
// write in place does), or target itself (a dangling link included).
std::expected<std::filesystem::path,std::error_code> replacement_target(const std::filesystem::path& target)
{
if(!platform::is_link(target))return target;
std::error_code error;
std::filesystem::path real=std::filesystem::canonical(target,error);
if(!error)return real;
if(platform::error_kind(error)==platform::ErrorKind::not_found)return target;
return std::unexpected(error);
}
// What writing the temporary file gave: a system error (EEXIST: the name is taken, another is tried; from: it was from that
// failed), or a failure of its own (a read of from), and Windows' metadata warning.
struct TempWrite{
std::error_code system;
bool from=false;
std::optional<MoveFailure> failure;
std::error_code metadata;
};
// Replaces host file target with a new file that write(temp) makes beside it (with FileAccess::create_new or copy_file's
// create_new), by replace_file: the target is the old file or the complete new one, never a part. The temporary file is removed
// after any failure. Its permission bits are target's (POSIX). Not synced: the result survives a crash of the game, not
// necessarily one of the system.
template<class Write>
std::expected<void,MoveFailure> replace_host(const std::filesystem::path& target,Write&& write,std::error_code& metadata)
{
const auto final=replacement_target(target);
if(!final)return move_failed(Failure{detail::file_error(final.error()),{}},true);
for(int attempt=0;;++attempt){
const std::filesystem::path temp=temp_beside(*final);
TempWrite done=write(temp);
const bool taken=done.system&&platform::error_kind(done.system)==platform::ErrorKind::exists;
if(taken&&attempt<8)continue;
if(done.failure||done.system){
// A name another writer holds is not ours to remove.
if(!taken)platform::remove_file(temp);
if(done.failure)return std::unexpected(std::move(*done.failure));
return move_failed(Failure{detail::file_error(done.system),{}},!done.from);
}
metadata=done.metadata;
platform::keep_permissions(*final,temp);
std::error_code error;
if(!platform::replace_file(temp,*final,error)){
platform::remove_file(temp);
return move_failed(Failure{detail::file_error(error),{}},true);
}
return {};
}
}
// Writes bytes into a new file temp.
TempWrite write_bytes(const std::filesystem::path& temp,std::span<const std::byte> bytes)
{
TempWrite done;
auto file=platform::File::open(temp,platform::FileAccess::create_new);
if(!file){
done.system=file.error();
return done;
}
if(auto written=file->write_at(0,bytes);!written)done.system=written.error();
return done;
}
// Copies reader's bytes into a new file temp.
TempWrite write_reader(const std::filesystem::path& temp,Reader& reader,std::byte* buffer,std::size_t chunk)
{
TempWrite done;
auto file=platform::File::open(temp,platform::FileAccess::create_new);
if(!file){
done.system=file.error();
return done;
}
const std::uint64_t size=reader.size();
for(std::uint64_t at=0;at<size;){
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(chunk,size-at));
const std::int64_t got=reader.read_at(at,{buffer,wanted});
if(got<0){
done.failure=MoveFailure{Failure{read_error(got),{}},false};
return done;
}
if(got==0)break;
if(auto written=file->write_at(at,{buffer,static_cast<std::size_t>(got)});!written){
done.system=written.error();
return done;
}
at+=static_cast<std::uint64_t>(got);
}
return done;
}
// What a copy through the sources moves per read and write, through one buffer per thread made by its first such copy.
constexpr std::size_t copy_chunk=262144;
std::byte* copy_buffer()
{
thread_local std::unique_ptr<std::byte[]> buffer;
if(!buffer)buffer=std::make_unique_for_overwrite<std::byte[]>(copy_chunk);
return buffer.get();
}
std::string read_only_text(std::string_view path)
{
return quote_path(path)+" is in a read-only pack; save files in data:/ instead";
}
// FNV-1a over the mount, a separator no UTF-8 text holds, and the relative path, then a final mix; never 0 (an empty slot).
std::uint64_t path_hash(std::string_view mount,std::string_view relative) noexcept
{
std::uint64_t hash=0xCBF29CE484222325u;
const auto add=[&hash](unsigned char byte){
hash^=byte;
hash*=0x100000001B3u;
};
for(const char c:mount)add(static_cast<unsigned char>(c));
add(0xFF);
for(const char c:relative)add(static_cast<unsigned char>(c));
hash^=hash>>33;
hash*=0xFF51AFD7ED558CCDu;
hash^=hash>>33;
return hash?hash:1;
}
}

std::string_view reason_text(FileError error) noexcept
{
switch(error){
case FileError::not_found:
return "file not found";
case FileError::access_denied:
return "access denied";
case FileError::damaged:
return "damaged data";
case FileError::is_directory:
return "is a directory";
case FileError::not_a_directory:
return "not a directory";
case FileError::read_only_pack:
return "in a read-only pack";
case FileError::bad_path:
return "invalid path";
case FileError::ambiguous:
return "ambiguous name";
case FileError::not_empty:
return "directory not empty";
case FileError::disk_full:
return "disk full";
default:
return "input/output error";
}
}

FileError read_error(std::int64_t result) noexcept
{
const std::int64_t code=-result;
return code>=1&&code<=static_cast<std::int64_t>(FileError::disk_full)?static_cast<FileError>(code):FileError::io;
}

std::string failure_text(FileAction action,std::string_view path,const Failure& failure)
{
if(!failure.message.empty())return failure.message;
const std::string_view verb=action==FileAction::read?"cannot read ":action==FileAction::write?"cannot write ":action==FileAction::remove?"cannot delete ":"cannot create ";
const std::string_view reason=reason_text(failure.code);
std::string text;
text.reserve(verb.size()+path.size()+reason.size()+4);
text.append(verb).append(quote_path(path)).append(": ").append(reason);
return text;
}

bool Source::real_spelling(std::string_view,std::string&)
{
return false;
}

std::optional<std::pair<std::string,std::string>> Source::ambiguous_names(std::string_view)
{
return std::nullopt;
}

std::expected<bool,MoveError> Source::rename(std::string_view,std::string_view)
{
return false;
}

std::expected<std::optional<std::filesystem::path>,FileError> Source::host_file(std::string_view,bool)
{
return std::nullopt;
}

void Source::forget(std::string_view)
{
}

Vfs::Vfs():table_(std::make_shared<const Table>()),host_(std::make_shared<HostSource>()),checked_(std::make_unique<std::atomic<std::uint64_t>[]>(checked_slots))
{
}

Vfs::~Vfs()=default;

std::shared_ptr<const Vfs::Table> Vfs::snapshot() const
{
const std::shared_lock lock(table_mutex_);
return table_;
}

template<class Visit>
bool Vfs::visit_covering(const Table& table,const VfsPath& p,Visit&& visit)
{
for(auto it=table.mounts.rbegin();it!=table.mounts.rend();++it){
const Mount& mount=*it;
if(mount.mount!=p.mount)continue;
std::string_view sub=p.relative;
if(mount.at_segments){
const std::size_t end=prefix_end(p.relative,mount.at_segments);
if(end==std::string_view::npos)continue;
const std::string_view prefix=std::string_view(p.relative).substr(0,end);
if(prefix!=mount.at&&fold_name(prefix)!=mount.at_fold)continue;
sub=end==p.relative.size()?std::string_view():std::string_view(p.relative).substr(end+1);
}
if(visit(Target{&mount,sub}))return true;
}
return false;
}

bool Vfs::leads_to_mount(const Table& table,const VfsPath& p)
{
const std::size_t count=segment_count(p.relative);
const std::string fold=count?fold_name(p.relative):std::string();
return std::ranges::any_of(table.mounts,[&](const Mount& mount){
if(mount.mount!=p.mount||mount.at_segments<=count)return false;
if(!count)return true;
const std::string_view prefix=std::string_view(mount.at_fold).substr(0,prefix_end(mount.at_fold,count));
return prefix==fold;
});
}

bool Vfs::mount(std::string_view at,std::shared_ptr<Source> source,bool writable,const void* owner)
{
if(!source)return false;
try{
auto parsed=parse_path(at,false);
if(!parsed)return false;
Mount entry;
entry.mount=std::move(parsed->mount);
entry.at=std::move(parsed->relative);
entry.at_fold=fold_name(entry.at);
entry.at_segments=segment_count(entry.at);
entry.source=std::move(source);
entry.writable=writable;
entry.owner=owner;
const std::lock_guard writer(writer_mutex_);
const std::shared_ptr<const Table> current=snapshot();
if(owner){
const bool taken=std::ranges::any_of(current->mounts,[&entry](const Mount& mount){return mount.owner==entry.owner&&mount.mount==entry.mount&&mount.at_fold==entry.at_fold;});
if(taken)return false;
}
auto next=std::make_shared<Table>();
next->mounts.reserve(current->mounts.size()+1);
next->mounts=current->mounts;
next->mounts.push_back(std::move(entry));
std::shared_ptr<const Table> replaced=std::move(next);
{
const std::unique_lock lock(table_mutex_);
table_.swap(replaced);
}
// The old table (now in replaced) is released here, outside the lock.
return true;
}
catch(...){
return false;
}
}

std::size_t Vfs::unmount(const void* owner)
{
if(!owner)return 0;
// Everything that can throw (the lock, the copy) comes before the swap, so a failure leaves the table as it was.
try{
const std::lock_guard writer(writer_mutex_);
const std::shared_ptr<const Table> current=snapshot();
const std::size_t removed=static_cast<std::size_t>(std::ranges::count_if(current->mounts,[owner](const Mount& mount){return mount.owner==owner;}));
if(!removed)return 0;
auto next=std::make_shared<Table>();
next->mounts.reserve(current->mounts.size()-removed);
for(const Mount& mount:current->mounts)if(mount.owner!=owner)next->mounts.push_back(mount);
std::shared_ptr<const Table> replaced=std::move(next);
{
const std::unique_lock lock(table_mutex_);
table_.swap(replaced);
}
return removed;
}
catch(...){
return 0;
}
}

std::expected<VfsPath,std::string> Vfs::parse(std::string_view path) const noexcept
{
try{
return parse_path(path,host_paths());
}
catch(...){
try{
return std::unexpected(quote_path(path)+" is not a valid file name");
}
catch(...){
return std::unexpected(std::string());
}
}
}

void Vfs::report_internal_error(std::string_view operation,std::string_view path) const noexcept
{
if(!dev_mode()||!log_)return;
// A log handler that throws in turn is swallowed: nothing may leave here.
try{
std::string what="unknown exception";
try{
throw;
}
catch(const std::exception& exception){
what=exception.what();
}
catch(...){
}
std::string line="error: internal error in ";
line.append(operation).append(" of ").append(quote_path(path)).append(": ").append(what);
log_(ZODIAC_LOG_ERROR,line);
}
catch(...){
}
}

bool Vfs::mount_folder(std::string_view at,const std::filesystem::path& host,bool writable,const void* owner)
{
try{
return mount(at,std::make_shared<FolderSource>(host,!writable),writable,owner);
}
catch(...){
return false;
}
}

std::expected<VfsPath,Failure> Vfs::parse_failure(std::string_view path) const
{
auto parsed=parse_path(path,host_paths());
if(!parsed)return std::unexpected(Failure{FileError::bad_path,std::move(parsed.error())});
return std::move(*parsed);
}

Failure Vfs::ambiguous_failure(std::string_view path,const Target& target) const
{
Failure failure{FileError::ambiguous,quote_path(path)};
const auto names=target.mount->source->ambiguous_names(target.sub);
if(!names){
failure.message.append(" matches more than one name; rename one of them");
return failure;
}
std::string prefix=target.mount->mount=="data:"?"data:/":"";
if(!target.mount->at.empty())prefix.append(target.mount->at).push_back('/');
failure.message.append(" matches both ").append(quote_path(prefix+names->first)).append(" and ").append(quote_path(prefix+names->second)).append("; rename one of them");
return failure;
}

void Vfs::warn(std::string_view line) const
{
if(log_)log_(ZODIAC_LOG_WARNING,line);
}

bool Vfs::checked(std::uint64_t hash) const noexcept
{
for(std::size_t i=0;i<checked_probes;++i){
const std::uint64_t slot=checked_[(hash+i)&(checked_slots-1)].load(std::memory_order_relaxed);
if(slot==hash)return true;
if(slot==0)return false;
}
return false;
}

void Vfs::remember_checked(std::uint64_t hash) noexcept
{
for(std::size_t i=0;i<checked_probes;++i){
std::atomic<std::uint64_t>& slot=checked_[(hash+i)&(checked_slots-1)];
std::uint64_t seen=slot.load(std::memory_order_relaxed);
if(seen==hash)return;
if(seen==0&&(slot.compare_exchange_strong(seen,hash,std::memory_order_relaxed)||seen==hash))return;
}
// The run is full: the newest path takes its first slot.
checked_[hash&(checked_slots-1)].store(hash,std::memory_order_relaxed);
}

void Vfs::check_spelling(std::string_view path,const VfsPath& p,const Target& target) noexcept
{
if(!dev_mode()||target.sub.empty())return;
try{
const std::uint64_t hash=path_hash(p.mount,p.relative);
if(checked(hash))return;
std::string real;
const bool differs=target.mount->source->real_spelling(target.sub,real);
remember_checked(hash);
if(!differs)return;
{
std::string key;
key.reserve(p.mount.size()+p.relative.size());
key.append(p.mount).append(p.relative);
const std::lock_guard lock(warned_mutex_);
if(spelling_warned_.contains(key))return;
if(spelling_warned_.size()<max_warned)spelling_warned_.insert(std::move(key));
}
std::string shown=target.mount->mount=="data:"?"data:/":"";
if(!target.mount->at.empty())shown.append(target.mount->at).push_back('/');
shown.append(real);
std::string line="warning: ";
line.append(quote_path(path)).append(" was found as ").append(quote_path(shown)).append("; use the real name, other systems may not find it");
warn(line);
}
catch(...){
// Advisory only: the lookup it follows has already succeeded and stays a success.
report_internal_error("the spelling check",path);
}
}

void Vfs::check_release_pack(std::string_view path,const VfsPath& p) noexcept
{
if(!dev_mode()||!release_pack_check_||!p.mount.empty())return;
try{
if(!release_pack_check_(p.relative))return;
{
const std::lock_guard lock(warned_mutex_);
if(pack_warned_.contains(p.relative))return;
if(pack_warned_.size()<max_warned)pack_warned_.insert(p.relative);
}
std::string line="warning: ";
line.append(quote_path(path)).append(" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead");
warn(line);
}
catch(...){
// Advisory only: the file is already open for writing, and that result stands.
report_internal_error("the release pack check",path);
}
}

std::expected<EntryInfo,Failure> Vfs::stat(std::string_view path)
{
return guarded(*this,"stat",path,[&]()->std::expected<EntryInfo,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->mount=="host:"){
auto info=host_->stat(p->relative);
if(!info)return failed(info.error());
if(p->folder&&info->kind!=EntryKind::directory)return failed(FileError::not_a_directory);
return std::move(*info);
}
const std::shared_ptr<const Table> table=snapshot();
std::expected<EntryInfo,Failure> result=failed(FileError::not_found);
Target found;
const bool stopped=visit_covering(*table,*p,[&](const Target& target){
auto info=target.mount->source->stat(target.sub);
if(info){
result=std::move(*info);
found=target;
return true;
}
if(fall_through(info.error()))return false;
result=std::unexpected(info.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{info.error(),{}});
return true;
});
if(stopped){
if(result){
if(p->folder&&result->kind!=EntryKind::directory)return failed(FileError::not_a_directory);
check_spelling(path,*p,found);
}
return result;
}
if(leads_to_mount(*table,*p)){
EntryInfo info;
info.kind=EntryKind::directory;
const std::size_t slash=p->relative.rfind('/');
info.name=slash==std::string::npos?p->relative:p->relative.substr(slash+1);
return info;
}
return result;
});
}

std::expected<std::shared_ptr<Reader>,Failure> Vfs::open_read(std::string_view path)
{
return guarded(*this,"open_read",path,[&]()->std::expected<std::shared_ptr<Reader>,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->folder)return failed(FileError::is_directory);
if(p->mount=="host:"){
auto reader=host_->open_read(p->relative);
if(!reader)return failed(reader.error());
return std::move(*reader);
}
const std::shared_ptr<const Table> table=snapshot();
std::expected<std::shared_ptr<Reader>,Failure> result=failed(FileError::not_found);
Target found;
const bool stopped=visit_covering(*table,*p,[&](const Target& target){
auto reader=target.mount->source->open_read(target.sub);
if(reader){
result=std::move(*reader);
found=target;
return true;
}
if(fall_through(reader.error()))return false;
result=std::unexpected(reader.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{reader.error(),{}});
return true;
});
if(stopped){
if(result)check_spelling(path,*p,found);
return result;
}
if(leads_to_mount(*table,*p))return failed(FileError::is_directory);
return result;
});
}

std::expected<std::unique_ptr<WriteTarget>,Failure> Vfs::open_write(std::string_view path,WriteMode mode)
{
return guarded(*this,"open_write",path,[&]()->std::expected<std::unique_ptr<WriteTarget>,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->folder)return failed(FileError::is_directory);
if(p->mount=="host:"){
auto target=host_->open_write(p->relative,mode);
if(!target)return failed(target.error());
return std::move(*target);
}
const std::shared_ptr<const Table> table=snapshot();
std::expected<std::unique_ptr<WriteTarget>,Failure> result=failed(FileError::not_found);
bool covered=false;
Target found;
const bool stopped=visit_covering(*table,*p,[&](const Target& target){
covered=true;
if(!target.mount->writable)return false;
auto writer=target.mount->source->open_write(target.sub,mode);
if(writer){
result=std::move(*writer);
found=target;
}
else result=std::unexpected(writer.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{writer.error(),{}});
return true;
});
if(!stopped){
if(leads_to_mount(*table,*p))return failed(FileError::is_directory);
if(covered)return std::unexpected(Failure{FileError::read_only_pack,read_only_text(path)});
return result;
}
if(result){
check_release_pack(path,*p);
check_spelling(path,*p,found);
}
return result;
});
}

std::expected<void,Failure> Vfs::remove(std::string_view path)
{
return guarded(*this,"remove",path,[&]()->std::expected<void,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->folder)return failed(FileError::is_directory);
if(p->mount=="host:"){
if(auto removed=host_->remove(p->relative);!removed)return failed(removed.error());
return {};
}
const std::shared_ptr<const Table> table=snapshot();
std::expected<void,Failure> result=failed(FileError::not_found);
const bool stopped=visit_covering(*table,*p,[&](const Target& target){
Source& source=*target.mount->source;
if(!target.mount->writable){
const auto info=source.stat(target.sub);
if(info&&info->kind!=EntryKind::deleted){
result=std::unexpected(Failure{FileError::read_only_pack,read_only_text(path)});
return true;
}
if(info||fall_through(info.error()))return false;
result=std::unexpected(info.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{info.error(),{}});
return true;
}
// The spelling is checked before the entry is gone (dev mode only).
if(dev_mode()&&source.stat(target.sub))check_spelling(path,*p,target);
auto removed=source.remove(target.sub);
if(removed){
result={};
return true;
}
if(fall_through(removed.error()))return false;
result=std::unexpected(removed.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{removed.error(),{}});
return true;
});
if(!stopped&&leads_to_mount(*table,*p))return failed(FileError::is_directory);
return result;
});
}

std::expected<Vfs::End,Failure> Vfs::reading_end(const Table& table,std::string_view path,const VfsPath& p,bool removing) const
{
if(p.mount=="host:")return End{host_.get(),nullptr,p.relative};
std::optional<Failure> failure;
End found;
visit_covering(table,p,[&](const Target& target){
const auto info=target.mount->source->stat(target.sub);
if(!info){
if(fall_through(info.error()))return false;
failure=info.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{info.error(),{}};
return true;
}
if(info->kind==EntryKind::deleted)return false;
if(info->kind==EntryKind::directory)failure=Failure{FileError::is_directory,{}};
else if(removing&&!target.mount->writable)failure=Failure{FileError::read_only_pack,read_only_text(path)};
else found=End{target.mount->source.get(),target.mount,target.sub};
return true;
});
if(failure)return std::unexpected(std::move(*failure));
if(!found.source)return failed(leads_to_mount(table,p)?FileError::is_directory:FileError::not_found);
return found;
}

std::expected<Vfs::End,Failure> Vfs::writing_end(const Table& table,std::string_view path,const VfsPath& p) const
{
if(p.mount=="host:")return End{host_.get(),nullptr,p.relative};
bool covered=false;
End found;
const bool writable=visit_covering(table,p,[&](const Target& target){
covered=true;
if(!target.mount->writable)return false;
found=End{target.mount->source.get(),target.mount,target.sub};
return true;
});
if(writable)return found;
if(leads_to_mount(table,p))return failed(FileError::is_directory);
if(covered)return std::unexpected(Failure{FileError::read_only_pack,read_only_text(path)});
return failed(FileError::not_found);
}

Failure Vfs::end_failure(std::string_view path,const End& end,FileError code) const
{
if(code==FileError::ambiguous&&end.mount)return ambiguous_failure(path,Target{end.mount,end.sub});
return Failure{code,{}};
}

void Vfs::warn_copy_metadata(std::string_view path,const std::error_code& error) const noexcept
{
if(!dev_mode())return;
try{
std::string line="warning: ";
line.append(quote_path(path)).append(" was copied, but the time and attributes it took from its source could not be reset: ").append(reason_text(detail::file_error(error)));
warn(line);
}
catch(...){
report_internal_error("the copy's metadata reset",path);
}
}

void Vfs::written(std::string_view path,const VfsPath& p,const End& end) noexcept
{
check_release_pack(path,p);
if(end.mount)check_spelling(path,p,Target{end.mount,end.sub});
}

std::expected<bool,MoveFailure> Vfs::rename(std::string_view from,std::string_view to)
{
return guarded_move(*this,"rename",from,[&]()->std::expected<bool,MoveFailure>{
const auto old_path=parse_failure(from);
if(!old_path)return move_failed(old_path.error(),false);
const auto new_path=parse_failure(to);
if(!new_path)return move_failed(new_path.error(),true);
if(old_path->folder)return move_failed(Failure{FileError::is_directory,{}},false);
if(new_path->folder)return move_failed(Failure{FileError::is_directory,{}},true);
const std::shared_ptr<const Table> table=snapshot();
const auto source=reading_end(*table,from,*old_path,true);
if(!source)return move_failed(source.error(),false);
const auto target=writing_end(*table,to,*new_path);
if(!target)return move_failed(target.error(),true);
if(source->mount)check_spelling(from,*old_path,Target{source->mount,source->sub});
// Host files on both sides are compared first, in one source too: one identity is one file (two mounts of one folder, a host
// path and a mount, a link to the other), and renaming it onto itself would at best do nothing and at worst (a link renamed over
// its own target) lose it, so nothing is done.
const auto old_file=source->source->host_file(source->sub,false);
if(!old_file)return move_failed(end_failure(from,*source,old_file.error()),false);
std::optional<std::filesystem::path> new_host;
if(*old_file){
const auto new_file=target->source->host_file(target->sub,true);
if(!new_file)return move_failed(end_failure(to,*target,new_file.error()),true);
if(*new_file){
const auto same=same_file(from,to,**old_file,**new_file);
if(!same)return std::unexpected(same.error());
if(*same)return true;
new_host=std::move(*new_file);
}
}
if(source->source==target->source){
const auto renamed=source->source->rename(source->sub,target->sub);
if(!renamed){
const bool on_to=renamed.error().to;
return move_failed(end_failure(on_to?to:from,on_to?*target:*source,renamed.error().code),on_to);
}
if(*renamed)written(to,*new_path,*target);
return *renamed;
}
// Two sources: the system renames when both hand out host files (known by now to be two files).
if(!*old_file||!new_host)return false;
std::error_code error;
if(!platform::replace_file(**old_file,*new_host,error)){
if(platform::error_kind(error)==platform::ErrorKind::cross_device)return false;
return move_failed(Failure{detail::file_error(error),{}},platform::stat_path(**old_file).has_value());
}
source->source->forget(source->sub);
target->source->forget(target->sub);
written(to,*new_path,*target);
return true;
});
}

std::expected<bool,MoveFailure> Vfs::copy_file(std::string_view from,std::string_view to)
{
const auto old_path=parse_failure(from);
if(!old_path)return move_failed(old_path.error(),false);
const auto new_path=parse_failure(to);
if(!new_path)return move_failed(new_path.error(),true);
if(old_path->folder)return move_failed(Failure{FileError::is_directory,{}},false);
if(new_path->folder)return move_failed(Failure{FileError::is_directory,{}},true);
{
const std::shared_ptr<const Table> table=snapshot();
const auto source=reading_end(*table,from,*old_path,false);
if(!source)return move_failed(source.error(),false);
const auto target=writing_end(*table,to,*new_path);
if(!target)return move_failed(target.error(),true);
const auto old_file=source->source->host_file(source->sub,false);
if(!old_file)return move_failed(end_failure(from,*source,old_file.error()),false);
const auto new_file=target->source->host_file(target->sub,true);
if(!new_file)return move_failed(end_failure(to,*target,new_file.error()),true);
if(*new_file){
// A host file target is replaced by a complete new file: never left half overwritten.
if(*old_file){
const auto same=same_file(from,to,**old_file,**new_file);
if(!same)return std::unexpected(same.error());
if(*same)return true;
}
std::error_code metadata;
std::expected<void,MoveFailure> replaced;
if(*old_file){
// The system's copy makes the temporary file itself (O_EXCL, COPY_FILE_FAIL_IF_EXISTS), so it is never handed an existing file.
replaced=replace_host(**new_file,[&](const std::filesystem::path& temp){
const platform::CopyResult copied=platform::copy_file(**old_file,temp,true);
return TempWrite{copied.error,copied.from_failed,std::nullopt,copied.metadata};
},metadata);
}
else{
const auto reader=open_read(from);
if(!reader)return move_failed(reader.error(),false);
std::byte* const buffer=copy_buffer();
replaced=replace_host(**new_file,[&](const std::filesystem::path& temp){return write_reader(temp,**reader,buffer,copy_chunk);},metadata);
}
target->source->forget(target->sub);
if(!replaced)return std::unexpected(replaced.error());
if(metadata)warn_copy_metadata(to,metadata);
written(to,*new_path,*target);
return false;
}
}
// A target without host files: a write target's bytes take the entry's place only at commit (Source's contract), so a failure
// leaves the old entry whole.
const auto reader=open_read(from);
if(!reader)return move_failed(reader.error(),false);
auto writer=open_write(to,WriteMode::truncate);
if(!writer)return move_failed(writer.error(),true);
Reader& in=**reader;
WriteTarget& out=**writer;
const std::uint64_t size=in.size();
std::byte* const buffer=size?copy_buffer():nullptr;
std::uint64_t done=0;
while(done<size){
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(copy_chunk,size-done));
const std::int64_t got=in.read_at(done,{buffer,wanted});
if(got<0)return move_failed(Failure{read_error(got),{}},false);
if(got==0)break;
if(auto put=out.write_at(done,{buffer,static_cast<std::size_t>(got)});!put)return move_failed(Failure{put.error(),{}},true);
done+=static_cast<std::uint64_t>(got);
}
if(auto committed=out.commit();!committed)return move_failed(Failure{committed.error(),{}},true);
return false;
}

std::expected<void,Failure> Vfs::write_file(std::string_view path,std::span<const std::byte> bytes)
{
return guarded(*this,"write_file",path,[&]()->std::expected<void,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->folder)return failed(FileError::is_directory);
{
const std::shared_ptr<const Table> table=snapshot();
const auto target=writing_end(*table,path,*p);
if(!target)return std::unexpected(target.error());
const auto file=target->source->host_file(target->sub,true);
if(!file)return std::unexpected(end_failure(path,*target,file.error()));
if(*file){
std::error_code unused;
const auto replaced=replace_host(**file,[&](const std::filesystem::path& temp){return write_bytes(temp,bytes);},unused);
target->source->forget(target->sub);
if(!replaced)return std::unexpected(replaced.error().failure);
written(path,*p,*target);
return {};
}
}
// A target without host files takes the bytes at commit (Source's contract).
auto writer=open_write(path,WriteMode::truncate);
if(!writer)return std::unexpected(writer.error());
if(!bytes.empty())if(auto put=(*writer)->write_at(0,bytes);!put)return failed(put.error());
if(auto committed=(*writer)->commit();!committed)return failed(committed.error());
return {};
});
}

std::expected<void,MoveFailure> Vfs::copy(std::string_view from,std::string_view to)
{
return guarded_move(*this,"copy",from,[&]()->std::expected<void,MoveFailure>{
const auto copied=copy_file(from,to);
if(!copied)return std::unexpected(copied.error());
return {};
});
}

std::expected<void,MoveFailure> Vfs::move(std::string_view from,std::string_view to)
{
return guarded_move(*this,"move",from,[&]()->std::expected<void,MoveFailure>{
const auto renamed=rename(from,to);
if(!renamed)return std::unexpected(renamed.error());
if(*renamed)return {};
const auto copied=copy_file(from,to);
if(!copied)return std::unexpected(copied.error());
// One file on both sides: removing from would remove it.
if(*copied)return {};
if(auto removed=remove(from);!removed)return std::unexpected(MoveFailure{std::move(removed.error()),false,true});
return {};
});
}

std::expected<std::vector<std::string>,Failure> Vfs::list(std::string_view path,ListKind kind,std::string_view pattern)
{
return guarded(*this,"list",path,[&]()->std::expected<std::vector<std::string>,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
std::vector<EntryInfo> entries;
bool found_any=false;
// A mount that has a file where the folder should be: when no mount has the folder, the answer is not_a_directory.
bool met_file=false;
if(p->mount=="host:"){
if(auto listed=host_->list(p->relative,entries);!listed)return failed(listed.error());
found_any=true;
}
else{
const std::shared_ptr<const Table> table=snapshot();
std::optional<Failure> failure;
visit_covering(*table,*p,[&](const Target& target){
auto listed=target.mount->source->list(target.sub,entries);
if(listed){
if(!found_any)check_spelling(path,*p,target);
found_any=true;
return false;
}
if(listed.error()==FileError::not_a_directory){
met_file=true;
return false;
}
if(fall_through(listed.error()))return false;
failure=listed.error()==FileError::ambiguous?ambiguous_failure(path,target):Failure{listed.error(),{}};
return true;
});
if(failure)return std::unexpected(std::move(*failure));
// The folders that lead to deeper mount points.
const std::size_t count=segment_count(p->relative);
const std::string fold=count?fold_name(p->relative):std::string();
for(const Mount& mount:table->mounts){
if(mount.mount!=p->mount||mount.at_segments<=count)continue;
if(count&&std::string_view(mount.at_fold).substr(0,prefix_end(mount.at_fold,count))!=fold)continue;
const std::size_t start=count?prefix_end(mount.at,count)+1:0;
const std::size_t end=prefix_end(mount.at,count+1);
EntryInfo& info=entries.emplace_back();
info.kind=EntryKind::directory;
info.name=mount.at.substr(start,end-start);
found_any=true;
}
}
if(!found_any)return failed(met_file?FileError::not_a_directory:FileError::not_found);
const EntryKind wanted=kind==ListKind::files?EntryKind::file:EntryKind::directory;
const bool every=pattern=="*";
const std::string pattern_fold=every?std::string():fold_name(pattern);
std::vector<std::pair<std::string,std::string>> names;
names.reserve(entries.size());
for(EntryInfo& entry:entries){
if(entry.kind!=wanted)continue;
// A FolderSource listing carries the folds its cache already holds.
std::string fold=entry.fold.empty()?fold_name(entry.name):std::move(entry.fold);
if(!every&&!glob_match(pattern_fold,fold))continue;
names.emplace_back(std::move(fold),std::move(entry.name));
}
std::ranges::sort(names);
const auto [first,last]=std::ranges::unique(names,{},[](const auto& name)->const std::string&{return name.second;});
names.erase(first,last);
std::vector<std::string> result;
result.reserve(names.size());
for(auto& name:names)result.push_back(std::move(name.second));
return result;
});
}

std::expected<void,Failure> Vfs::create_directory(std::string_view path,bool recursive)
{
return guarded(*this,"create_directory",path,[&]()->std::expected<void,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->mount=="host:"){
const auto made=recursive?host_->make_directories(p->relative):host_->make_directory(p->relative);
if(!made)return failed(made.error());
return {};
}
const std::shared_ptr<const Table> table=snapshot();
std::expected<void,Failure> result=failed(FileError::not_found);
bool covered=false;
const bool stopped=visit_covering(*table,*p,[&](const Target& target){
covered=true;
if(!target.mount->writable)return false;
Source& source=*target.mount->source;
const auto fail=[&](FileError error){
result=std::unexpected(error==FileError::ambiguous?ambiguous_failure(path,target):Failure{error,{}});
return true;
};
if(recursive){
for(std::size_t at=target.sub.find('/');at!=std::string_view::npos;at=target.sub.find('/',at+1)){
if(auto made=source.make_directory(target.sub.substr(0,at));!made)return fail(made.error());
}
}
if(auto made=source.make_directory(target.sub);!made)return fail(made.error());
result={};
return true;
});
if(stopped)return result;
if(leads_to_mount(*table,*p))return {};
if(covered)return std::unexpected(Failure{FileError::read_only_pack,read_only_text(path)});
return result;
});
}

std::expected<void,Failure> Vfs::delete_directory(std::string_view path,bool recursive)
{
return guarded(*this,"delete_directory",path,[&]()->std::expected<void,Failure>{
const auto p=parse_failure(path);
if(!p)return std::unexpected(p.error());
if(p->mount=="host:"){
if(auto removed=host_->remove_directory(p->relative,recursive);!removed)return failed(removed.error());
return {};
}
const std::shared_ptr<const Table> table=snapshot();
if(leads_to_mount(*table,*p))return failed(FileError::access_denied);
std::expected<void,Failure> result=failed(FileError::not_found);
visit_covering(*table,*p,[&](const Target& target){
if(target.sub.empty()){
result=failed(FileError::access_denied);
return true;
}
Source& source=*target.mount->source;
const auto fail=[&](FileError error){
result=std::unexpected(error==FileError::ambiguous?ambiguous_failure(path,target):Failure{error,{}});
return true;
};
const auto info=source.stat(target.sub);
if(!info)return fall_through(info.error())?false:fail(info.error());
if(info->kind!=EntryKind::directory)return info->kind==EntryKind::deleted?false:fail(FileError::not_a_directory);
if(!target.mount->writable){
result=std::unexpected(Failure{FileError::read_only_pack,read_only_text(path)});
return true;
}
check_spelling(path,*p,target);
if(auto removed=source.remove_directory(target.sub,recursive);!removed)return fail(removed.error());
result={};
return true;
});
return result;
});
}
}
