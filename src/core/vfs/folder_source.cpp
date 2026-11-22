#include <core/vfs/folder_source.hpp>
#include <core/path_utf8.hpp>
#include <core/vfs/path.hpp>
#include <platform/clock.hpp>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>

namespace zodiac::vfs {
namespace fs=std::filesystem;
namespace detail {
namespace {
std::int64_t reader_failure(std::int64_t negated_errno) noexcept
{
const std::error_code error(static_cast<int>(-negated_errno),std::generic_category());
return -static_cast<std::int64_t>(detail::file_error(error));
}
// A folder's file opened for reading. The size is the one at open, so eof checks cost no system call.
class FileReader final:public Reader{
public:
explicit FileReader(platform::File file) noexcept:file_(std::move(file)),size_(file_.size()){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override
{
const std::int64_t got=file_.read_at(offset,buffer);
return got<0?reader_failure(got):got;
}
std::uint64_t size() const override{return size_;}
private:
platform::File file_;
std::uint64_t size_;
};
// A folder's file opened for writing. Every byte goes straight to the file, so commit has nothing left to do; the size is tracked
// from the writes, since one file object owns the target.
class FileWriter final:public WriteTarget{
public:
explicit FileWriter(platform::File file) noexcept:file_(std::move(file)),size_(file_.size()){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override
{
const std::int64_t got=file_.read_at(offset,buffer);
return got<0?reader_failure(got):got;
}
std::expected<void,FileError> write_at(std::uint64_t offset,std::span<const std::byte> bytes) override
{
if(offset>std::numeric_limits<std::uint64_t>::max()-bytes.size())return std::unexpected(FileError::io);
if(auto written=file_.write_at(offset,bytes);!written)return std::unexpected(detail::file_error(written.error()));
size_=std::max(size_,offset+bytes.size());
return {};
}
std::uint64_t size() const override{return size_;}
std::expected<void,FileError> truncate(std::uint64_t size) override
{
if(auto cut=file_.truncate(size);!cut)return std::unexpected(detail::file_error(cut.error()));
size_=size;
return {};
}
std::expected<void,FileError> commit() override{return {};}
std::expected<void,FileError> sync() override
{
if(auto synced=file_.sync_data();!synced)return std::unexpected(detail::file_error(synced.error()));
return {};
}
private:
platform::File file_;
std::uint64_t size_;
};
}

FileError file_error(const std::error_code& error) noexcept
{
switch(platform::error_kind(error)){
case platform::ErrorKind::not_found:
return FileError::not_found;
case platform::ErrorKind::not_a_directory:
return FileError::not_a_directory;
case platform::ErrorKind::is_directory:
return FileError::is_directory;
case platform::ErrorKind::access_denied:
return FileError::access_denied;
case platform::ErrorKind::not_empty:
return FileError::not_empty;
case platform::ErrorKind::disk_full:
return FileError::disk_full;
default:
return FileError::io;
}
}

EntryKind entry_kind(platform::NodeKind kind) noexcept
{
switch(kind){
case platform::NodeKind::file:
return EntryKind::file;
case platform::NodeKind::directory:
return EntryKind::directory;
default:
return EntryKind::none;
}
}

std::int64_t to_ms(std::int64_t ns) noexcept
{
const std::int64_t ms=ns/1000000;
return ns%1000000<0?ms-1:ms;
}

platform::FileAccess access_of(WriteMode mode) noexcept
{
switch(mode){
case WriteMode::truncate:
return platform::FileAccess::create_truncate;
case WriteMode::append:
return platform::FileAccess::create_or_open;
default:
return platform::FileAccess::read_write;
}
}

std::shared_ptr<Reader> make_file_reader(platform::File file)
{
return std::make_shared<FileReader>(std::move(file));
}

std::unique_ptr<WriteTarget> make_file_writer(platform::File file)
{
return std::make_unique<FileWriter>(std::move(file));
}

namespace {
// Removes a folder known not to be a link, with everything in it; the listing tells which children are links.
std::error_code remove_folder_tree(const fs::path& path)
{
std::vector<platform::DirectoryEntry> entries;
if(std::error_code error=platform::list_directory(path,entries))return error;
for(const platform::DirectoryEntry& entry:entries){
const fs::path child=path/path_from_utf8(entry.name);
const std::error_code error=!entry.link&&entry.kind==platform::NodeKind::directory?remove_folder_tree(child):platform::remove_file(child);
if(error&&platform::error_kind(error)!=platform::ErrorKind::not_found)return error;
}
return platform::remove_directory(path);
}
}

std::error_code remove_tree(const fs::path& path)
{
// A link to a folder goes, never what it leads to (as std::filesystem::remove_all).
if(platform::is_link(path))return platform::remove_file(path);
return remove_folder_tree(path);
}

std::error_code make_directories(const fs::path& path)
{
const auto node=platform::stat_path(path);
if(node)return node->kind==platform::NodeKind::directory?std::error_code():std::error_code(ENOTDIR,std::generic_category());
if(platform::error_kind(node.error())!=platform::ErrorKind::not_found)return node.error();
const fs::path parent=path.parent_path();
if(!parent.empty()&&parent!=path)if(std::error_code error=make_directories(parent))return error;
std::error_code error=platform::make_directory(path);
// Made meanwhile by someone else.
if(error&&platform::error_kind(error)==platform::ErrorKind::exists){
const auto made=platform::stat_path(path);
if(made&&made->kind==platform::NodeKind::directory)error.clear();
}
return error;
}
}

namespace {
// The parent folder of a '/'-joined real path ("" for a name in the root).
std::string_view parent_of(std::string_view real) noexcept
{
const std::size_t slash=real.rfind('/');
return slash==std::string_view::npos?std::string_view():real.substr(0,slash);
}
std::string_view leaf_of(std::string_view real) noexcept
{
const std::size_t slash=real.rfind('/');
return slash==std::string_view::npos?real:real.substr(slash+1);
}
EntryInfo entry_of(std::string_view relative,const platform::NodeInfo& node)
{
EntryInfo info;
info.kind=detail::entry_kind(node.kind);
info.size=node.size;
info.modified_utc_ms=detail::to_ms(node.modified_utc_ns);
info.name.assign(leaf_of(relative));
return info;
}
bool fall_through(FileError error) noexcept
{
return error==FileError::not_found||error==FileError::not_a_directory;
}
// How much of relative an exact-name try may be trusted for: relative.size() for all of it, else the offset where the first segment
// that may not be tried exactly begins (0 for the first). On Windows a '~' may be an 8.3 short name and a non-ASCII letter may fold
// differently in NTFS's own upcase table, so from the first such segment on the path is walked through the cache, which holds the
// long names only and folds as Linux does; the plain ASCII folders before it fold alike on both and are tried exactly. Elsewhere an
// exact hit is the name itself, so all of it is trusted.
std::size_t trusted_prefix(std::string_view relative) noexcept
{
#ifdef _WIN32
std::size_t segment=0;
for(std::size_t i=0;i<relative.size();++i){
const char c=relative[i];
if(c=='/')segment=i+1;
else if(c=='~'||static_cast<unsigned char>(c)>=0x80)return segment;
}
#endif
return relative.size();
}
// Whether the entry (a cached folder, or a name in one) spelled cached is the one spelled real, their folds already being equal.
// Where the file system ignores case, an exact hit spells an entry as asked, so the fold decides; elsewhere two entries may differ
// only in case, so the spelling must match too.
bool same_entry([[maybe_unused]] std::string_view cached,[[maybe_unused]] std::string_view real) noexcept
{
#ifdef _WIN32
return true;
#else
return cached==real;
#endif
}
// Whether two real paths below the root name one entry: by fold where the file system ignores case, by bytes elsewhere.
bool same_real_path(std::string_view a,std::string_view b)
{
#ifdef _WIN32
return a==b||fold_name(a)==fold_name(b);
#else
return a==b;
#endif
}
bool before_by_fold(const std::string& fold,const std::string& real,std::string_view other_fold,std::string_view other_real) noexcept
{
return fold!=other_fold?std::string_view(fold)<other_fold:std::string_view(real)<other_real;
}
// The last ambiguity this thread met in a FolderSource lookup, so the message names exactly what that lookup saw.
// Keyed by the source's id (0 is never one), not its address, which a later source could reuse.
struct LastAmbiguity{
std::uint64_t source=0;
std::string relative;
std::string first;
std::string second;
};
thread_local LastAmbiguity last_ambiguity;
std::atomic<std::uint64_t> next_source_id{1};
}

FolderSource::FolderSource(fs::path root,bool read_only,bool create_root)
:root_(std::move(root)),read_only_(read_only),create_root_(create_root),id_(next_source_id.fetch_add(1,std::memory_order_relaxed))
{
}

fs::path FolderSource::host_path(std::string_view real) const
{
return real.empty()?root_:root_/path_from_utf8(real);
}

FolderSource::Match FolderSource::match(const Names& names,std::string_view segment)
{
const std::string fold=fold_name(segment);
const auto range=std::ranges::equal_range(names.names,std::string_view(fold),{},[](const Name& name){return std::string_view(name.fold);});
Match result;
if(range.empty())return result;
for(const Name& name:range){
if(name.real==segment){
result.name=&name;
return result;
}
}
if(range.size()==1)result.name=&range.front();
else result.ambiguous=true;
return result;
}

std::optional<std::int64_t> FolderSource::folder_time(std::string_view real_folder) const
{
const auto node=platform::stat_path(host_path(real_folder));
if(!node||node->kind!=platform::NodeKind::directory)return std::nullopt;
return node->modified_utc_ns;
}

std::expected<std::shared_ptr<const FolderSource::Names>,FileError> FolderSource::folder_names(const std::string& real_folder)
{
const std::string key=fold_name(real_folder);
std::shared_ptr<const Names> cached;
std::uint64_t generation;
{
const std::lock_guard lock(cache_mutex_);
const CacheEntry& entry=cache_[key];
cached=entry.names;
generation=entry.generation;
}
const fs::path folder=host_path(real_folder);
// The folder's time is read before its names, so a change during the scan leaves a time that differs from the next one read.
const auto node=platform::stat_path(folder);
if(!node){
const FileError error=detail::file_error(node.error());
if(error==FileError::not_found&&real_folder.empty()&&create_root_){
static const std::shared_ptr<const Names> empty=std::make_shared<const Names>();
return empty;
}
return std::unexpected(error);
}
if(node->kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
if(cached&&cached->modified_ns==node->modified_utc_ns&&same_entry(cached->real,real_folder)){
if(!cached->racy)return cached;
// A racy entry is trusted for one window after its scan, so a folder that keeps changing is rescanned at most once per window.
if(platform::utc_now_ms()*1000000-cached->scanned_ns<racy_window_ns)return cached;
}
const std::int64_t scanned_ns=platform::utc_now_ms()*1000000;
std::vector<platform::DirectoryEntry> entries;
if(const std::error_code error=platform::list_directory(folder,entries))return std::unexpected(detail::file_error(error));
scans_.fetch_add(1,std::memory_order_relaxed);
auto names=std::make_shared<Names>();
names->real=real_folder;
names->modified_ns=node->modified_utc_ns;
names->scanned_ns=scanned_ns;
// Either way: a folder time in the future (clock skew, another machine's share) must not keep the entry racy forever.
const std::int64_t distance=node->modified_utc_ns>scanned_ns?node->modified_utc_ns-scanned_ns:scanned_ns-node->modified_utc_ns;
names->racy=distance<racy_window_ns;
names->names.reserve(entries.size());
for(platform::DirectoryEntry& entry:entries){
if(entry.kind!=platform::NodeKind::file&&entry.kind!=platform::NodeKind::directory)continue;
if(!valid_name(entry.name)){
// A temporary file of a write or copy that a crash interrupted: only the exact generated form, dated more than stale_temp_ns
// away from now by its name's own time, either way (a clock set back, or another machine's, cannot keep a leftover forever; a
// live temp of another process removed so makes only its own write fail, its target kept), and not held by a write of this process.
if(!read_only_&&entry.kind==platform::NodeKind::file&&!entry.link){
const std::optional<std::int64_t> age=temp_name_age(entry.name,scanned_ns/1000000000);
if(age&&(*age<0?-*age:*age)>stale_temp_ns/1000000000&&!temp_held(entry.name))platform::discard_file(folder/path_from_utf8(entry.name));
}
continue;
}
Name& name=names->names.emplace_back();
name.fold=fold_name(entry.name);
name.real=std::move(entry.name);
name.kind=entry.kind;
}
std::ranges::sort(names->names,[](const Name& a,const Name& b){return before_by_fold(a.fold,a.real,b.fold,b.real);});
std::shared_ptr<const Names> result=std::move(names);
{
const std::lock_guard lock(cache_mutex_);
CacheEntry& entry=cache_[key];
// A write in the folder since the scan began: its names may predate the write, so they are used once and not kept.
if(entry.generation==generation)entry.names=result;
}
return result;
}

void FolderSource::note_change(std::string_view real_folder,std::string_view leaf,platform::NodeKind kind,bool added,std::optional<std::int64_t> before_ns)
{
const std::string key=fold_name(real_folder);
const std::optional<std::int64_t> after_ns=folder_time(real_folder);
std::shared_ptr<const Names> current;
std::uint64_t generation;
{
const std::lock_guard lock(cache_mutex_);
CacheEntry& entry=cache_[key];
generation=++entry.generation;
current=std::move(entry.names);
}
// The names stay only when they matched the folder just before the change, so nothing another process did is taken in unseen.
if(!current||!before_ns||!after_ns||!same_entry(current->real,real_folder)||current->modified_ns!=*before_ns)return;
// Taken out of the cache, the names are this call's alone unless a lookup still holds them; then they are copied, never changed under it.
// (Every cached Names was made non-const by make_shared, so changing an unshared one is sound.)
std::shared_ptr<Names> next=current.use_count()==1?std::const_pointer_cast<Names>(std::move(current)):std::make_shared<Names>(*current);
next->modified_ns=*after_ns;
const std::int64_t now_ns=platform::utc_now_ms()*1000000;
const std::int64_t distance=*after_ns>now_ns?*after_ns-now_ns:now_ns-*after_ns;
next->racy=next->racy||distance<racy_window_ns;
std::string fold=fold_name(leaf);
// The entry the change was about: on a file system that ignores case the leaf may be spelled as asked, so any name of its fold is it.
const auto range=std::ranges::equal_range(next->names,std::string_view(fold),{},[](const Name& name){return std::string_view(name.fold);});
const auto same=std::ranges::find_if(range,[leaf](const Name& name){return same_entry(name.real,leaf);});
if(added&&same==range.end()){
const auto at=std::lower_bound(next->names.begin(),next->names.end(),0,[&fold,leaf](const Name& name,int){return before_by_fold(name.fold,name.real,fold,leaf);});
next->names.insert(at,Name{std::move(fold),std::string(leaf),kind});
}
else if(!added&&same!=range.end())next->names.erase(same);
std::shared_ptr<const Names> result=std::move(next);
const std::lock_guard lock(cache_mutex_);
CacheEntry& entry=cache_[key];
// Only when no other change or scan came in between; otherwise the folder is simply scanned again.
if(entry.generation==generation&&!entry.names)entry.names=std::move(result);
}

void FolderSource::invalidate(std::string_view real_folder)
{
const std::string key=fold_name(real_folder);
const std::lock_guard lock(cache_mutex_);
CacheEntry& entry=cache_[key];
++entry.generation;
entry.names.reset();
}

void FolderSource::invalidate_tree(std::string_view real_folder)
{
const std::string key=fold_name(real_folder);
const std::lock_guard lock(cache_mutex_);
for(auto& [folder,entry]:cache_){
if(!folder.starts_with(key)||(folder.size()!=key.size()&&folder[key.size()]!='/'))continue;
++entry.generation;
entry.names.reset();
}
}

std::expected<FolderSource::Resolved,FileError> FolderSource::resolve(std::string_view relative,bool leaf_may_miss)
{
Resolved result;
if(relative.empty()){
const auto node=platform::stat_path(root_);
if(node){
result.node=*node;
return result;
}
const FileError error=detail::file_error(node.error());
if(error!=FileError::not_found||!create_root_)return std::unexpected(error);
// A data folder that no write has made yet reads as an empty folder.
result.node.kind=platform::NodeKind::directory;
return result;
}
// The exact name first: one system call when the spelling is right (always, on a file system that ignores case).
const std::size_t trusted=trusted_prefix(relative);
std::size_t start=0;
if(trusted==relative.size()){
const auto exact=platform::stat_path(host_path(relative));
if(exact){
result.real.assign(relative);
result.node=*exact;
return result;
}
if(const FileError error=detail::file_error(exact.error());!fall_through(error))return std::unexpected(error);
}
else if(trusted>0){
// Only the leading plain folders are tried exactly; the walk through the cache starts below them.
const std::string_view folders=relative.substr(0,trusted-1);
const auto exact=platform::stat_path(host_path(folders));
if(exact){
if(exact->kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
result.real.assign(folders);
start=trusted;
}
else if(const FileError error=detail::file_error(exact.error());!fall_through(error))return std::unexpected(error);
}
result.real.reserve(relative.size());
for(std::size_t at=start;;){
const std::size_t cut=relative.find('/',at);
const bool last=cut==std::string_view::npos;
const std::string_view segment=relative.substr(at,last?std::string_view::npos:cut-at);
const auto names=folder_names(result.real);
if(!names)return std::unexpected(names.error());
const Match found=match(**names,segment);
const std::string prefix=result.real.empty()?std::string():result.real+"/";
if(found.ambiguous){
// The fold's names are sorted by bytes inside their range, so the first two are the smallest.
const std::string fold=fold_name(segment);
const auto range=std::ranges::equal_range((*names)->names,std::string_view(fold),{},[](const Name& name){return std::string_view(name.fold);});
last_ambiguity=LastAmbiguity{id_,std::string(relative),prefix+range[0].real,prefix+range[1].real};
return std::unexpected(FileError::ambiguous);
}
if(!found.name){
if(!last||!leaf_may_miss)return std::unexpected(FileError::not_found);
result.real=prefix;
result.real.append(segment);
result.node=platform::NodeInfo{};
result.parent_modified_ns=(*names)->modified_ns;
return result;
}
result.real=prefix;
result.real.append(found.name->real);
if(last)break;
if(found.name->kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
at=cut+1;
}
const auto node=platform::stat_path(host_path(result.real));
if(!node){
const FileError error=detail::file_error(node.error());
// Removed since the folder was scanned.
if(error==FileError::not_found&&leaf_may_miss){
result.node=platform::NodeInfo{};
return result;
}
return std::unexpected(error);
}
result.node=*node;
return result;
}

std::expected<void,FileError> FolderSource::ensure_root()
{
if(!create_root_)return {};
const auto node=platform::stat_path(root_);
if(node)return {};
if(const std::error_code error=detail::make_directories(root_))return std::unexpected(detail::file_error(error));
invalidate({});
return {};
}

std::expected<EntryInfo,FileError> FolderSource::stat(std::string_view relative)
{
const auto found=resolve(relative,false);
if(!found)return std::unexpected(found.error());
if(found->node.kind!=platform::NodeKind::file&&found->node.kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_found);
// The name as asked: an exact hit on a file system that ignores case never learns the stored spelling, and every platform answers alike.
return entry_of(relative,found->node);
}

std::expected<std::shared_ptr<Reader>,FileError> FolderSource::open_read(std::string_view relative)
{
if(relative.empty())return std::unexpected(FileError::is_directory);
// The exact name first, straight to an open: a right spelling costs one system call and the size query.
if(trusted_prefix(relative)==relative.size()){
auto file=platform::File::open(host_path(relative),platform::FileAccess::read);
if(file)return detail::make_file_reader(std::move(*file));
if(const FileError error=detail::file_error(file.error());!fall_through(error))return std::unexpected(error);
}
const auto found=resolve(relative,false);
if(!found)return std::unexpected(found.error());
if(found->node.kind==platform::NodeKind::directory)return std::unexpected(FileError::is_directory);
if(found->node.kind!=platform::NodeKind::file)return std::unexpected(FileError::not_found);
auto file=platform::File::open(host_path(found->real),platform::FileAccess::read);
if(!file)return std::unexpected(detail::file_error(file.error()));
return detail::make_file_reader(std::move(*file));
}

std::expected<std::unique_ptr<WriteTarget>,FileError> FolderSource::open_write(std::string_view relative,WriteMode mode)
{
if(read_only_)return std::unexpected(FileError::access_denied);
if(relative.empty())return std::unexpected(FileError::is_directory);
if(mode!=WriteMode::update)if(auto made=ensure_root();!made)return std::unexpected(made.error());
const auto found=resolve(relative,mode!=WriteMode::update);
if(!found)return std::unexpected(found.error());
if(found->node.kind==platform::NodeKind::directory)return std::unexpected(FileError::is_directory);
const bool created=found->node.kind==platform::NodeKind::none;
auto file=platform::File::open(host_path(found->real),detail::access_of(mode));
if(!file)return std::unexpected(detail::file_error(file.error()));
if(created)note_change(parent_of(found->real),leaf_of(found->real),platform::NodeKind::file,true,found->parent_modified_ns);
return detail::make_file_writer(std::move(*file));
}

std::expected<void,FileError> FolderSource::list(std::string_view relative,std::vector<EntryInfo>& out)
{
const auto found=resolve(relative,false);
if(!found)return std::unexpected(found.error());
if(found->node.kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
const auto names=folder_names(found->real);
if(!names)return std::unexpected(names.error());
out.reserve(out.size()+(*names)->names.size());
for(const Name& name:(*names)->names){
EntryInfo& info=out.emplace_back();
info.kind=detail::entry_kind(name.kind);
info.name=name.real;
info.fold=name.fold;
}
return {};
}

std::expected<void,FileError> FolderSource::remove(std::string_view relative)
{
if(read_only_)return std::unexpected(FileError::access_denied);
if(relative.empty())return std::unexpected(FileError::is_directory);
const auto found=resolve(relative,false);
if(!found)return std::unexpected(found.error());
if(found->node.kind==platform::NodeKind::directory)return std::unexpected(FileError::is_directory);
const std::string_view parent=parent_of(found->real);
const std::optional<std::int64_t> before=folder_time(parent);
const std::error_code error=platform::remove_file(host_path(found->real));
if(error){
invalidate(parent);
return std::unexpected(detail::file_error(error));
}
note_change(parent,leaf_of(found->real),platform::NodeKind::file,false,before);
return {};
}

std::expected<void,FileError> FolderSource::make_directory(std::string_view relative)
{
if(read_only_)return std::unexpected(FileError::access_denied);
if(auto made=ensure_root();!made)return std::unexpected(made.error());
const auto found=resolve(relative,true);
if(!found)return std::unexpected(found.error());
if(found->node.kind==platform::NodeKind::directory)return {};
if(found->node.kind!=platform::NodeKind::none)return std::unexpected(FileError::not_a_directory);
const std::error_code error=platform::make_directory(host_path(found->real));
const std::string_view parent=parent_of(found->real);
if(error&&platform::error_kind(error)==platform::ErrorKind::exists){
invalidate(parent);
const auto made=platform::stat_path(host_path(found->real));
if(made&&made->kind==platform::NodeKind::directory)return {};
return std::unexpected(FileError::not_a_directory);
}
if(error){
invalidate(parent);
return std::unexpected(detail::file_error(error));
}
note_change(parent,leaf_of(found->real),platform::NodeKind::directory,true,found->parent_modified_ns);
return {};
}

std::expected<void,FileError> FolderSource::remove_directory(std::string_view relative,bool recursive)
{
if(read_only_||relative.empty())return std::unexpected(FileError::access_denied);
const auto found=resolve(relative,false);
if(!found)return std::unexpected(found.error());
if(found->node.kind!=platform::NodeKind::directory)return std::unexpected(FileError::not_a_directory);
const fs::path folder=host_path(found->real);
const std::string_view parent=parent_of(found->real);
const std::optional<std::int64_t> before=folder_time(parent);
const std::error_code error=recursive?detail::remove_tree(folder):platform::remove_directory(folder);
invalidate_tree(found->real);
if(error){
invalidate(parent);
return std::unexpected(detail::file_error(error));
}
note_change(parent,leaf_of(found->real),platform::NodeKind::directory,false,before);
return {};
}

std::string FolderSource::write_name([[maybe_unused]] std::string_view relative,const Resolved& found)
{
#ifdef _WIN32
// An exact hit spells an existing entry as asked, and a rename or a copy would give it that spelling: keep the stored name, as a
// write over it does.
if(found.node.kind!=platform::NodeKind::none){
std::string stored;
if(real_spelling(relative,stored))return stored;
}
#endif
return found.real;
}

std::expected<std::optional<fs::path>,FileError> FolderSource::host_file(std::string_view relative,bool for_write)
{
if(relative.empty())return std::unexpected(FileError::is_directory);
if(for_write){
if(read_only_)return std::unexpected(FileError::access_denied);
if(auto made=ensure_root();!made)return std::unexpected(made.error());
}
const auto found=resolve(relative,for_write);
if(!found)return std::unexpected(found.error());
if(found->node.kind==platform::NodeKind::directory)return std::unexpected(FileError::is_directory);
if(found->node.kind==platform::NodeKind::none&&!for_write)return std::unexpected(FileError::not_found);
if(found->node.kind==platform::NodeKind::other)return std::unexpected(for_write?FileError::access_denied:FileError::not_found);
return host_path(write_name(relative,*found));
}

void FolderSource::forget(std::string_view relative)
{
invalidate(parent_of(relative));
}

std::expected<bool,MoveError> FolderSource::rename(std::string_view from,std::string_view to)
{
const auto on_from=[](FileError code){return std::unexpected(MoveError{code,false});};
const auto on_to=[](FileError code){return std::unexpected(MoveError{code,true});};
if(from.empty())return on_from(FileError::is_directory);
if(to.empty())return on_to(FileError::is_directory);
if(read_only_)return on_from(FileError::access_denied);
const auto old_entry=resolve(from,false);
if(!old_entry)return on_from(old_entry.error());
if(old_entry->node.kind==platform::NodeKind::directory)return on_from(FileError::is_directory);
if(old_entry->node.kind!=platform::NodeKind::file)return on_from(FileError::not_found);
const auto new_entry=resolve(to,true);
if(!new_entry)return on_to(new_entry.error());
if(new_entry->node.kind==platform::NodeKind::directory)return on_to(FileError::is_directory);
const std::string_view old_parent=parent_of(old_entry->real);
const std::string_view new_parent=parent_of(new_entry->real);
// One entry under two spellings (to reused from's name, or the file system ignores case): nothing to do.
if(same_real_path(old_entry->real,new_entry->real))return true;
const std::string target=write_name(to,*new_entry);
const bool same_folder=same_real_path(old_parent,new_parent);
const std::optional<std::int64_t> old_before=folder_time(old_parent);
std::optional<std::int64_t> new_before;
if(!same_folder)new_before=new_entry->node.kind==platform::NodeKind::none&&new_entry->parent_modified_ns?new_entry->parent_modified_ns:folder_time(new_parent);
std::error_code error;
if(!platform::replace_file(host_path(old_entry->real),host_path(target),error)){
invalidate(old_parent);
if(!same_folder)invalidate(new_parent);
if(platform::error_kind(error)==platform::ErrorKind::cross_device)return false;
// from was there a moment ago: unless it is gone now, the failure is about to.
return std::unexpected(MoveError{detail::file_error(error),platform::stat_path(host_path(old_entry->real)).has_value()});
}
note_change(old_parent,leaf_of(old_entry->real),platform::NodeKind::file,false,old_before);
// In one folder the first change already moved the cached names to the folder's time after the rename.
note_change(new_parent,leaf_of(target),platform::NodeKind::file,true,same_folder?folder_time(new_parent):new_before);
return true;
}

bool FolderSource::real_spelling(std::string_view relative,std::string& real)
{
if(relative.empty())return false;
std::string spelled;
spelled.reserve(relative.size());
for(std::size_t at=0;;){
const std::size_t cut=relative.find('/',at);
const bool last=cut==std::string_view::npos;
const std::string_view segment=relative.substr(at,last?std::string_view::npos:cut-at);
const auto names=folder_names(spelled);
if(!names)return false;
const Match found=match(**names,segment);
if(!found.name)return false;
if(!spelled.empty())spelled.push_back('/');
spelled.append(found.name->real);
if(last)break;
at=cut+1;
}
if(spelled==relative)return false;
real=std::move(spelled);
return true;
}

std::optional<std::pair<std::string,std::string>> FolderSource::ambiguous_names(std::string_view relative)
{
// What this thread's failed lookup itself saw, so the names cannot have changed in between.
if(last_ambiguity.source==id_&&last_ambiguity.relative==relative)return std::pair(last_ambiguity.first,last_ambiguity.second);
if(!resolve(relative,false)&&last_ambiguity.source==id_&&last_ambiguity.relative==relative)return std::pair(last_ambiguity.first,last_ambiguity.second);
return std::nullopt;
}
}
