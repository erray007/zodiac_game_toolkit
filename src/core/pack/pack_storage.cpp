// PackStorage (contract in pack_storage.hpp): the registry, opening and recovery, the chunked writer and reader, flush and
// compact. Every write happens with no window (a window is read only), so a writable storage's offsets are file offsets.
#include <core/pack/pack_storage.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/path.hpp>
#include <monocypher.h>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
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
};
using PathText=std::filesystem::path::string_type;
struct RegistryKey{
PathText path;
std::uint64_t offset=0;
};
// A key as unregister looks it up, so that the lookup allocates nothing.
struct KeyView{
std::basic_string_view<std::filesystem::path::value_type> path;
std::uint64_t offset=0;
};
KeyView view_of(const RegistryKey& key) noexcept
{
return {key.path,key.offset};
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
return x.path<y.path||(x.path==y.path&&x.offset<y.offset);
}
};
struct Registry{
std::mutex mutex;
// Signalled when a slot is erased: an open waiting for a storage being freed to finish closing.
std::condition_variable erased;
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
#endif

// Reads one entry of a pack file from the snapshot open_reader took.
class PackReader final:public vfs::Reader{
public:
PackReader(std::shared_ptr<platform::File> file,std::uint64_t offset,std::uint64_t size,std::uint64_t id,std::uint8_t chunk_log2,const crypto::Key& key) noexcept
:file_(std::move(file)),offset_(offset),size_(size),id_(id),key_(key),chunk_size_(std::size_t{1}<<chunk_log2),chunk_log2_(chunk_log2){}
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
RegistryKey slot_key{canonical.native(),window?window->offset:0};
Registry& shared=registry();
std::unique_lock lock(shared.mutex);
for(;;){
const auto found=shared.slots.find(slot_key);
if(found==shared.slots.end())break;
std::shared_ptr<PackStorage> existing=found->second.storage.lock();
// A storage closing (closed, not yet out of the registry) is waited for like one being freed. Its lock is taken under the
// registry's: close never holds both (it releases its own before it leaves the registry).
if(existing&&existing->is_open()){
lock.unlock();
// header_ and file_key_ never change once a storage is loaded, so they are read without its lock.
auto file_key=file_key_of(key,existing->header_);
if(!file_key)return std::unexpected(file_key.error());
const bool same=crypto_verify32(file_key->data(),existing->file_key_.data())==0;
crypto::wipe(file_key->data(),file_key->size());
if(!same)return std::unexpected(wrong_key());
return existing;
}
// The storage is being freed or closed: wait until its close has written its last index and left the registry. The copy is
// dropped first, so the last release never happens under the registry's lock (a release locks it).
existing.reset();
shared.erased.wait(lock);
}
// The slot is taken before anything is loaded and filled only once the storage is shared: a storage that fails on the way is
// never registered, so freeing it never locks the registry, which this thread holds.
const auto slot=shared.slots.try_emplace(slot_key).first;
try{
auto loaded=load(canonical,key,create,options,window);
if(!loaded){
shared.slots.erase(slot);
shared.erased.notify_all();
return std::unexpected(std::move(loaded.error()));
}
(*loaded)->registry_path_=std::move(slot_key.path);
std::shared_ptr<PackStorage> storage=share(std::move(*loaded));
slot->second=Slot{storage,storage.get()};
storage->registered_=true;
return storage;
}
catch(...){
shared.slots.erase(slot);
shared.erased.notify_all();
throw;
}
}

std::shared_ptr<PackStorage> PackStorage::share(std::unique_ptr<PackStorage> storage)
{
// The last holder closes the storage (flushing it) and reports a failure there is no one else to tell. A storage that is not
// registered yet (shared_ptr's own allocation failing) only frees itself: it has nothing to write.
return std::shared_ptr<PackStorage>(storage.release(),[](PackStorage* released) noexcept{
try{
if(auto closed=released->close();!closed&&released->log_){
std::string line="error: cannot write \"";
line.append(one_line(released->log_name_)).append("\": ").append(vfs::reason_text(closed.error()));
released->log_(line);
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
storage->file_=std::make_shared<platform::File>(std::move(*made));
created=storage->create_new(options,key);
}
catch(...){
discard();
throw;
}
if(!created){
discard();
return std::unexpected(std::move(created.error()));
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
file_key_=*file_key;
crypto::wipe(file_key->data(),file_key->size());
#if defined(ZODIAC_TEST_HOOKS)
key_tag_=crypto::hash(std::as_bytes(std::span(file_key_)));
#endif
chunk_size_=std::uint64_t{1}<<header_.chunk_log2;
window_size_=0;
const auto failed=[](const std::error_code& error){return std::unexpected(system_failure(FileAction::write,error));};
if(auto written=file_->write_at(0,header_bytes_);!written)return failed(written.error());
try{
const auto length=write_index(*file_,kHeaderSize,0,0,{});
if(!length)return std::unexpected(PackError{FileAction::write,Failure{length.error(),{}}});
index_offset_=kHeaderSize;
index_length_=*length;
end_=kHeaderSize+*length+kFooterSize;
}
catch(const std::runtime_error&){
return std::unexpected(PackError{FileAction::write,Failure{FileError::io,{}}});
}
if(auto synced=file_->sync_data();!synced)return failed(synced.error());
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
file_key_=*file_key;
crypto::wipe(file_key->data(),file_key->size());
#if defined(ZODIAC_TEST_HOOKS)
key_tag_=crypto::hash(std::as_bytes(std::span(file_key_)));
#endif
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
// A footer that was changed, never one an interrupted append left: refused, not recovered from.
return std::unexpected(wrong_key());
case FooterState::none:
break;
}
// Recovery: the newest complete footer inside the window, scanning backwards for its magic (one footer ending in the block's
// first bytes is found by the next block, which overlaps this one by the magic's size less one).
constexpr std::uint64_t lowest=kHeaderSize+kMinIndexLength;
const auto block=std::make_unique_for_overwrite<char[]>(kScanBlock);
std::uint64_t high=size;
while(high>lowest+kFooterSize-kFooterMagic.size()){
const std::uint64_t low=std::max<std::uint64_t>(lowest,high>kScanBlock?high-kScanBlock:0);
const auto length=static_cast<std::size_t>(high-low);
if(const std::int64_t got=read_exact(*file_,base_+low,reinterpret_cast<std::byte*>(block.get()),length);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
const std::string_view text(block.get(),length);
for(std::size_t at=text.rfind(kFooterMagic);at!=std::string_view::npos;at=at==0?std::string_view::npos:text.rfind(kFooterMagic,at-1)){
// The footer's magic is its last 8 bytes.
const std::uint64_t magic=low+at;
if(magic<lowest+kFooterSize-kFooterMagic.size())break;
const std::uint64_t start=magic+kFooterMagic.size()-kFooterSize;
FooterBytes candidate;
if(const std::int64_t got=read_exact(*file_,base_+start,candidate.data(),kFooterSize);got!=0){
if(got<0)return std::unexpected(PackError{FileAction::read,Failure{read_failure(got),{}}});
return std::unexpected(wrong_key());
}
if(inspect_footer(candidate,start,footer)!=FooterState::complete)continue;
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
if(!crypto::open(file_key_,footer.nonce,header_bytes_,plain,tag,plain))return std::unexpected(wrong_key());
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
generation_=index->generation;
wasted_=index->wasted_bytes;
metadata_=std::move(index->metadata);
index_offset_=footer.index_offset;
index_length_=footer.index_length;
end_=footer_offset+kFooterSize;
return {};
}

std::expected<void,vfs::FileError> PackStorage::writable_locked() const
{
if(closed_)return std::unexpected(FileError::io);
if(read_only_)return std::unexpected(FileError::access_denied);
return {};
}

std::expected<void,vfs::FileError> PackStorage::prepare_append_locked()
{
if(!truncate_pending_)return {};
if(auto cut=file_->truncate(end_);!cut)return std::unexpected(file_error(cut.error()));
truncate_pending_=false;
return {};
}

// The nonce rule (pack_storage.hpp): an id is used for one entry's chunks once in the file's life. It is never 0 (the id of
// deletion markers) and never one this storage has seen (used_ids_ holds every id of the index it opened and every id it has
// handed out since, entries replaced, removed or compacted away included), so no (file key, chunk_nonce) pair repeats; ids it
// cannot know (lost in a crash, in older indexes) are avoided by the 64-bit random draw. crypto::random_bytes throws
// std::runtime_error when the entropy source fails.
std::uint64_t PackStorage::new_id_locked()
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
if(seal_log)seal_log->push_back(testing::SealRecord{key_tag_,nonce,index});
#endif
}

template<class Fill>
std::expected<IndexEntry,vfs::MoveError> PackStorage::append_entry(std::uint64_t size,Fill&& fill)
{
const auto stored=stored_size(size,header_.chunk_log2);
if(!stored||*stored>std::numeric_limits<std::uint64_t>::max()-end_)return std::unexpected(vfs::MoveError{FileError::io,true});
if(auto ready=prepare_append_locked();!ready)return std::unexpected(vfs::MoveError{ready.error(),true});
IndexEntry record;
try{
record.id=new_id_locked();
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
if(auto written=file_->write_at(end_+first*stored_chunk,{buffer.get(),filled});!written)return std::unexpected(vfs::MoveError{file_error(written.error()),true});
first+=count;
}
pending.done=true;
end_+=*stored;
return record;
}

void PackStorage::insert_locked(std::string fold,IndexEntry record)
{
const std::uint64_t stored=record.deleted?0:*stored_size(record.size,header_.chunk_log2);
const auto [slot,inserted]=entries_.try_emplace(std::move(fold));
if(!inserted){
wasted_+=slot->second.stored;
if(!slot->second.record.deleted)--files_;
}
if(!record.deleted)++files_;
slot->second=Entry{std::move(record),stored};
dirty_=true;
}

std::expected<void,vfs::FileError> PackStorage::add(std::string_view name,std::span<const std::byte> bytes,std::int64_t modified_utc_ms)
{
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
const auto canonical=canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected(FileError::bad_path);
std::string fold=vfs::fold_name(name);
auto record=append_entry(bytes.size(),[&](std::uint64_t at,std::span<std::byte> room)->std::expected<std::span<const std::byte>,FileError>{
return bytes.subspan(static_cast<std::size_t>(at),room.size());
});
if(!record)return std::unexpected(record.error().code);
record->name.assign(name);
record->modified_utc_ms=modified_utc_ms;
insert_locked(std::move(fold),std::move(*record));
return {};
}

std::expected<void,vfs::MoveError> PackStorage::add_reader(std::string_view name,vfs::Reader& from,std::int64_t modified_utc_ms)
{
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(vfs::MoveError{writable.error(),true});
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
insert_locked(std::move(fold),std::move(*record));
return {};
}

std::expected<void,vfs::FileError> PackStorage::add_deletion_marker(std::string_view name)
{
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
const auto canonical=canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected(FileError::bad_path);
IndexEntry record;
record.name.assign(name);
record.deleted=true;
insert_locked(vfs::fold_name(name),std::move(record));
return {};
}

std::expected<bool,vfs::FileError> PackStorage::remove(std::string_view name)
{
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
const auto found=entries_.find(vfs::fold_name(name));
if(found==entries_.end())return false;
wasted_+=found->second.stored;
if(!found->second.record.deleted)--files_;
entries_.erase(found);
dirty_=true;
return true;
}

std::expected<vfs::EntryInfo,vfs::FileError> PackStorage::stat(std::string_view name) const
{
const std::string fold=vfs::fold_name(name);
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
const auto found=entries_.find(fold);
if(found==entries_.end())return std::unexpected(FileError::not_found);
const IndexEntry& record=found->second.record;
vfs::EntryInfo info;
info.kind=record.deleted?vfs::EntryKind::deleted:vfs::EntryKind::file;
info.size=record.size;
info.modified_utc_ms=record.modified_utc_ms;
const std::size_t slash=name.rfind('/');
info.name.assign(slash==std::string_view::npos?name:name.substr(slash+1));
return info;
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
const std::shared_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::not_found);
const auto found=entries_.find(fold);
if(found==entries_.end()||found->second.record.deleted)return std::unexpected(FileError::not_found);
const IndexEntry& record=found->second.record;
return std::make_shared<PackReader>(file_,base_+record.offset,record.size,record.id,header_.chunk_log2,file_key_);
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
// A fresh random nonce for every index, never derived from the generation (the nonce rule).
Footer footer;
crypto::random_bytes(std::as_writable_bytes(std::span(footer.nonce)));
record_seal(footer.nonce,true);
const std::span<std::byte> plain(buffer.get(),plain_bytes);
crypto::Tag tag;
crypto::seal(file_key_,footer.nonce,header_bytes_,plain,plain,tag);
std::memcpy(buffer.get()+plain_bytes,tag.data(),kTagSize);
footer.index_offset=at;
footer.index_length=plain_size+kTagSize;
const FooterBytes encoded=encode_footer(footer);
std::memcpy(buffer.get()+plain_bytes+kTagSize,encoded.data(),kFooterSize);
if(auto written=file.write_at(at,{buffer.get(),plain_bytes+kTagSize+kFooterSize});!written)return std::unexpected(file_error(written.error()));
return footer.index_length;
}

std::expected<void,vfs::FileError> PackStorage::flush_locked()
{
if(!dirty_)return {};
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
if(auto ready=prepare_append_locked();!ready)return std::unexpected(ready.error());
const std::uint64_t generation=generation_+1;
const std::uint64_t wasted=wasted_+index_length_+kFooterSize;
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
if(auto synced=file_->sync_data();!synced)return std::unexpected(file_error(synced.error()));
truncate_pending_=false;
generation_=generation;
wasted_=wasted;
index_offset_=end_;
index_length_=*length;
end_+=*length+kFooterSize;
dirty_=false;
return {};
}

std::expected<void,vfs::FileError> PackStorage::flush()
{
const std::unique_lock lock(mutex_);
if(closed_)return std::unexpected(FileError::io);
return flush_locked();
}

std::expected<void,vfs::FileError> PackStorage::compact()
{
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
try{
return compact_locked();
}
catch(const std::runtime_error&){
return std::unexpected(FileError::io);
}
}

std::expected<void,vfs::FileError> PackStorage::compact_locked()
{
std::filesystem::path temp=host_;
temp+=".compact.tmp";
// A leftover of a compact that crashed is ours to replace.
platform::remove_file(temp);
auto opened=platform::File::open(temp,platform::FileAccess::create_private);
if(!opened)return std::unexpected(file_error(opened.error()));
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
for(const auto& [fold,entry]:entries_){
Entry copy;
copy.record=entry.record;
copy.stored=entry.stored;
if(!entry.record.deleted){
const IndexEntry& old=entry.record;
// A new id: the old file's chunks keep theirs, and the file key stays (the nonce rule).
copy.record.id=new_id_locked();
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
if(auto written=next->write_at(position+first*stored_chunk,{buffer.get(),bytes});!written)return std::unexpected(file_error(written.error()));
first+=count;
}
position+=entry.stored;
}
const auto slot=fresh.emplace(fold,std::move(copy)).first;
records.push_back(&slot->second.record);
}
const auto length=write_index(*next,position,generation_+1,0,records);
if(!length)return std::unexpected(length.error());
if(auto synced=next->sync_data();!synced)return std::unexpected(file_error(synced.error()));
if(const std::error_code moded=next->copy_mode(host_))return std::unexpected(file_error(moded));
std::error_code error;
if(!platform::replace_file(temp,host_,error))return std::unexpected(file_error(error));
discard.done=true;
// From here the new file is the pack's: nothing below can fail but the folder's flush, reported after the switch.
file_=std::move(next);
entries_.swap(fresh);
++generation_;
wasted_=0;
index_offset_=position;
index_length_=*length;
end_=position+*length+kFooterSize;
dirty_=false;
truncate_pending_=false;
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
const std::unique_lock lock(mutex_);
if(auto writable=writable_locked();!writable)return std::unexpected(writable.error());
if(metadata.size()>std::numeric_limits<std::uint32_t>::max())return std::unexpected(FileError::io);
metadata_=std::move(metadata);
dirty_=true;
return {};
}

std::expected<void,vfs::FileError> PackStorage::close()
{
std::expected<void,FileError> result;
{
const std::unique_lock lock(mutex_);
if(closed_)return {};
try{
result=flush_locked();
}
catch(const std::bad_alloc&){
result=std::unexpected(FileError::io);
}
closed_=true;
entries_.clear();
files_=0;
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
const std::unique_lock lock(mutex_);
if(log_)return;
log_name_=std::move(name);
log_=std::move(log);
}

void PackStorage::unregister() noexcept
{
if(!registered_)return;
Registry& shared=registry();
{
const std::lock_guard lock(shared.mutex);
const auto found=shared.slots.find(KeyView{registry_path_,base_});
if(found!=shared.slots.end()&&found->second.raw==this)shared.slots.erase(found);
registered_=false;
}
shared.erased.notify_all();
}

#if defined(ZODIAC_TEST_HOOKS)
void testing::pack_record_seals(std::vector<SealRecord>* log) noexcept
{
const std::lock_guard lock(seal_mutex);
seal_log=log;
}
#endif
}
