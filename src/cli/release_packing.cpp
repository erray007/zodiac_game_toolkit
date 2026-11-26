// zodiac build's release pack, its manifest and the embedded layout (contract in cli/release_packing.hpp).
#include <cli/release_packing.hpp>
#include <core/pack/pack_format.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/pack/release_pack.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace zodiac::cli {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t kCopyPiece=std::size_t{1}<<20;
constexpr std::string_view kManifestName=".zodiac/release.manifest";
constexpr std::string_view kManifestMagic="zodiac-release-manifest 1";

std::string relative_text(const fs::path& path,const fs::path& root)
{
const std::u8string text=path.lexically_relative(root).generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
std::string in_quotes(std::string_view text)
{
std::string result(1,'"');
result.append(one_line(text)).push_back('"');
return result;
}
std::string cannot(std::string_view verb,std::string_view name,std::string_view reason)
{
std::string line="error: cannot ";
line.append(verb).push_back(' ');
line.append(in_quotes(name)).append(": ").append(reason);
return line;
}
std::string cannot(std::string_view verb,const fs::path& path,const std::error_code& error)
{
return cannot(verb,path_to_utf8(path),one_line(error_text(error)));
}
std::string cannot(std::string_view verb,const fs::path& path,vfs::FileError error)
{
return cannot(verb,path_to_utf8(path),vfs::reason_text(error));
}
std::error_code errno_code(std::int64_t negated) noexcept
{
return std::error_code(static_cast<int>(-negated),std::generic_category());
}
template<std::size_t N>
void append_hex(std::string& out,const std::array<std::uint8_t,N>& bytes)
{
constexpr char digits[]="0123456789abcdef";
for(const std::uint8_t byte:bytes){
out.push_back(digits[byte>>4]);
out.push_back(digits[byte&0xFu]);
}
}

// An asset read by PackStorage::add_reader and hashed on the way: add_reader reads every byte once, in order, so the hash is the
// file's. A read out of order (never made) fails rather than give a wrong hash. At most size bytes are read, the size the entry
// was given; a file that shrinks meanwhile ends early, which add_reader reports.
class HashingReader final:public vfs::Reader{
public:
HashingReader(const platform::File& file,std::uint64_t size) noexcept:file_(file),size_(size){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override
{
if(offset!=next_)return -static_cast<std::int64_t>(vfs::FileError::io);
if(offset>=size_||buffer.empty())return 0;
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size_-offset));
const std::int64_t got=file_.read_at(offset,buffer.first(wanted));
if(got<0){
error_=errno_code(got);
return -static_cast<std::int64_t>(vfs::detail::file_error(error_));
}
hasher_.update(buffer.first(static_cast<std::size_t>(got)));
next_+=static_cast<std::uint64_t>(got);
return got;
}
std::uint64_t size() const override{return size_;}
std::array<std::uint8_t,32> digest() noexcept{return hasher_.digest();}
// The system's reason for a failed read, when there was one.
const std::error_code& error() const noexcept{return error_;}
private:
const platform::File& file_;
std::uint64_t size_;
std::uint64_t next_=0;
crypto::Hasher hasher_;
std::error_code error_;
};

struct ManifestLine{
std::string path;
std::array<std::uint8_t,32> hash;
std::uint64_t size;
};

fs::path manifest_path(const Project& project)
{
return project.root/".zodiac"/"release.manifest";
}
fs::path manifest_temporary(const Project& project)
{
return project.root/".zodiac"/"release.manifest.tmp";
}

std::string damaged_manifest()
{
return std::string("error: ")+std::string(kManifestName)+" is damaged; restore it from your backup or delete it";
}

