// PackStorage (contract in pack_storage.hpp): the registry, opening and recovery, the chunked writer and reader, flush and
// compact. Every write happens with no window (a window is read only), so a writable storage's offsets are file offsets.
#include <core/pack/pack_storage.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/path.hpp>
#include <monocypher.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace zodiac::pack {
namespace {
using vfs::Failure;
using vfs::FileAction;
using vfs::FileError;

// What one write of chunks covers at most (a chunk larger than this is written alone).
constexpr std::size_t kBatchBytes=std::size_t{1}<<20;
// A header, the smallest index and a footer.
constexpr std::uint64_t kMinPackSize=kHeaderSize+kMinIndexLength+kFooterSize;
// What the recovery scan reads at a time.
constexpr std::size_t kScanBlock=65536;
constexpr std::uint64_t kNoChunk=std::numeric_limits<std::uint64_t>::max();

PackError wrong_key()
{
return PackError{FileAction::read,Failure{FileError::damaged,kWrongKeyText}};
}
PackError newer_format()
{
return PackError{FileAction::read,Failure{FileError::damaged,kNewerFormatText}};
}
PackError system_failure(FileAction action,const std::error_code& error)
{
return PackError{action,Failure{vfs::detail::file_error(error),{}}};
}
FileError file_error(const std::error_code& error) noexcept
{
return vfs::detail::file_error(error);
}
// The FileError of a failed File::read_at (a negated errno number).
FileError read_failure(std::int64_t result) noexcept
{
return file_error(std::error_code(static_cast<int>(-result),std::generic_category()));
}
std::int64_t negated(FileError error) noexcept
{
return -static_cast<std::int64_t>(error);
}
// Reads exactly size bytes at offset: 0, a negated errno number, or 1 when the file ends first.
std::int64_t read_exact(const platform::File& file,std::uint64_t offset,std::byte* to,std::size_t size) noexcept
{
const std::int64_t got=file.read_at(offset,{to,size});
if(got<0)return got;
return static_cast<std::uint64_t>(got)==size?0:1;
}
// The master key key stands for under header: kWrongKeyText for a key of the other kind or a derivation that fails.
std::expected<crypto::Key,PackError> master_key(const KeySource& key,const Header& header)
{
if(header.kdf==Kdf::raw_key){
const crypto::Key* raw=std::get_if<crypto::Key>(&key);
if(!raw)return std::unexpected(wrong_key());
return *raw;
}
const std::string* password=std::get_if<std::string>(&key);
if(!password)return std::unexpected(wrong_key());
try{
return crypto::derive_password_key(*password,header.salt,header.kdf_kib,header.kdf_passes);
}
// The header's settings passed decode_header's limits; anything derive_password_key still refuses, or memory it cannot have,
// is a pack this machine cannot open, reported like damage.
catch(const std::invalid_argument&){
return std::unexpected(wrong_key());
}
catch(const std::bad_alloc&){
return std::unexpected(wrong_key());
}
}
std::expected<crypto::Key,PackError> file_key_of(const KeySource& key,const Header& header)
{
auto master=master_key(key,header);
if(!master)return std::unexpected(master.error());
const crypto::Key file_key=crypto::derive_file_key(*master,header.salt);
crypto::wipe(master->data(),master->size());
return file_key;
}
void wipe_key_source(KeySource& key) noexcept
{
if(auto* password=std::get_if<std::string>(&key))crypto::wipe(password->data(),password->size());
else{
crypto::Key& raw=std::get<crypto::Key>(key);
crypto::wipe(raw.data(),raw.size());
}
}
// Checks a pack's settings before anything is created: std::invalid_argument outside the header's limits.
void check_options(const PackOptions& options,const KeySource& key)
{
if(options.chunk_log2<kMinChunkLog2||options.chunk_log2>kMaxChunkLog2)throw std::invalid_argument("the pack chunk size is out of range");
if(std::holds_alternative<std::string>(key)&&(options.kdf_kib<crypto::kMinKdfKib||options.kdf_kib>crypto::kMaxKdfKib||options.kdf_passes<1||options.kdf_passes>crypto::kMaxKdfPasses))throw std::invalid_argument("the key derivation settings are out of range");
}

// The process-wide registry of open storages. Never freed, so a storage released during static destruction still finds it.
struct Slot{
std::weak_ptr<PackStorage> storage;
const PackStorage* raw=nullptr;
// An open is loading the storage of this slot (with the registry's lock released): others wait for it.
bool loading=false;
};
using PathText=std::filesystem::path::string_type;
// A storage's place: the canonical path, and its window (none: offset and size 0).
struct RegistryKey{
PathText path;
std::uint64_t offset=0;
std::uint64_t size=0;
bool windowed=false;
};
// A key as unregister looks it up, so that the lookup allocates nothing.
struct KeyView{
std::basic_string_view<std::filesystem::path::value_type> path;
std::uint64_t offset=0;
std::uint64_t size=0;
bool windowed=false;
};
KeyView view_of(const RegistryKey& key) noexcept
{
return {key.path,key.offset,key.size,key.windowed};
}
KeyView view_of(const KeyView& key) noexcept
{
return key;
}
struct KeyLess{
using is_transparent=void;
template<class A,class B>
bool operator()(const A& a,const B& b) const noexcept
{
const KeyView x=view_of(a);
const KeyView y=view_of(b);
return std::tie(x.path,x.windowed,x.offset,x.size)<std::tie(y.path,y.windowed,y.offset,y.size);
}
};
struct Registry{
std::mutex mutex;
// Signalled when a slot is erased or finishes loading.
std::condition_variable changed;
std::map<RegistryKey,Slot,KeyLess> slots;
};
Registry& registry()
{
static Registry* const instance=new Registry;
return *instance;
}

#if defined(ZODIAC_TEST_HOOKS)
std::mutex seal_mutex;
std::vector<testing::SealRecord>* seal_log=nullptr;
std::atomic<std::uint64_t> writes_before_failure{std::numeric_limits<std::uint64_t>::max()};
#endif
// Writes one batch of chunks (with ZODIAC_TEST_HOOKS, the write pack_fail_chunk_write chose fails).
std::expected<void,std::error_code> write_chunks(platform::File& file,std::uint64_t at,std::span<const std::byte> bytes) noexcept
{
#if defined(ZODIAC_TEST_HOOKS)
for(std::uint64_t left=writes_before_failure.load();left!=std::numeric_limits<std::uint64_t>::max();){
if(writes_before_failure.compare_exchange_weak(left,left==0?std::numeric_limits<std::uint64_t>::max():left-1)){
if(left==0)return std::unexpected(std::error_code(EIO,std::generic_category()));
break;
}
}
#endif
return file.write_at(at,bytes);
}

// Calls visit(folder) for each folder of a '/'-joined name or fold: the text before each '/'.
template<class Visit>
void each_folder(std::string_view fold,Visit&& visit)
{
for(std::size_t at=fold.find('/');at!=std::string_view::npos;at=fold.find('/',at+1))visit(fold.substr(0,at));
}
// name without its first depth segments (it has more).
std::string_view segments_after(std::string_view name,std::size_t depth) noexcept
{
std::size_t at=0;
for(;depth;--depth)at=name.find('/',at)+1;
return name.substr(at);
}
// True when fold names something below folder fold (fold continues it with '/').
bool below(std::string_view fold,std::string_view folder) noexcept
{
return fold.size()>folder.size()&&fold[folder.size()]=='/'&&fold.starts_with(folder);
}

// Reads one entry of a pack file from the snapshot open_reader took.
class PackReader final:public vfs::Reader{
public:
PackReader(std::shared_ptr<platform::File> file,std::uint64_t offset,std::uint64_t size,std::uint64_t id,std::uint8_t chunk_log2,const crypto::Key& key,std::shared_ptr<const void> holder) noexcept
:file_(std::move(file)),holder_(std::move(holder)),offset_(offset),size_(size),id_(id),key_(key),chunk_size_(std::size_t{1}<<chunk_log2),chunk_log2_(chunk_log2){}
~PackReader() override
{
crypto::wipe(key_.data(),key_.size());
}
PackReader(const PackReader&)=delete;
PackReader& operator=(const PackReader&)=delete;
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override;
std::uint64_t size() const override{return size_;}
private:
std::int64_t load(std::uint64_t index,std::size_t length,std::byte* plain);
std::shared_ptr<platform::File> file_;
// What the reader keeps alive besides its file (a mount's storage), or nothing.
std::shared_ptr<const void> holder_;
std::uint64_t offset_;
std::uint64_t size_;
std::uint64_t id_;
crypto::Key key_;
std::size_t chunk_size_;
std::uint8_t chunk_log2_;
// One stored chunk (ciphertext and tag); after a partial read, the plaintext of chunk cached_.
std::unique_ptr<std::byte[]> buffer_;
std::uint64_t cached_=kNoChunk;
};

// Reads chunk index (length plaintext bytes) and opens it into plain, which is buffer_ itself (in place) or memory disjoint from
// it: 0, or a negated FileError (plain is then zeroed, or untouched when the read failed).
std::int64_t PackReader::load(std::uint64_t index,std::size_t length,std::byte* plain)
{
cached_=kNoChunk;
const std::int64_t got=read_exact(*file_,offset_+index*(chunk_size_+kTagSize),buffer_.get(),length+kTagSize);
if(got<0)return negated(read_failure(got));
if(got>0)return negated(FileError::damaged);
crypto::Tag tag;
std::memcpy(tag.data(),buffer_.get()+length,kTagSize);
const bool authentic=crypto::open(key_,chunk_nonce(static_cast<std::uint32_t>(index),id_),{},{buffer_.get(),length},tag,{plain,length});
return authentic?0:negated(FileError::damaged);
}

std::int64_t PackReader::read_at(std::uint64_t offset,std::span<std::byte> buffer)
{
if(offset>=size_||buffer.empty())return 0;
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size_-offset));
// Made by the first read, kept for the reader's life: every chunk is read into it.
if(!buffer_)buffer_=std::make_unique_for_overwrite<std::byte[]>(chunk_size_+kTagSize);
std::byte* const out=buffer.data();
std::size_t done=0;
while(done<wanted){
const std::uint64_t at=offset+done;
const std::uint64_t index=at>>chunk_log2_;
const auto within=static_cast<std::size_t>(at&(chunk_size_-1));
const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size_,size_-(index<<chunk_log2_)));
const std::size_t take=std::min(length-within,wanted-done);
if(index!=cached_){
// A whole chunk goes straight into the caller's buffer; a part of one through the cache.
if(within==0&&take==length){
if(const std::int64_t loaded=load(index,length,out+done);loaded<0)return loaded;
done+=take;
continue;
}
if(const std::int64_t loaded=load(index,length,buffer_.get());loaded<0)return loaded;
cached_=index;
}
std::memcpy(out+done,buffer_.get()+within,take);
done+=take;
}
return static_cast<std::int64_t>(wanted);
}
}

