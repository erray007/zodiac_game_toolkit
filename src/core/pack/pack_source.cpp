// The pack source and its write target (contract in pack_source.hpp).
#include <core/pack/pack_source.hpp>
#include <platform/clock.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace zodiac::pack {
namespace {
using vfs::EntryKind;
using vfs::FileError;
using vfs::WriteMode;

// A file being written through a mount: its bytes in memory, stored as one entry by PackStorage::add.
class PackWriteTarget final:public vfs::WriteTarget{
public:
PackWriteTarget(std::shared_ptr<PackStorage> storage,std::string name,bool changed) noexcept:storage_(std::move(storage)),name_(std::move(name)),changed_(changed){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override;
std::expected<void,FileError> write_at(std::uint64_t offset,std::span<const std::byte> bytes) override;
std::uint64_t size() const override{return size_;}
std::expected<void,FileError> truncate(std::uint64_t size) override;
std::expected<void,FileError> commit() override{return store();}
std::expected<void,FileError> sync() override;
// Starts the file with every byte of reader (the entry an append or an update continues).
std::expected<void,FileError> load(vfs::Reader& reader);
private:
// Makes room for size bytes (std::bad_alloc when memory runs out), at least doubling, so a file written in pieces is copied
// a bounded number of times.
void reserve(std::size_t size);
// Stores the bytes as the entry when they changed since they were last stored.
std::expected<void,FileError> store();
std::shared_ptr<PackStorage> storage_;
std::string name_;
std::unique_ptr<std::byte[]> bytes_;
std::size_t size_=0;
std::size_t capacity_=0;
bool changed_;
};

std::int64_t PackWriteTarget::read_at(std::uint64_t offset,std::span<std::byte> buffer)
{
if(offset>=size_||buffer.empty())return 0;
const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size_-offset));
std::memcpy(buffer.data(),bytes_.get()+offset,count);
return static_cast<std::int64_t>(count);
}

void PackWriteTarget::reserve(std::size_t size)
{
if(size<=capacity_)return;
const std::size_t grown=capacity_>std::numeric_limits<std::size_t>::max()/2?std::numeric_limits<std::size_t>::max():capacity_*2;
const std::size_t capacity=std::max({size,grown,std::size_t{4096}});
auto bytes=std::make_unique_for_overwrite<std::byte[]>(capacity);
if(size_)std::memcpy(bytes.get(),bytes_.get(),size_);
bytes_=std::move(bytes);
capacity_=capacity;
}

std::expected<void,FileError> PackWriteTarget::write_at(std::uint64_t offset,std::span<const std::byte> bytes)
{
if(bytes.empty())return {};
if(offset>std::numeric_limits<std::size_t>::max()-bytes.size())return std::unexpected(FileError::io);
const auto at=static_cast<std::size_t>(offset);
const std::size_t end=at+bytes.size();
reserve(end);
// A gap past the end reads as zeros, as in a file.
if(at>size_)std::memset(bytes_.get()+size_,0,at-size_);
std::memcpy(bytes_.get()+at,bytes.data(),bytes.size());
size_=std::max(size_,end);
changed_=true;
return {};
}

std::expected<void,FileError> PackWriteTarget::truncate(std::uint64_t size)
{
if(size>std::numeric_limits<std::size_t>::max())return std::unexpected(FileError::io);
const auto length=static_cast<std::size_t>(size);
if(length>size_){
reserve(length);
std::memset(bytes_.get()+size_,0,length-size_);
}
size_=length;
changed_=true;
return {};
}

std::expected<void,FileError> PackWriteTarget::store()
{
if(!changed_)return {};
if(auto added=storage_->add(name_,{bytes_.get(),size_},platform::utc_now_ms());!added)return std::unexpected(added.error());
changed_=false;
return {};
}

std::expected<void,FileError> PackWriteTarget::sync()
{
// The bytes reach the device only as an entry of the pack: what has been written so far is stored, then the index flushed.
if(auto stored=store();!stored)return stored;
return storage_->flush();
}