// The next patch number an earlier manifest records, 1 without one.
std::expected<std::uint64_t,std::string> next_patch(const Project& project)
{
auto file=platform::File::open(manifest_path(project),platform::FileAccess::read);
if(!file){
if(platform::error_kind(file.error())==platform::ErrorKind::not_found)return 1;
return std::unexpected(cannot("read",kManifestName,one_line(error_text(file.error()))));
}
// The first three lines are short (the magic, "build " and 32 digits, "next_patch " and a number): this much holds them.
std::array<char,256> head;
std::size_t size=0;
while(size<head.size()){
const std::int64_t got=file->read_at(size,std::as_writable_bytes(std::span(head)).subspan(size));
if(got<0)return std::unexpected(cannot("read",kManifestName,one_line(error_text(errno_code(got)))));
if(got==0)break;
size+=static_cast<std::size_t>(got);
}
std::string_view text(head.data(),size);
const auto take_line=[&text]()->std::optional<std::string_view>{
const std::size_t end=text.find('\n');
if(end==std::string_view::npos)return std::nullopt;
const std::string_view line=text.substr(0,end);
text.remove_prefix(end+1);
return line;
};
const auto magic=take_line();
const auto build=take_line();
const auto patch=take_line();
if(!magic||*magic!=kManifestMagic||!build||!build->starts_with("build ")||build->size()!=6+32||!patch||!patch->starts_with("next_patch "))return std::unexpected(damaged_manifest());
if(!std::ranges::all_of(build->substr(6),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))return std::unexpected(damaged_manifest());
const std::string_view number=patch->substr(11);
std::uint64_t value=0;
const auto [end,error]=std::from_chars(number.data(),number.data()+number.size(),value);
if(error!=std::errc()||end!=number.data()+number.size()||value==0||number.front()=='0')return std::unexpected(damaged_manifest());
return value;
}

// Writes the manifest's temporary file in 1 MiB pieces and flushes it.
std::expected<void,std::string> write_manifest(const Project& project,std::string_view build_id,std::uint64_t patch,std::vector<ManifestLine>& lines)
{
std::ranges::sort(lines,{},&ManifestLine::path);
const fs::path temporary=manifest_temporary(project);
auto file=platform::File::open(temporary,platform::FileAccess::create_truncate);
if(!file)return std::unexpected(cannot("write",kManifestName,one_line(error_text(file.error()))));
std::string piece;
piece.reserve(kCopyPiece+4096);
std::uint64_t at=0;
const auto flush=[&]()->std::expected<void,std::string>{
if(auto written=file->write_at(at,std::as_bytes(std::span(piece)));!written)return std::unexpected(cannot("write",kManifestName,one_line(error_text(written.error()))));
at+=piece.size();
piece.clear();
return {};
};
piece.append(kManifestMagic).append("\nbuild ").append(build_id).append("\nnext_patch ").append(std::to_string(patch)).push_back('\n');
for(const ManifestLine& line:lines){
append_hex(piece,line.hash);
piece.push_back(' ');
piece.append(std::to_string(line.size)).push_back(' ');
piece.append(line.path).push_back('\n');
if(piece.size()>=kCopyPiece){
if(auto flushed=flush();!flushed)return flushed;
}
}
if(auto flushed=flush();!flushed)return flushed;
if(auto synced=file->sync_data();!synced)return std::unexpected(cannot("write",kManifestName,one_line(error_text(synced.error()))));
return {};
}
}

AssetSplit split_assets(const Project& project,const std::vector<fs::path>& assets)
{
AssetSplit split;
for(const fs::path& asset:assets){
const std::string relative=relative_text(asset,project.root);
(packed_in_release(project.pack,relative)?split.packed:split.plain).push_back(asset);
}
return split;
}