std::expected<std::shared_ptr<PackStorage>,PackError> PackStorage::open(const std::filesystem::path& host,KeySource key,bool create,const PackOptions& options,std::optional<PackLocation> window)
{
struct Wipe{
KeySource& key;
~Wipe(){wipe_key_source(key);}
} wipe{key};
std::error_code error;
const std::filesystem::path canonical=std::filesystem::weakly_canonical(host,error);
if(error)return std::unexpected(system_failure(FileAction::read,error));
RegistryKey slot_key{canonical.native(),window?window->offset:0,window?window->size:0,window.has_value()};
Registry& shared=registry();
std::unique_lock lock(shared.mutex);
for(;;){
const auto found=shared.slots.find(slot_key);
if(found==shared.slots.end())break;
if(!found->second.loading){
std::shared_ptr<PackStorage> existing=found->second.storage.lock();
// A storage closing (closed, not yet out of the registry) is waited for like one being freed. Its shared lock is taken
// under the registry's, which close never holds both of (it leaves the registry after releasing its own locks).
if(existing&&existing->is_open()){
lock.unlock();
// header_ and file_key_ never change once a storage is loaded, so they are read without its locks.
auto file_key=file_key_of(key,existing->header_);
if(!file_key)return std::unexpected(file_key.error());
const bool same=crypto_verify32(file_key->data(),existing->file_key_.data())==0;
crypto::wipe(file_key->data(),file_key->size());
if(!same)return std::unexpected(wrong_key());
return existing;
}
// Dropped before waiting, so a last release never happens under the registry's lock (a release locks it): a closed
// storage's release does not (its close already left the registry), and an expired one has nothing to drop.
existing.reset();
}
// Being loaded by another open, closed or freed: wait for its slot to change.
shared.changed.wait(lock);
}
// The slot marks the load in progress; the load itself (file reads, the key derivation) runs without the registry's lock.
const auto slot=shared.slots.try_emplace(slot_key).first;
slot->second.loading=true;
lock.unlock();
const auto abandon=[&]{
const std::lock_guard relock(shared.mutex);
shared.slots.erase(slot);
shared.changed.notify_all();
};
std::shared_ptr<PackStorage> storage;
try{
auto loaded=load(canonical,key,create,options,window);
if(!loaded){
abandon();
return std::unexpected(std::move(loaded.error()));
}
(*loaded)->registry_path_=std::move(slot_key.path);
// Not registered yet, so a failure of share's own allocation frees it without touching the registry.
storage=share(std::move(*loaded));
}
catch(...){
abandon();
throw;
}
{
const std::lock_guard relock(shared.mutex);
slot->second=Slot{storage,storage.get(),false};
storage->registered_=true;
}
shared.changed.notify_all();
return storage;
}