std::expected<void,FileError> PackWriteTarget::load(vfs::Reader& reader)
{
const std::uint64_t size=reader.size();
if(size>std::numeric_limits<std::size_t>::max())return std::unexpected(FileError::io);
reserve(static_cast<std::size_t>(size));
std::size_t done=0;
while(done<size){
const std::int64_t got=reader.read_at(done,{bytes_.get()+done,static_cast<std::size_t>(size)-done});
if(got<0)return std::unexpected(vfs::read_error(got));
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
size_=done;
return {};
}

class PackSource final:public vfs::Source{
public:
PackSource(std::shared_ptr<PackStorage> storage,bool writable) noexcept:storage_(std::move(storage)),read_only_(!writable){}
std::expected<vfs::EntryInfo,FileError> stat(std::string_view relative) override;
std::expected<std::shared_ptr<vfs::Reader>,FileError> open_read(std::string_view relative) override;
std::expected<std::unique_ptr<vfs::WriteTarget>,FileError> open_write(std::string_view relative,WriteMode mode) override;
std::expected<void,FileError> list(std::string_view relative,std::vector<vfs::EntryInfo>& out) override;
std::expected<void,FileError> remove(std::string_view relative) override;
std::expected<void,FileError> make_directory(std::string_view relative) override;
std::expected<void,FileError> remove_directory(std::string_view relative,bool recursive) override;
std::expected<bool,vfs::MoveError> rename(std::string_view from,std::string_view to) override;
bool read_only() const noexcept override{return read_only_;}
bool real_spelling(std::string_view relative,std::string& real) override;
private:
std::shared_ptr<PackStorage> storage_;
bool read_only_;
};

std::expected<vfs::EntryInfo,FileError> PackSource::stat(std::string_view relative)
{
if(!relative.empty())return storage_->stat(relative);
if(!storage_->is_open())return std::unexpected(FileError::not_found);
vfs::EntryInfo info;
info.kind=EntryKind::directory;
return info;
}

std::expected<std::shared_ptr<vfs::Reader>,FileError> PackSource::open_read(std::string_view relative)
{
if(relative.empty())return std::unexpected(storage_->is_open()?FileError::is_directory:FileError::not_found);
return storage_->open_mounted(relative,storage_);
}

std::expected<std::unique_ptr<vfs::WriteTarget>,FileError> PackSource::open_write(std::string_view relative,WriteMode mode)
{
if(read_only_)return std::unexpected(FileError::access_denied);
if(relative.empty())return std::unexpected(FileError::is_directory);
std::optional<std::string> name=storage_->stored_name(relative);
if(!name){
if(const auto info=storage_->stat(relative);info&&info->kind==EntryKind::directory)return std::unexpected(FileError::is_directory);
if(mode==WriteMode::update)return std::unexpected(FileError::not_found);
return std::make_unique<PackWriteTarget>(storage_,std::string(relative),true);
}
const bool truncating=mode==WriteMode::truncate;
auto target=std::make_unique<PackWriteTarget>(storage_,*name,truncating);
if(!truncating){
const auto reader=storage_->open_reader(*name);
if(!reader)return std::unexpected(reader.error());
if(auto loaded=target->load(**reader);!loaded)return std::unexpected(loaded.error());
}
return target;
}

std::expected<void,FileError> PackSource::list(std::string_view relative,std::vector<vfs::EntryInfo>& out)
{
return storage_->list_folder(relative,out);
}

std::expected<void,FileError> PackSource::remove(std::string_view relative)
{
if(read_only_)return std::unexpected(FileError::access_denied);
const auto removed=storage_->remove(relative);
if(!removed)return std::unexpected(removed.error());
if(*removed)return {};
const auto info=storage_->stat(relative);
return std::unexpected(info&&info->kind==EntryKind::directory?FileError::is_directory:FileError::not_found);
}

std::expected<void,FileError> PackSource::make_directory(std::string_view relative)
{
if(read_only_)return std::unexpected(FileError::access_denied);
if(relative.empty())return {};
if(const auto info=storage_->stat(relative);info&&info->kind==EntryKind::file)return std::unexpected(FileError::not_a_directory);
return {};
}

std::expected<void,FileError> PackSource::remove_directory(std::string_view relative,bool recursive)
{
if(read_only_||relative.empty())return std::unexpected(FileError::access_denied);
const auto info=storage_->stat(relative);
if(!info||info->kind==EntryKind::deleted)return std::unexpected(FileError::not_found);
if(info->kind!=EntryKind::directory)return std::unexpected(FileError::not_a_directory);
// A folder of a pack is never empty: it is there only for the files in it.
if(!recursive)return std::unexpected(FileError::not_empty);
if(auto removed=storage_->remove_folder(relative);!removed)return std::unexpected(removed.error());
return {};
}

std::expected<bool,vfs::MoveError> PackSource::rename(std::string_view from,std::string_view to)
{
if(read_only_)return std::unexpected(vfs::MoveError{FileError::access_denied,false});
if(auto renamed=storage_->rename(from,to);!renamed)return std::unexpected(renamed.error());
return true;
}

bool PackSource::real_spelling(std::string_view relative,std::string& real)
{
std::optional<std::string> name=storage_->stored_name(relative);
if(!name||*name==relative)return false;
real=std::move(*name);
return true;
}
}

std::shared_ptr<vfs::Source> make_pack_source(std::shared_ptr<PackStorage> storage,bool writable)
{
if(!storage)return nullptr;
return std::make_shared<PackSource>(std::move(storage),writable);
}
}