std::expected<void,std::string> write_release_pack(const Project& project,const crypto::Key& key,const AssetSplit& split,const fs::path& pack_file,std::string& build_id)
{
const auto patch=next_patch(project);
if(!patch)return std::unexpected(patch.error());
std::array<std::uint8_t,16> id;
try{
crypto::random_bytes(std::as_writable_bytes(std::span(id)));
}
catch(const std::runtime_error&){
return std::unexpected(cannot("write",path_to_utf8(pack_file),"the entropy source failed"));
}
build_id.clear();
append_hex(build_id,id);
platform::discard_file(pack_file);
auto storage=pack::PackStorage::open(pack_file,pack::KeySource(key),true);
if(!storage)return std::unexpected("error: "+vfs::failure_text(storage.error().action,path_to_utf8(pack_file),storage.error().failure));
std::vector<ManifestLine> lines;
lines.reserve(split.packed.size());
for(const fs::path& asset:split.packed){
std::string name=relative_text(asset,project.root);
const auto canonical=pack::canonical_entry_name(name);
if(!canonical||*canonical!=name)return std::unexpected("error: "+in_quotes(name)+" is not a valid file name");
if(const auto other=(*storage)->stored_name(name)){
const bool first=*other<name;
return std::unexpected("error: "+in_quotes(name)+" matches both "+in_quotes(first?*other:name)+" and "+in_quotes(first?name:*other)+"; rename one of them");
}
auto file=platform::File::open(asset,platform::FileAccess::read);
if(!file)return std::unexpected(cannot("read",asset,file.error()));
const auto info=platform::stat_path(asset);
if(!info)return std::unexpected(cannot("read",asset,info.error()));
HashingReader reader(*file,file->size());
if(auto added=(*storage)->add_reader(name,reader,vfs::detail::to_ms(info->modified_utc_ns));!added){
if(added.error().to)return std::unexpected(cannot("write",pack_file,added.error().code));
if(reader.error())return std::unexpected(cannot("read",asset,reader.error()));
return std::unexpected(cannot("read",asset,added.error().code));
}
lines.push_back({std::move(name),reader.digest(),reader.size()});
}
if(auto set=(*storage)->set_metadata("build="+build_id);!set)return std::unexpected(cannot("write",pack_file,set.error()));
if(auto closed=(*storage)->close();!closed)return std::unexpected(cannot("write",pack_file,closed.error()));
storage->reset();
return write_manifest(project,build_id,*patch,lines);
}

std::expected<void,std::string> append_pack(const fs::path& executable,const fs::path& pack_file,std::uint32_t payload_crc)
{
auto out=platform::File::open(executable,platform::FileAccess::read_write);
if(!out)return std::unexpected(cannot("write",executable,out.error()));
auto in=platform::File::open(pack_file,platform::FileAccess::read);
if(!in)return std::unexpected(cannot("read",pack_file,in.error()));
const std::uint64_t offset=out->size();
const std::uint64_t size=in->size();
const auto buffer=std::make_unique_for_overwrite<std::byte[]>(kCopyPiece);
for(std::uint64_t done=0;done<size;){
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(kCopyPiece,size-done));
const std::int64_t got=in->read_at(done,{buffer.get(),wanted});
if(got<0)return std::unexpected(cannot("read",pack_file,errno_code(got)));
if(got==0)return std::unexpected(cannot("read",pack_file,vfs::FileError::io));
if(auto written=out->write_at(offset+done,{buffer.get(),static_cast<std::size_t>(got)});!written)return std::unexpected(cannot("write",executable,written.error()));
done+=static_cast<std::uint64_t>(got);
}
// Zero bytes so the trailer ends on a multiple of 8: a signing tool then needs no alignment of its own before its table.
const std::uint64_t end=offset+size+pack::kTrailerSize;
const std::size_t padding=static_cast<std::size_t>((pack::kTrailerAlignment-end%pack::kTrailerAlignment)%pack::kTrailerAlignment);
std::string tail(padding,'\0');
tail.append(pack::trailer_bytes(offset,size,payload_crc));
if(auto written=out->write_at(offset+size,std::as_bytes(std::span(tail)));!written)return std::unexpected(cannot("write",executable,written.error()));
if(auto synced=out->sync_data();!synced)return std::unexpected(cannot("write",executable,synced.error()));
return {};
}

std::expected<void,std::string> commit_release_manifest(const Project& project)
{
std::error_code error;
if(!platform::replace_file(manifest_temporary(project),manifest_path(project),error))return std::unexpected(cannot("write",kManifestName,one_line(error_text(error))));
if(const std::error_code synced=platform::sync_directory(project.root/".zodiac"))return std::unexpected(cannot("write",kManifestName,one_line(error_text(synced))));
return {};
}

void discard_release_manifest(const Project& project) noexcept
{
try{
platform::discard_file(manifest_temporary(project));
}
catch(...){
}
}
}