std::shared_ptr<PackStorage> PackStorage::share(std::unique_ptr<PackStorage> storage)
{
// The last holder closes the storage (flushing it) and reports a failure there is no one else to tell.
return std::shared_ptr<PackStorage>(storage.release(),[](PackStorage* released) noexcept{
try{
if(auto closed=released->close();!closed){
const std::lock_guard guard(released->log_mutex_);
if(released->log_){
std::string line="error: cannot write \"";
line.append(one_line(released->log_name_)).append("\": ").append(vfs::reason_text(closed.error()));
released->log_(line);
}
}
}
catch(...){
}
delete released;
});
}

PackStorage::~PackStorage()
{
crypto::wipe(file_key_.data(),file_key_.size());
crypto::wipe(index_key_.data(),index_key_.size());
}

void PackStorage::take_keys(crypto::Key& file_key) noexcept
{
file_key_=file_key;
index_key_=crypto::derive_index_key(file_key_);
crypto::wipe(file_key.data(),file_key.size());
#if defined(ZODIAC_TEST_HOOKS)
key_tag_=crypto::hash(std::as_bytes(std::span(file_key_)));
index_tag_=crypto::hash(std::as_bytes(std::span(index_key_)));
#endif
}

// Takes file's exclusive lock for a writable storage: in_use when another open of the file holds it. A system that cannot lock
// leaves the file unlocked (the lock only guards against a second program; nothing else depends on it).
std::expected<void,PackError> PackStorage::lock_file(platform::File& file) const
{
const auto locked=file.lock_exclusive();
if(!locked||*locked)return {};
std::string text(1,'"');
text.append(one_line(path_to_utf8(host_))).push_back('"');
text.append(kInUseText);
return std::unexpected(PackError{FileAction::write,Failure{FileError::access_denied,std::move(text)},true});
}

std::expected<std::unique_ptr<PackStorage>,PackError> PackStorage::load(const std::filesystem::path& host,const KeySource& key,bool create,const PackOptions& options,std::optional<PackLocation> window)
{
std::unique_ptr<PackStorage> storage(new PackStorage());
storage->host_=host;
if(window){
auto file=platform::File::open(host,platform::FileAccess::read);
if(!file)return std::unexpected(system_failure(FileAction::read,file.error()));
const std::uint64_t size=file->size();
if(window->size>size||window->offset>size-window->size)return std::unexpected(wrong_key());
storage->file_=std::make_shared<platform::File>(std::move(*file));
storage->base_=window->offset;
storage->window_size_=window->size;
storage->windowed_=true;
storage->read_only_=true;
if(auto read=storage->read_existing(key);!read)return std::unexpected(std::move(read.error()));
return storage;
}
for(int attempt=0;;++attempt){
auto file=platform::File::open(host,platform::FileAccess::read_write);
if(file){
if(auto locked=storage->lock_file(*file);!locked)return std::unexpected(std::move(locked.error()));
storage->file_=std::make_shared<platform::File>(std::move(*file));
break;
}
const platform::ErrorKind kind=platform::error_kind(file.error());
if(kind==platform::ErrorKind::access_denied){
auto readable=platform::File::open(host,platform::FileAccess::read);
if(!readable)return std::unexpected(system_failure(FileAction::read,readable.error()));
storage->file_=std::make_shared<platform::File>(std::move(*readable));
storage->read_only_=true;
break;
}
if(kind!=platform::ErrorKind::not_found||!create)return std::unexpected(system_failure(FileAction::read,file.error()));
check_options(options,key);
auto made=platform::File::open(host,platform::FileAccess::create_new);
if(made){
// A pack that could not be made whole is removed, so nothing half made is ever opened.
const auto discard=[&]{
storage->file_.reset();
platform::discard_file(host);
};
std::expected<void,PackError> created;
try{
// Locked before anything is written: the file is new, so only a program racing for it could hold the lock.
if(auto locked=storage->lock_file(*made);!locked)created=std::unexpected(std::move(locked.error()));
else{
storage->file_=std::make_shared<platform::File>(std::move(*made));
created=storage->create_new(options,key);
}
}
catch(...){
discard();
throw;
}
if(!created){
// A file another program holds is not ours to remove.
if(created.error().in_use)return std::unexpected(std::move(created.error()));
discard();
return std::unexpected(std::move(created.error()));
}
// The new name reaches the device too (POSIX; nothing on Windows), else a crash could lose the whole new pack.
if(const std::error_code synced=platform::sync_directory(host.parent_path())){
discard();
return std::unexpected(system_failure(FileAction::write,synced));
}
return storage;
}
// Another process made it in between: open that one (once).
if(platform::error_kind(made.error())!=platform::ErrorKind::exists||attempt>0)return std::unexpected(system_failure(FileAction::write,made.error()));
}
storage->window_size_=storage->file_->size();
if(auto read=storage->read_existing(key);!read)return std::unexpected(std::move(read.error()));
return storage;
}

std::expected<void,PackError> PackStorage::create_new(const PackOptions& options,const KeySource& key)
{
header_.chunk_log2=options.chunk_log2;
if(std::holds_alternative<std::string>(key)){
header_.kdf=Kdf::argon2id;
header_.kdf_kib=options.kdf_kib;
header_.kdf_passes=options.kdf_passes;
}
else header_.kdf=Kdf::raw_key;
try{
crypto::random_bytes(std::as_writable_bytes(std::span(header_.salt)));
}
catch(const std::runtime_error&){
return std::unexpected(PackError{FileAction::write,Failure{FileError::io,{}}});
}
header_bytes_=encode_header(header_);
auto file_key=file_key_of(key,header_);
if(!file_key)return std::unexpected(file_key.error());
take_keys(*file_key);
chunk_size_=std::uint64_t{1}<<header_.chunk_log2;
window_size_=0;
if(auto written=file_->write_at(0,header_bytes_);!written)return std::unexpected(system_failure(FileAction::write,written.error()));
try{
// Durable as a flush (the header with it, by the first device flush).
const auto length=write_index(*file_,kHeaderSize,0,0,{});
if(!length)return std::unexpected(PackError{FileAction::write,Failure{length.error(),{}}});
index_offset_=kHeaderSize;
index_length_=*length;
end_=kHeaderSize+*length+kFooterSize;
}
catch(const std::runtime_error&){
return std::unexpected(PackError{FileAction::write,Failure{FileError::io,{}}});
}
return {};
}

std::expected<void,PackError> PackStorage::read_existing(const KeySource& key)
{
const std::uint64_t size=window_size_;
if(size<kMinPackSize)return std::unexpected(wrong_key());
if(const std::int64_t got=read_exact(*file_,base_,header_bytes_.data(),kHeaderSize);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
const auto header=decode_header(header_bytes_);
if(!header)return std::unexpected(header.error()==HeaderError::newer?newer_format():wrong_key());
header_=*header;
chunk_size_=std::uint64_t{1}<<header_.chunk_log2;
auto file_key=file_key_of(key,header_);
if(!file_key)return std::unexpected(file_key.error());
take_keys(*file_key);
FooterBytes last;
if(const std::int64_t got=read_exact(*file_,base_+size-kFooterSize,last.data(),kFooterSize);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
Footer footer;
switch(inspect_footer(last,size-kFooterSize,footer)){
case FooterState::complete:
return try_footer(size-kFooterSize,footer);
case FooterState::marked:
// A whole magic is written only once the rest of its footer is durable: this footer was changed. Refused, not recovered from.
return std::unexpected(wrong_key());
case FooterState::none:
break;
}
// Recovery: the newest whole magic inside the window, scanning backwards (one ending in a block's first bytes is found by the
// next block, which overlaps this one by the magic's size less one). Its footer must be complete and authenticate.
constexpr std::uint64_t lowest=kHeaderSize+kMinIndexLength;
const auto block=std::make_unique_for_overwrite<char[]>(kScanBlock);
std::uint64_t high=size;
while(high>lowest+kFooterMagicOffset){
const std::uint64_t low=std::max<std::uint64_t>(lowest,high>kScanBlock?high-kScanBlock:0);
const auto length=static_cast<std::size_t>(high-low);
if(const std::int64_t got=read_exact(*file_,base_+low,reinterpret_cast<std::byte*>(block.get()),length);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
const std::string_view text(block.get(),length);
const std::size_t at=text.rfind(kFooterMagic);
if(at!=std::string_view::npos){
const std::uint64_t magic=low+at;
// A magic too near the header to end a footer is no footer; nothing lower can be one either.
if(magic<lowest+kFooterMagicOffset)break;
const std::uint64_t start=magic-kFooterMagicOffset;
FooterBytes candidate;
if(const std::int64_t got=read_exact(*file_,base_+start,candidate.data(),kFooterSize);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
// A whole magic whose footer is not complete was changed: refused, never skipped for an older one.
if(inspect_footer(candidate,start,footer)!=FooterState::complete)return std::unexpected(wrong_key());
auto recovered=try_footer(start,footer);
if(!recovered)return recovered;
truncate_pending_=!read_only_&&end_<size;
return {};
}
if(low==lowest)break;
high=low+kFooterMagic.size()-1;
}
return std::unexpected(wrong_key());
}

// Opens the index footer points at (footer at footer_offset, both relative to base_) and takes it as the storage's state.
std::expected<void,PackError> PackStorage::try_footer(std::uint64_t footer_offset,const Footer& footer)
{
if(footer.index_length>crypto::kMaxSealBytes+kTagSize||footer.index_length>std::numeric_limits<std::size_t>::max())return std::unexpected(wrong_key());
const auto length=static_cast<std::size_t>(footer.index_length);
std::unique_ptr<std::byte[]> record;
try{
record=std::make_unique_for_overwrite<std::byte[]>(length);
}
catch(const std::bad_alloc&){
return std::unexpected(wrong_key());
}
if(const std::int64_t got=read_exact(*file_,base_+footer.index_offset,record.get(),length);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
const std::size_t plain_size=length-kTagSize;
crypto::Tag tag;
std::memcpy(tag.data(),record.get()+plain_size,kTagSize);
const std::span<std::byte> plain(record.get(),plain_size);
if(!crypto::open(index_key_,footer.nonce,header_bytes_,plain,tag,plain))return std::unexpected(wrong_key());
auto index=decode_index(plain);
if(!index)return std::unexpected(wrong_key());
std::vector<std::string> folds;
if(!validate_index(*index,header_.chunk_log2,footer.index_offset,&folds))return std::unexpected(wrong_key());
if(header_.flags!=0)return std::unexpected(newer_format());
entries_.reserve(index->entries.size());
used_ids_.reserve(index->entries.size());
files_=0;
for(std::size_t i=0;i<index->entries.size();++i){
IndexEntry& record_entry=index->entries[i];
Entry entry;
if(!record_entry.deleted){
entry.stored=*stored_size(record_entry.size,header_.chunk_log2);
used_ids_.insert(record_entry.id);
++files_;
}
entry.record=std::move(record_entry);
entries_.emplace(std::move(folds[i]),std::move(entry));
}
// Not shared yet: the folders are counted without the lock.
for(const auto& [fold,entry]:entries_){
const bool marker=entry.record.deleted;
each_folder(fold,[&](std::string_view folder){
auto slot=folders_.find(folder);
if(slot==folders_.end())slot=folders_.try_emplace(std::string(folder)).first;
++(marker?slot->second.markers:slot->second.files);
});
}
generation_=index->generation;
wasted_=index->wasted_bytes;
metadata_=std::move(index->metadata);
index_offset_=footer.index_offset;
index_length_=footer.index_length;
end_=footer_offset+kFooterSize;
return {};
}

std::expected<void,vfs::FileError> PackStorage::writable_writing() const
{
// closed_ changes only under writer_mutex_, which the caller holds.
if(closed_)return std::unexpected(FileError::io);
if(read_only_)return std::unexpected(FileError::access_denied);
return {};
}

std::expected<void,vfs::FileError> PackStorage::prepare_append_writing()
{
if(!truncate_pending_)return {};
if(auto cut=file_->truncate(end_);!cut)return std::unexpected(file_error(cut.error()));
// The cut reaches the device before anything is appended after it.
if(auto synced=file_->sync_data();!synced)return std::unexpected(file_error(synced.error()));
truncate_pending_=false;
return {};
}

// The nonce rule (pack_storage.hpp): an id is used for one entry's chunks once in the file's life. It is never 0 (the id of
// deletion markers) and never one this storage has seen (used_ids_ holds every id of the index it opened and every id it has
// handed out since, entries replaced, removed or compacted away and appends that failed included), so no (file key,
// chunk_nonce) pair repeats; ids it cannot know (lost in a crash, in older indexes) are avoided by the 64-bit random draw.
// crypto::random_bytes throws std::runtime_error when the entropy source fails.
std::uint64_t PackStorage::new_id_writing()
{
for(;;){
std::uint64_t id=0;
crypto::random_bytes(std::as_writable_bytes(std::span(&id,1)));
if(id!=0&&used_ids_.insert(id).second)return id;
}
}

void PackStorage::record_seal([[maybe_unused]] const crypto::Nonce& nonce,[[maybe_unused]] bool index) const
{
#if defined(ZODIAC_TEST_HOOKS)
const std::lock_guard lock(seal_mutex);
if(seal_log)seal_log->push_back(testing::SealRecord{index?index_tag_:key_tag_,nonce,index});
#endif
}

template<class Fill>
std::expected<IndexEntry,vfs::MoveError> PackStorage::append_entry(std::uint64_t size,Fill&& fill)
{
const auto stored=stored_size(size,header_.chunk_log2);
if(!stored||*stored>std::numeric_limits<std::uint64_t>::max()-end_)return std::unexpected(vfs::MoveError{FileError::io,true});
if(auto ready=prepare_append_writing();!ready)return std::unexpected(vfs::MoveError{ready.error(),true});
IndexEntry record;
try{
record.id=new_id_writing();
}
catch(const std::runtime_error&){
return std::unexpected(vfs::MoveError{FileError::io,true});
}
record.size=size;
record.offset=end_;
if(size==0)return record;
// Anything written before a failure (or a throw) lies past end_, so the next append truncates it first.
struct Pending{
bool& flag;
bool done=false;
~Pending(){if(!done)flag=true;}
} pending{truncate_pending_};
const std::size_t chunk=static_cast<std::size_t>(chunk_size_);
const std::size_t stored_chunk=chunk+kTagSize;
const std::uint64_t chunks=(size+chunk_size_-1)>>header_.chunk_log2;
const std::size_t batch=std::max<std::size_t>(1,kBatchBytes/stored_chunk);
const auto buffer_size=static_cast<std::size_t>(std::min<std::uint64_t>(*stored,std::uint64_t{batch}*stored_chunk));
const auto buffer=std::make_unique_for_overwrite<std::byte[]>(buffer_size);
for(std::uint64_t first=0;first<chunks;){
const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(batch,chunks-first));
std::size_t filled=0;
for(std::size_t i=0;i<count;++i){
const std::uint64_t index=first+i;
const std::uint64_t at=index<<header_.chunk_log2;
const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(chunk,size-at));
const std::span<std::byte> room(buffer.get()+filled,length);
const std::expected<std::span<const std::byte>,FileError> plain=fill(at,room);
if(!plain)return std::unexpected(vfs::MoveError{plain.error(),false});
const crypto::Nonce nonce=chunk_nonce(static_cast<std::uint32_t>(index),record.id);
crypto::Tag tag;
record_seal(nonce,false);
crypto::seal(file_key_,nonce,{},*plain,room,tag);
std::memcpy(buffer.get()+filled+length,tag.data(),kTagSize);
filled+=length+kTagSize;
}
if(auto written=write_chunks(*file_,end_+first*stored_chunk,{buffer.get(),filled});!written)return std::unexpected(vfs::MoveError{file_error(written.error()),true});
first+=count;
}
pending.done=true;
end_+=*stored;
return record;
}

PackStorage::FolderNodes PackStorage::new_folders_writing(std::string_view fold) const
{
FolderNodes nodes;
// folders_ changes only under writer_mutex_, which the caller holds, so it is read without the shared mutex.
Folders made;
each_folder(fold,[&](std::string_view folder){
if(folders_.find(folder)!=folders_.end())return;
nodes.push_back(made.extract(made.try_emplace(std::string(folder)).first));
});
return nodes;
}

void PackStorage::add_folders_locked(FolderNodes& nodes) noexcept
{
// The caller reserved room for them: no insert rehashes, so none allocates.
for(Folders::node_type& node:nodes)folders_.insert(std::move(node));
}

void PackStorage::count_locked(std::string_view fold,bool marker) noexcept
{
each_folder(fold,[&](std::string_view folder){
const auto slot=folders_.find(folder);
if(slot!=folders_.end())++(marker?slot->second.markers:slot->second.files);
});
}

void PackStorage::uncount_locked(std::string_view fold,bool marker) noexcept
{
each_folder(fold,[&](std::string_view folder){
const auto slot=folders_.find(folder);
if(slot==folders_.end())return;
--(marker?slot->second.markers:slot->second.files);
if(slot->second.files==0&&slot->second.markers==0)folders_.erase(slot);
});
}

vfs::EntryKind PackStorage::kind_locked(const std::string& fold,const Entry** entry) const
{
const auto found=entries_.find(fold);
if(found!=entries_.end()){
*entry=&found->second;
return found->second.record.deleted?vfs::EntryKind::deleted:vfs::EntryKind::file;
}
const auto folder=folders_.find(std::string_view(fold));
return folder!=folders_.end()&&folder->second.files?vfs::EntryKind::directory:vfs::EntryKind::none;
}

void PackStorage::insert_writing(std::string fold,IndexEntry record)
{
const std::uint64_t stored=record.deleted?0:*stored_size(record.size,header_.chunk_log2);
// Everything that allocates is made before the shared mutex is taken, so lookups wait only for the pointers to move: the
// folders the entry adds, and (for a new name) its node. entries_ changes only under writer_mutex_, held here.
FolderNodes folders=new_folders_writing(fold);
const auto existing=entries_.find(fold);
std::unordered_map<std::string,Entry>::node_type node;
if(existing==entries_.end()){
std::unordered_map<std::string,Entry> made;
node=made.extract(made.try_emplace(std::move(fold)).first);
}
const bool fresh=static_cast<bool>(node);
const bool marker=record.deleted;
// The entry replaced, freed after the shared mutex is released.
Entry previous;
{
const std::unique_lock lock(mutex_);
// The reserves are all that can throw, and they come before any change.
folders_.reserve(folders_.size()+folders.size());
if(fresh)entries_.reserve(entries_.size()+1);
add_folders_locked(folders);
const auto slot=fresh?entries_.insert(std::move(node)).position:existing;
// Counted in before the old entry is counted out, so a folder both hold never goes in between.
count_locked(slot->first,marker);
if(!fresh){
wasted_+=slot->second.stored;
if(!slot->second.record.deleted)--files_;
uncount_locked(slot->first,slot->second.record.deleted);
previous=std::move(slot->second);
}
if(!marker)++files_;
slot->second=Entry{std::move(record),stored};
}
dirty_=true;
}

std::expected<void,vfs::FileError> PackStorage::add(std::string_view name,std::span<const std::byte> bytes,std::int64_t modified_utc_ms)
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
const auto canonical=canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected(FileError::bad_path);
std::string fold=vfs::fold_name(name);
auto record=append_entry(bytes.size(),[&](std::uint64_t at,std::span<std::byte> room)->std::expected<std::span<const std::byte>,FileError>{
return bytes.subspan(static_cast<std::size_t>(at),room.size());
});
if(!record)return std::unexpected(record.error().code);
record->name.assign(name);
record->modified_utc_ms=modified_utc_ms;
insert_writing(std::move(fold),std::move(*record));
return {};
}

std::expected<void,vfs::MoveError> PackStorage::add_reader(std::string_view name,vfs::Reader& from,std::int64_t modified_utc_ms)
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(vfs::MoveError{writable.error(),true});
const auto canonical=canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected(vfs::MoveError{FileError::bad_path,true});
std::string fold=vfs::fold_name(name);
const std::uint64_t size=from.size();
auto record=append_entry(size,[&](std::uint64_t at,std::span<std::byte> room)->std::expected<std::span<const std::byte>,FileError>{
std::size_t done=0;
while(done<room.size()){
const std::int64_t got=from.read_at(at+done,room.subspan(done));
if(got<0)return std::unexpected(vfs::read_error(got));
// Shorter than when it was opened.
if(got==0)return std::unexpected(FileError::io);
done+=static_cast<std::size_t>(got);
}
return room;
});
if(!record)return std::unexpected(record.error());
record->name.assign(name);
record->modified_utc_ms=modified_utc_ms;
insert_writing(std::move(fold),std::move(*record));
return {};
}

std::expected<void,vfs::FileError> PackStorage::add_deletion_marker(std::string_view name)
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
const auto canonical=canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected(FileError::bad_path);
IndexEntry record;
record.name.assign(name);
record.deleted=true;
insert_writing(vfs::fold_name(name),std::move(record));
return {};
}

std::expected<bool,vfs::FileError> PackStorage::remove(std::string_view name)
{
const std::string fold=vfs::fold_name(name);
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
// entries_ changes only under writer_mutex_, held here, so it is searched without the shared mutex.
const auto found=entries_.find(fold);
if(found==entries_.end()||found->second.record.deleted)return false;
// Freed after the shared mutex is released.
std::unordered_map<std::string,Entry>::node_type removed;
{
const std::unique_lock lock(mutex_);
wasted_+=found->second.stored;
--files_;
uncount_locked(found->first,false);
removed=entries_.extract(found);
}
dirty_=true;
return true;
}

std::expected<vfs::EntryInfo,vfs::FileError> PackStorage::stat(std::string_view name) const
{
const std::string fold=vfs::fold_name(name);
vfs::EntryInfo info;
{
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
const Entry* entry=nullptr;
info.kind=kind_locked(fold,&entry);
if(info.kind==vfs::EntryKind::none)return std::unexpected(FileError::not_found);
if(entry){
info.size=entry->record.size;
info.modified_utc_ms=entry->record.modified_utc_ms;
}
}
const std::size_t slash=name.rfind('/');
info.name.assign(slash==std::string_view::npos?name:name.substr(slash+1));
return info;
}

std::optional<std::string> PackStorage::stored_name(std::string_view name) const
{
const std::string fold=vfs::fold_name(name);
const std::shared_lock lock(mutex_);
if(closed_)return std::nullopt;
const auto found=entries_.find(fold);
if(found==entries_.end()||found->second.record.deleted)return std::nullopt;
return found->second.record.name;
}

std::expected<void,vfs::FileError> PackStorage::list_folder(std::string_view folder,std::vector<vfs::EntryInfo>& out) const
{
const std::string fold=vfs::fold_name(folder);
const std::size_t depth=folder.empty()?0:static_cast<std::size_t>(std::ranges::count(folder,'/'))+1;
const std::size_t skip=folder.empty()?0:fold.size()+1;
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
bool markers_only=false;
if(!folder.empty()){
const auto counted=folders_.find(std::string_view(fold));
if(counted==folders_.end()){
const auto found=entries_.find(fold);
return std::unexpected(found!=entries_.end()&&!found->second.record.deleted?FileError::not_a_directory:FileError::not_found);
}
markers_only=counted->second.files==0;
}
// The folders below, by fold (views of entries_' keys, which stay put while the lock is held), and their place in out.
std::unordered_map<std::string_view,std::size_t> folders;
for(const auto& [key,entry]:entries_){
if(skip&&!below(key,fold))continue;
const std::string_view rest=std::string_view(key).substr(skip);
const std::string_view name=segments_after(entry.record.name,depth);
const std::size_t slash=rest.find('/');
if(slash==std::string_view::npos){
vfs::EntryInfo& info=out.emplace_back();
info.kind=entry.record.deleted?vfs::EntryKind::deleted:vfs::EntryKind::file;
info.size=entry.record.size;
info.modified_utc_ms=entry.record.modified_utc_ms;
info.name.assign(name);
info.fold.assign(rest);
continue;
}
// A folder with no file below it (markers only) is not listed.
const auto counted=folders_.find(std::string_view(key).substr(0,skip+slash));
if(counted==folders_.end()||counted->second.files==0)continue;
const std::string_view child=rest.substr(0,slash);
const std::string_view spelling=name.substr(0,name.find('/'));
const auto [slot,inserted]=folders.try_emplace(child,out.size());
if(inserted){
vfs::EntryInfo& info=out.emplace_back();
info.kind=vfs::EntryKind::directory;
info.name.assign(spelling);
info.fold.assign(child);
}
else if(spelling<out[slot->second].name)out[slot->second].name.assign(spelling);
}
if(markers_only)return std::unexpected(FileError::not_found);
return {};
}

std::expected<void,vfs::MoveError> PackStorage::rename(std::string_view from,std::string_view to)
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(vfs::MoveError{writable.error(),false});
const auto canonical=canonical_entry_name(to);
if(!canonical||*canonical!=to)return std::unexpected(vfs::MoveError{FileError::bad_path,true});
const std::string from_fold=vfs::fold_name(from);
std::string to_fold=vfs::fold_name(to);
std::string name(to);
// entries_ and folders_ change only under writer_mutex_, held here, so they are searched without the shared mutex.
const auto found=entries_.find(from_fold);
if(found==entries_.end()||found->second.record.deleted)return std::unexpected(vfs::MoveError{FileError::not_found,false});
if(to_fold==from_fold){
// Another spelling of the same name.
{
const std::unique_lock lock(mutex_);
found->second.record.name.swap(name);
}
dirty_=true;
return {};
}
if(const auto folder=folders_.find(std::string_view(to_fold));folder!=folders_.end()&&folder->second.files)return std::unexpected(vfs::MoveError{FileError::is_directory,true});
FolderNodes folders=new_folders_writing(to_fold);
std::unordered_map<std::string,Entry>::node_type moved;
// The file to replaced, freed after the shared mutex is released.
std::unordered_map<std::string,Entry>::node_type replaced;
{
const std::unique_lock lock(mutex_);
// The reserve is all that can throw, and it comes before any change. entries_ never grows here: one node leaves before it
// (or another) goes back.
folders_.reserve(folders_.size()+folders.size());
add_folders_locked(folders);
count_locked(to_fold,false);
uncount_locked(from_fold,false);
moved=entries_.extract(found);
if(const auto old=entries_.find(to_fold);old!=entries_.end()){
wasted_+=old->second.stored;
if(!old->second.record.deleted)--files_;
uncount_locked(old->first,old->second.record.deleted);
replaced=entries_.extract(old);
}
moved.key().swap(to_fold);
moved.mapped().record.name.swap(name);
entries_.insert(std::move(moved));
}
dirty_=true;
return {};
}

std::expected<std::size_t,vfs::FileError> PackStorage::remove_folder(std::string_view folder)
{
const std::string fold=vfs::fold_name(folder);
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
// entries_ and folders_ change only under writer_mutex_, held here: the files are found without the shared mutex, which
// is taken only to take them out.
const auto counted=folders_.find(std::string_view(fold));
if(counted==folders_.end()||counted->second.files==0)return std::unexpected(FileError::not_found);
std::vector<std::unordered_map<std::string,Entry>::iterator> doomed;
doomed.reserve(counted->second.files);
for(auto it=entries_.begin();it!=entries_.end();++it)if(!it->second.record.deleted&&below(it->first,fold))doomed.push_back(it);
std::vector<std::unordered_map<std::string,Entry>::node_type> removed;
removed.reserve(doomed.size());
{
const std::unique_lock lock(mutex_);
for(const auto it:doomed){
wasted_+=it->second.stored;
--files_;
uncount_locked(it->first,false);
removed.push_back(entries_.extract(it));
}
}
dirty_=true;
return removed.size();
}

std::vector<std::string> PackStorage::list() const
{
std::vector<std::string> names;
{
const std::shared_lock lock(mutex_);
if(closed_)return names;
names.reserve(files_);
for(const auto& [fold,entry]:entries_)if(!entry.record.deleted)names.push_back(entry.record.name);
}
// char_traits<char> compares as unsigned char: by bytes.
std::sort(names.begin(),names.end());
return names;
}

std::size_t PackStorage::count() const
{
const std::shared_lock lock(mutex_);
return closed_?0:files_;
}

std::uint64_t PackStorage::wasted_bytes() const
{
const std::shared_lock lock(mutex_);
return wasted_;
}

std::expected<std::shared_ptr<vfs::Reader>,vfs::FileError> PackStorage::open_reader(std::string_view name) const
{
const std::string fold=vfs::fold_name(name);
std::shared_ptr<platform::File> file;
IndexEntry record;
{
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
const auto found=entries_.find(fold);
if(found==entries_.end()||found->second.record.deleted)return std::unexpected(FileError::not_found);
file=file_;
record.offset=found->second.record.offset;
record.size=found->second.record.size;
record.id=found->second.record.id;
}
// Made outside the lock: the snapshot (file, offset, size, id) is all it needs.
return std::make_shared<PackReader>(std::move(file),base_+record.offset,record.size,record.id,header_.chunk_log2,file_key_,nullptr);
}

std::expected<std::shared_ptr<vfs::Reader>,vfs::FileError> PackStorage::open_mounted(std::string_view name,std::shared_ptr<const void> holder) const
{
const std::string fold=vfs::fold_name(name);
std::shared_ptr<platform::File> file;
std::uint64_t offset=0;
std::uint64_t size=0;
std::uint64_t id=0;
{
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
const Entry* entry=nullptr;
switch(kind_locked(fold,&entry)){
case vfs::EntryKind::file:
break;
case vfs::EntryKind::deleted:
return std::shared_ptr<vfs::Reader>();
case vfs::EntryKind::directory:
return std::unexpected(FileError::is_directory);
default:
return std::unexpected(FileError::not_found);
}
file=file_;
offset=entry->record.offset;
size=entry->record.size;
id=entry->record.id;
}
return std::make_shared<PackReader>(std::move(file),base_+offset,size,id,header_.chunk_log2,file_key_,std::move(holder));
}

std::expected<std::uint64_t,vfs::FileError> PackStorage::write_index(platform::File& file,std::uint64_t at,std::uint64_t generation,std::uint64_t wasted,const std::vector<const IndexEntry*>& entries)
{
if(metadata_.size()>std::numeric_limits<std::uint32_t>::max()||entries.size()>std::numeric_limits<std::uint32_t>::max())return std::unexpected(FileError::io);
std::uint64_t plain_size=index_head_size(metadata_);
for(const IndexEntry* entry:entries)plain_size+=entry_size(*entry);
if(plain_size>crypto::kMaxSealBytes||plain_size>std::numeric_limits<std::size_t>::max()-kTagSize-kFooterSize)return std::unexpected(FileError::io);
const auto plain_bytes=static_cast<std::size_t>(plain_size);
const auto buffer=std::make_unique_for_overwrite<std::byte[]>(plain_bytes+kTagSize+kFooterSize);
std::byte* out=write_index_head(buffer.get(),generation,wasted,metadata_,static_cast<std::uint32_t>(entries.size()));
for(const IndexEntry* entry:entries)out=write_entry(out,*entry);
// A fresh random nonce for every index, never derived from the generation (the nonce rule), under the index key.
Footer footer;
crypto::random_bytes(std::as_writable_bytes(std::span(footer.nonce)));
record_seal(footer.nonce,true);
const std::span<std::byte> plain(buffer.get(),plain_bytes);
crypto::Tag tag;
crypto::seal(index_key_,footer.nonce,header_bytes_,plain,plain,tag);
std::memcpy(buffer.get()+plain_bytes,tag.data(),kTagSize);
footer.index_offset=at;
footer.index_length=plain_size+kTagSize;
const FooterBytes encoded=encode_footer(footer);
std::memcpy(buffer.get()+plain_bytes+kTagSize,encoded.data(),kFooterSize);
// The Durability rule: everything but the magic, a device flush (which takes every chunk written before with it), then the
// magic and another flush. Until the second flush completes no whole magic can be on the device.
const std::size_t body=plain_bytes+kTagSize+kFooterMagicOffset;
if(auto written=file.write_at(at,{buffer.get(),body});!written)return std::unexpected(file_error(written.error()));
if(auto synced=file.sync_data();!synced)return std::unexpected(file_error(synced.error()));
if(auto written=file.write_at(at+body,{buffer.get()+body,kFooterMagic.size()});!written)return std::unexpected(file_error(written.error()));
if(auto synced=file.sync_data();!synced)return std::unexpected(file_error(synced.error()));
return footer.index_length;
}

std::expected<void,vfs::FileError> PackStorage::flush_writing()
{
if(!dirty_)return {};
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
if(auto ready=prepare_append_writing();!ready)return std::unexpected(ready.error());
const std::uint64_t generation=generation_+1;
const std::uint64_t wasted=wasted_+index_length_+kFooterSize;
// entries_ changes only under writer_mutex_, held here, so it is read without the shared mutex.
std::vector<const IndexEntry*> records;
records.reserve(entries_.size());
for(const auto& [fold,entry]:entries_)records.push_back(&entry.record);
truncate_pending_=true;
std::expected<std::uint64_t,FileError> length;
try{
length=write_index(*file_,end_,generation,wasted,records);
}
catch(const std::runtime_error&){
return std::unexpected(FileError::io);
}
if(!length)return std::unexpected(length.error());
truncate_pending_=false;
generation_=generation;
index_offset_=end_;
index_length_=*length;
end_+=*length+kFooterSize;
dirty_=false;
const std::unique_lock lock(mutex_);
wasted_=wasted;
return {};
}

std::expected<void,vfs::FileError> PackStorage::flush()
{
const std::lock_guard writing(writer_mutex_);
if(closed_)return std::unexpected(FileError::io);
return flush_writing();
}

std::expected<void,vfs::FileError> PackStorage::compact()
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
try{
return compact_writing();
}
catch(const std::runtime_error&){
return std::unexpected(FileError::io);
}
}

std::expected<void,vfs::FileError> PackStorage::compact_writing()
{
std::filesystem::path temp=host_;
temp+=".compact.tmp";
// A leftover of a compact that crashed is ours to replace.
platform::remove_file(temp);
auto opened=platform::File::open(temp,platform::FileAccess::create_private);
if(!opened)return std::unexpected(file_error(opened.error()));
// The new file is locked before it takes the pack's name, so no other program can open it for writing in between.
if(const auto locked=opened->lock_exclusive();locked&&!*locked){
opened->close();
platform::discard_file(temp);
return std::unexpected(FileError::access_denied);
}
// Allocated before the replace, so nothing after it can fail but the folder's flush.
auto next=std::make_shared<platform::File>(std::move(*opened));
struct Discard{
const std::filesystem::path& temp;
platform::File& file;
bool done=false;
~Discard()
{
if(done)return;
file.close();
platform::discard_file(temp);
}
} discard{temp,*next};
if(auto written=next->write_at(0,header_bytes_);!written)return std::unexpected(file_error(written.error()));
std::unordered_map<std::string,Entry> fresh;
fresh.reserve(entries_.size());
std::vector<const IndexEntry*> records;
records.reserve(entries_.size());
const std::size_t chunk=static_cast<std::size_t>(chunk_size_);
const std::size_t stored_chunk=chunk+kTagSize;
const std::size_t batch=std::max<std::size_t>(1,kBatchBytes/stored_chunk);
std::uint64_t largest=0;
for(const auto& [fold,entry]:entries_)largest=std::max(largest,entry.stored);
std::unique_ptr<std::byte[]> buffer;
if(largest)buffer=std::make_unique_for_overwrite<std::byte[]>(static_cast<std::size_t>(std::min<std::uint64_t>(largest,std::uint64_t{batch}*stored_chunk)));
std::uint64_t position=kHeaderSize;
// entries_ and file_ change only under writer_mutex_, held here: lookups go on reading them (and the old file) meanwhile.
for(const auto& [fold,entry]:entries_){
Entry copy;
copy.record=entry.record;
copy.stored=entry.stored;
if(!entry.record.deleted){
const IndexEntry& old=entry.record;
// A new id: the old file's chunks keep theirs, and the file key stays (the nonce rule).
copy.record.id=new_id_writing();
copy.record.offset=position;
const std::uint64_t chunks=(old.size+chunk_size_-1)>>header_.chunk_log2;
for(std::uint64_t first=0;first<chunks;){
const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(batch,chunks-first));
const std::uint64_t last_end=std::min<std::uint64_t>(old.size,(first+count)<<header_.chunk_log2);
const auto bytes=static_cast<std::size_t>(last_end-(first<<header_.chunk_log2)+count*kTagSize);
if(const std::int64_t got=read_exact(*file_,base_+old.offset+first*stored_chunk,buffer.get(),bytes);got!=0)return std::unexpected(got<0?read_failure(got):FileError::damaged);
std::size_t at=0;
for(std::size_t i=0;i<count;++i){
const std::uint64_t index=first+i;
const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(chunk,old.size-(index<<header_.chunk_log2)));
const std::span<std::byte> data(buffer.get()+at,length);
crypto::Tag tag;
std::memcpy(tag.data(),buffer.get()+at+length,kTagSize);
if(!crypto::open(file_key_,chunk_nonce(static_cast<std::uint32_t>(index),old.id),{},data,tag,data))return std::unexpected(FileError::damaged);
const crypto::Nonce nonce=chunk_nonce(static_cast<std::uint32_t>(index),copy.record.id);
record_seal(nonce,false);
crypto::seal(file_key_,nonce,{},data,data,tag);
std::memcpy(buffer.get()+at+length,tag.data(),kTagSize);
at+=length+kTagSize;
}
if(auto written=write_chunks(*next,position+first*stored_chunk,{buffer.get(),bytes});!written)return std::unexpected(file_error(written.error()));
first+=count;
}
position+=entry.stored;
}
const auto slot=fresh.emplace(fold,std::move(copy)).first;
records.push_back(&slot->second.record);
}
// Durable before the replace (write_index flushes twice).
const auto length=write_index(*next,position,generation_+1,0,records);
if(!length)return std::unexpected(length.error());
if(const std::error_code moded=next->copy_mode(host_))return std::unexpected(file_error(moded));
std::error_code error;
if(!platform::replace_file(temp,host_,error))return std::unexpected(file_error(error));
discard.done=true;
// From here the new file is the pack's: nothing below can fail but the folder's flush, reported after the switch.
++generation_;
index_offset_=position;
index_length_=*length;
end_=position+*length+kFooterSize;
dirty_=false;
truncate_pending_=false;
{
const std::unique_lock lock(mutex_);
file_.swap(next);
entries_.swap(fresh);
wasted_=0;
}
// The old file (next now) stays open for its readers but is no longer written: its lock goes. The old entries (fresh now) are
// freed outside the lock.
next->unlock();
next.reset();
fresh.clear();
if(const std::error_code synced=platform::sync_directory(host_.parent_path()))return std::unexpected(file_error(synced));
return {};
}

std::string PackStorage::metadata() const
{
const std::shared_lock lock(mutex_);
return metadata_;
}

std::expected<void,vfs::FileError> PackStorage::set_metadata(std::string metadata)
{
const std::lock_guard writing(writer_mutex_);
if(auto writable=writable_writing();!writable)return std::unexpected(writable.error());
if(metadata.size()>std::numeric_limits<std::uint32_t>::max())return std::unexpected(FileError::io);
{
const std::unique_lock lock(mutex_);
metadata_.swap(metadata);
}
dirty_=true;
return {};
}

std::expected<void,vfs::FileError> PackStorage::close()
{
std::expected<void,FileError> result;
{
const std::lock_guard writing(writer_mutex_);
if(closed_)return {};
try{
result=flush_writing();
}
catch(const std::bad_alloc&){
result=std::unexpected(FileError::io);
}
std::unordered_map<std::string,Entry> dropped;
Folders dropped_folders;
// The storage lets go of the file here; readers may keep it open, but not its lock, which is released below.
std::shared_ptr<platform::File> file;
{
const std::unique_lock lock(mutex_);
closed_=true;
entries_.swap(dropped);
folders_.swap(dropped_folders);
files_=0;
file_.swap(file);
}
// Readers may keep the file open a long time; the lock covers only the storage's writable life, so it goes now.
if(file&&!read_only_)file->unlock();
}
unregister();
return result;
}

bool PackStorage::is_open() const
{
const std::shared_lock lock(mutex_);
return !closed_;
}

void PackStorage::set_log(std::string name,Log log)
{
// Its own mutex, so an open never waits for a write (a compact) in progress.
const std::lock_guard guard(log_mutex_);
if(log_)return;
log_name_=std::move(name);
log_=std::move(log);
}

void PackStorage::unregister() noexcept
{
Registry& shared=registry();
{
const std::lock_guard lock(shared.mutex);
if(!registered_)return;
const auto found=shared.slots.find(KeyView{registry_path_,windowed_?base_:0,windowed_?window_size_:0,windowed_});
if(found!=shared.slots.end()&&found->second.raw==this)shared.slots.erase(found);
registered_=false;
}
shared.changed.notify_all();
}

#if defined(ZODIAC_TEST_HOOKS)
void testing::pack_record_seals(std::vector<SealRecord>* log) noexcept
{
const std::lock_guard lock(seal_mutex);
seal_log=log;
}

void testing::pack_fail_chunk_write(std::uint64_t after) noexcept
{
writes_before_failure.store(after);
}
#endif
}
