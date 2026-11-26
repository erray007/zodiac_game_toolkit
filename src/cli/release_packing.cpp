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
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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

bool is_hex(std::string_view text) noexcept
{
return std::ranges::all_of(text,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}

// A decimal number without a leading zero (0 itself only when allow_zero), or nullopt.
std::optional<std::uint64_t> decimal(std::string_view text,bool allow_zero) noexcept
{
if(text.empty()||(text.front()=='0'&&(text.size()>1||!allow_zero)))return std::nullopt;
std::uint64_t value=0;
const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
if(error!=std::errc()||end!=text.data()+text.size())return std::nullopt;
return value;
}

// The manifest's first three lines: the build id and the next patch number, or nullopt when they are not as written.
struct ManifestHead{
std::string build_id;
std::uint64_t next_patch;
};
std::optional<ManifestHead> parse_head(std::optional<std::string_view> magic,std::optional<std::string_view> build,std::optional<std::string_view> patch)
{
if(!magic||*magic!=kManifestMagic||!build||!build->starts_with("build ")||build->size()!=6+32||!patch||!patch->starts_with("next_patch "))return std::nullopt;
if(!is_hex(build->substr(6)))return std::nullopt;
const auto number=decimal(patch->substr(11),false);
if(!number)return std::nullopt;
return ManifestHead{std::string(build->substr(6)),*number};
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
const auto parsed=parse_head(magic,build,patch);
if(!parsed)return std::unexpected(damaged_manifest());
return parsed->next_patch;
}

// A deletion line: the path, and whether the newest patch gives it a marker (else an entry).
struct DeletedLine{
std::string path;
bool marker;
};

// Writes the manifest's temporary file in 1 MiB pieces and flushes it: the head, the release's lines, then (after a patch) the
// deletion lines and the newest patch's entries, each kind sorted by path bytes.
std::expected<void,std::string> write_manifest(const Project& project,std::string_view build_id,std::uint64_t patch,std::vector<ManifestLine>& lines,std::vector<DeletedLine>&& deleted={},std::vector<ManifestLine>&& patched={})
{
std::ranges::sort(lines,{},&ManifestLine::path);
std::ranges::sort(deleted,{},&DeletedLine::path);
std::ranges::sort(patched,{},&ManifestLine::path);
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
const auto entry=[&piece](const ManifestLine& line){
append_hex(piece,line.hash);
piece.push_back(' ');
piece.append(std::to_string(line.size)).push_back(' ');
piece.append(line.path).push_back('\n');
};
piece.append(kManifestMagic).append("\nbuild ").append(build_id).append("\nnext_patch ").append(std::to_string(patch)).push_back('\n');
for(const ManifestLine& line:lines){
entry(line);
if(piece.size()>=kCopyPiece)if(auto flushed=flush();!flushed)return flushed;
}
for(const DeletedLine& line:deleted){
piece.append(line.marker?"deleted marker ":"deleted entry ").append(line.path).push_back('\n');
if(piece.size()>=kCopyPiece)if(auto flushed=flush();!flushed)return flushed;
}
for(const ManifestLine& line:patched){
piece.append("patched ");
entry(line);
if(piece.size()>=kCopyPiece)if(auto flushed=flush();!flushed)return flushed;
}
if(auto flushed=flush();!flushed)return flushed;
if(auto synced=file->sync_data();!synced)return std::unexpected(cannot("write",kManifestName,one_line(error_text(synced.error()))));
return {};
}

// Reads a file line by line through one 1 MiB buffer. A line is valid until the next call.
class LineReader{
public:
explicit LineReader(const platform::File& file):file_(file),buffer_(std::make_unique_for_overwrite<char[]>(kCopyPiece)){}
// The next line without its "\n"; nullopt at the end. A last line without "\n", or one longer than the buffer (no manifest line
// comes near it), sets damaged; a failed read sets error.
std::optional<std::string_view> next(std::error_code& error,bool& damaged)
{
for(;;){
const std::string_view held(buffer_.get()+start_,end_-start_);
if(const std::size_t cut=held.find('\n');cut!=std::string_view::npos){
start_+=cut+1;
return held.substr(0,cut);
}
if(done_){
damaged=!held.empty();
return std::nullopt;
}
// The part of a line read so far moves to the front, and the rest of the buffer is filled after it.
if(start_>0){
std::memmove(buffer_.get(),held.data(),held.size());
start_=0;
end_=held.size();
}
if(end_==kCopyPiece){
damaged=true;
return std::nullopt;
}
const std::int64_t got=file_.read_at(at_,std::as_writable_bytes(std::span(buffer_.get()+end_,kCopyPiece-end_)));
if(got<0){
error=errno_code(got);
return std::nullopt;
}
if(got==0)done_=true;
at_+=static_cast<std::uint64_t>(got);
end_+=static_cast<std::size_t>(got);
}
}
private:
const platform::File& file_;
std::unique_ptr<char[]> buffer_;
std::size_t start_=0;
std::size_t end_=0;
std::uint64_t at_=0;
bool done_=false;
};

// `<64 hex digits> <size> <path>`, path a canonical entry name.
std::optional<ManifestLine> parse_entry(std::string_view line)
{
if(line.size()<64+4||line[64]!=' ')return std::nullopt;
const std::string_view hex=line.substr(0,64);
if(!is_hex(hex))return std::nullopt;
line.remove_prefix(65);
const std::size_t space=line.find(' ');
if(space==std::string_view::npos)return std::nullopt;
const auto size=decimal(line.substr(0,space),true);
const std::string_view path=line.substr(space+1);
const auto canonical=pack::canonical_entry_name(path);
if(!size||!canonical||*canonical!=path)return std::nullopt;
ManifestLine entry{std::string(path),{},*size};
for(std::size_t i=0;i<entry.hash.size();++i){
const auto digit=[](char c){return static_cast<std::uint8_t>(c<='9'?c-'0':c-'a'+10);};
entry.hash[i]=static_cast<std::uint8_t>(digit(hex[2*i])<<4|digit(hex[2*i+1]));
}
return entry;
}

// What the manifest knows about one path (by fold): its release entry, and the state the newest patch gives it.
enum class PatchState{none,marker,entry};
struct Known{
std::optional<ManifestLine> release;
PatchState newest=PatchState::none;
// The newest patch's entry (state entry), or only its path (state marker: as its deleted line spells it).
ManifestLine patched{};
// A patch has given it a marker once (it has a deleted line).
bool ever_marked=false;
};
struct Manifest{
ManifestHead head;
// By fold_name.
std::unordered_map<std::string,Known> known;
};

// The whole manifest, every line checked (the damaged text otherwise).
std::expected<Manifest,std::string> read_manifest(const Project& project)
{
auto file=platform::File::open(manifest_path(project),platform::FileAccess::read);
if(!file){
if(platform::error_kind(file.error())==platform::ErrorKind::not_found)return std::unexpected(std::string("error: no release manifest in .zodiac; build a full release first"));
return std::unexpected(cannot("read",kManifestName,one_line(error_text(file.error()))));
}
LineReader reader(*file);
std::error_code error;
bool damaged=false;
const auto take=[&]()->std::optional<std::string>{
const auto line=reader.next(error,damaged);
if(!line)return std::nullopt;
return std::string(*line);
};
const auto failed=[&]()->std::unexpected<std::string>{
if(error)return std::unexpected(cannot("read",kManifestName,one_line(error_text(error))));
return std::unexpected(damaged_manifest());
};
const auto magic=take();
const auto build=take();
const auto patch=take();
const auto head=parse_head(magic,build,patch);
if(!head)return failed();
Manifest manifest{*head,{}};
// A deleted entry line needs a patched line; checked once every line is read.
std::size_t deleted_entries=0;
std::size_t patched_with_deleted=0;
for(;;){
const auto line=reader.next(error,damaged);
if(!line)break;
const auto known_of=[&](std::string_view path)->Known&{return manifest.known[vfs::fold_name(path)];};
if(line->starts_with("deleted ")){
std::string_view rest=line->substr(8);
bool marker=false;
if(rest.starts_with("marker "))marker=true;
else if(!rest.starts_with("entry "))return failed();
rest.remove_prefix(marker?7:6);
const auto canonical=pack::canonical_entry_name(rest);
if(!canonical||*canonical!=rest)return failed();
Known& known=known_of(rest);
if(known.ever_marked)return failed();
known.ever_marked=true;
if(marker){
if(known.newest!=PatchState::none)return failed();
known.newest=PatchState::marker;
known.patched.path.assign(rest);
}
else{
++deleted_entries;
if(known.newest==PatchState::marker)return failed();
if(known.newest==PatchState::entry)++patched_with_deleted;
}
continue;
}
if(line->starts_with("patched ")){
auto entry=parse_entry(line->substr(8));
if(!entry)return failed();
Known& known=known_of(entry->path);
if(known.newest!=PatchState::none)return failed();
known.newest=PatchState::entry;
if(known.ever_marked)++patched_with_deleted;
known.patched=std::move(*entry);
continue;
}
auto entry=parse_entry(*line);
if(!entry)return failed();
Known& known=known_of(entry->path);
if(known.release)return failed();
known.release=std::move(entry);
}
if(error||damaged||deleted_entries!=patched_with_deleted)return failed();
return manifest;
}

// The BLAKE2b-256 of the file at path, streamed through buffer (kCopyPiece bytes); size is the size it must have.
std::expected<std::array<std::uint8_t,32>,std::string> hash_file(const fs::path& path,std::uint64_t size,std::byte* buffer)
{
auto file=platform::File::open(path,platform::FileAccess::read);
if(!file)return std::unexpected(cannot("read",path,file.error()));
crypto::Hasher hasher;
for(std::uint64_t done=0;done<size;){
const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(kCopyPiece,size-done));
const std::int64_t got=file->read_at(done,{buffer,wanted});
if(got<0)return std::unexpected(cannot("read",path,errno_code(got)));
// Shorter than its size a moment ago: changing while it is read.
if(got==0)return std::unexpected(cannot("read",path,vfs::FileError::io));
hasher.update(std::span<const std::byte>(buffer,static_cast<std::size_t>(got)));
done+=static_cast<std::uint64_t>(got);
}
return hasher.digest();
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

std::expected<std::string,std::string> new_build_id()
{
std::array<std::uint8_t,16> id;
try{
crypto::random_bytes(std::as_writable_bytes(std::span(id)));
}
catch(const std::runtime_error&){
return std::unexpected(std::string("error: cannot make a build id: the entropy source failed"));
}
std::string text;
append_hex(text,id);
return text;
}

std::expected<void,std::string> write_release_pack(const Project& project,const crypto::Key& key,const AssetSplit& split,const fs::path& pack_file,const std::string& build_id)
{
const auto patch=next_patch(project);
if(!patch)return std::unexpected(patch.error());
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

namespace {
// True when path lies below dir; both absolute and normalized, each element compared as the file system does.
bool inside(const fs::path& path,const fs::path& dir)
{
auto element=path.begin();
for(const fs::path& part:dir){
if(element==path.end()||!same_path_text(*element,part))return false;
++element;
}
return element!=path.end();
}

// A packed asset of the project now: its project-relative path, its file and size, and its hash once one was needed.
struct Current{
std::string path;
fs::path file;
std::uint64_t size=0;
std::optional<std::array<std::uint8_t,32>> hash;
};

// One path the patch holds: an entry (current set) or a marker spelled as path.
struct Emitted{
std::string path;
const Current* current=nullptr;
};

// Removes a temporary file on every way out but a kept one.
class TemporaryPack{
public:
explicit TemporaryPack(fs::path path):path_(std::move(path)){}
~TemporaryPack(){if(!kept_)platform::discard_file(path_);}
TemporaryPack(const TemporaryPack&)=delete;
TemporaryPack& operator=(const TemporaryPack&)=delete;
const fs::path& path() const noexcept{return path_;}
void kept() noexcept{kept_=true;}
private:
fs::path path_;
bool kept_=false;
};
}

std::expected<PatchResult,std::string> write_patch(const Project& project,const crypto::Key& key,const fs::path& output)
{
if(!project.pack.enabled)return std::unexpected(std::string("error: --patch needs [pack] enabled"));
auto manifest=read_manifest(project);
if(!manifest)return std::unexpected(std::move(manifest.error()));
const std::uint64_t number=manifest->head.next_patch;
// PatchResult::number is an int; no release ever comes near this many patches.
if(number>=static_cast<std::uint64_t>(std::numeric_limits<int>::max()))return std::unexpected(damaged_manifest());
auto expanded=expand_assets(project);
if(!expanded)return std::unexpected(std::move(expanded.error()));
// An output folder inside the project never feeds a patch's assets (as for a full build).
std::erase_if(*expanded,[&output](const fs::path& asset){return inside(asset,output);});
// The current assets by fold: every declared one, and the packed ones with their files.
std::unordered_map<std::string,Current> packed;
packed.reserve(expanded->size());
std::unordered_set<std::string> declared;
declared.reserve(expanded->size());
for(const fs::path& asset:*expanded){
std::string relative=relative_text(asset,project.root);
std::string fold=vfs::fold_name(relative);
declared.insert(fold);
if(!packed_in_release(project.pack,relative))continue;
const auto canonical=pack::canonical_entry_name(relative);
if(!canonical||*canonical!=relative)return std::unexpected("error: "+in_quotes(relative)+" is not a valid file name");
const auto info=platform::stat_path(asset);
if(!info)return std::unexpected(cannot("read",asset,info.error()));
const auto [slot,inserted]=packed.try_emplace(std::move(fold));
if(!inserted){
const bool first=slot->second.path<relative;
return std::unexpected("error: "+in_quotes(relative)+" matches both "+in_quotes(first?slot->second.path:relative)+" and "+in_quotes(first?relative:slot->second.path)+"; rename one of them");
}
slot->second.path=std::move(relative);
slot->second.file=asset;
slot->second.size=info->size;
}
// A path the manifest knows that is declared but no longer packed: the patch would hide the plain file. The smallest path is named.
const std::string* moved=nullptr;
for(const auto& [fold,known]:manifest->known){
if(!declared.contains(fold)||packed.contains(fold))continue;
const std::string& path=known.newest!=PatchState::none?known.patched.path:known.release->path;
if(!moved||path<*moved)moved=&path;
}
if(moved)return std::unexpected("error: "+in_quotes(*moved)+" moved out of the pack; that needs a full release build");
const auto buffer=std::make_unique_for_overwrite<std::byte[]>(kCopyPiece);
// Whether the current asset is exactly line (spelling, size, bytes); the hash is made only when the rest agrees, and once.
const auto same=[&](Current& current,const ManifestLine& line)->std::expected<bool,std::string>{
if(current.path!=line.path||current.size!=line.size)return false;
if(!current.hash){
auto hash=hash_file(current.file,current.size,buffer.get());
if(!hash)return std::unexpected(std::move(hash.error()));
current.hash=*hash;
}
return *current.hash==line.hash;
};
std::vector<Emitted> emitted;
bool changed=false;
// Every path the manifest knows: what it should be now (an entry while packed, else a marker) against what the game sees.
for(auto& [fold,known]:manifest->known){
const auto found=packed.find(fold);
Current* current=found!=packed.end()?&found->second:nullptr;
const bool tracked=known.newest!=PatchState::none;
// What the game sees now: the newest patch's state, else the release's entry.
const ManifestLine* seen=tracked?(known.newest==PatchState::entry?&known.patched:nullptr):&*known.release;
bool differs=true;
if(current&&seen){
auto equal=same(*current,*seen);
if(!equal)return std::unexpected(std::move(equal.error()));
differs=!*equal;
}
else if(!current&&!seen)differs=false;
changed=changed||differs;
// Untracked, the game sees the release: a path equal to it stays out of the patch. Tracked, it is always given again.
if(!tracked&&!differs)continue;
if(current)emitted.push_back(Emitted{current->path,current});
else emitted.push_back(Emitted{tracked?known.patched.path:known.release->path,nullptr});
}
// Every packed asset the manifest does not know is new.
for(auto& [fold,current]:packed){
if(manifest->known.contains(fold))continue;
changed=true;
emitted.push_back(Emitted{current.path,&current});
}
if(!changed)return PatchResult{static_cast<int>(number),0,0,false};
std::ranges::sort(emitted,{},&Emitted::path);
const fs::path folder=output/"patches";
std::error_code error;
fs::create_directories(folder,error);
if(error)return std::unexpected(cannot("write",folder,error));
const fs::path final_path=folder/path_from_utf8(pack::patch_file_name(number));
fs::path temporary_path=final_path;
temporary_path+=".tmp";
TemporaryPack temporary(std::move(temporary_path));
platform::discard_file(temporary.path());
std::vector<ManifestLine> patched;
std::vector<DeletedLine> deleted;
std::size_t entries=0;
std::size_t markers=0;
{
auto storage=pack::PackStorage::open(temporary.path(),pack::KeySource(key),true);
if(!storage)return std::unexpected("error: "+vfs::failure_text(storage.error().action,path_to_utf8(temporary.path()),storage.error().failure));
for(const Emitted& item:emitted){
const std::string fold=vfs::fold_name(item.path);
const auto found=manifest->known.find(fold);
const bool ever_marked=found!=manifest->known.end()&&found->second.ever_marked;
if(!item.current){
if(auto added=(*storage)->add_deletion_marker(item.path);!added)return std::unexpected(cannot("write",final_path,added.error()));
deleted.push_back(DeletedLine{item.path,true});
++markers;
continue;
}
const Current& current=*item.current;
auto file=platform::File::open(current.file,platform::FileAccess::read);
if(!file)return std::unexpected(cannot("read",current.file,file.error()));
const auto info=platform::stat_path(current.file);
if(!info)return std::unexpected(cannot("read",current.file,info.error()));
HashingReader reader(*file,file->size());
if(auto added=(*storage)->add_reader(item.path,reader,vfs::detail::to_ms(info->modified_utc_ns));!added){
if(added.error().to)return std::unexpected(cannot("write",final_path,added.error().code));
if(reader.error())return std::unexpected(cannot("read",current.file,reader.error()));
return std::unexpected(cannot("read",current.file,added.error().code));
}
patched.push_back(ManifestLine{item.path,reader.digest(),reader.size()});
if(ever_marked)deleted.push_back(DeletedLine{item.path,false});
++entries;
}
if(auto set=(*storage)->set_metadata("base_build="+manifest->head.build_id+"\npatch="+std::to_string(number));!set)return std::unexpected(cannot("write",final_path,set.error()));
if(auto closed=(*storage)->close();!closed)return std::unexpected(cannot("write",final_path,closed.error()));
}
// The release's own lines stay as they are.
std::vector<ManifestLine> release;
release.reserve(manifest->known.size());
for(auto& [fold,known]:manifest->known)if(known.release)release.push_back(std::move(*known.release));
if(auto written=write_manifest(project,manifest->head.build_id,number+1,release,std::move(deleted),std::move(patched));!written){
discard_release_manifest(project);
return std::unexpected(std::move(written.error()));
}
if(!platform::replace_file(temporary.path(),final_path,error)){
discard_release_manifest(project);
return std::unexpected(cannot("write",final_path,error));
}
temporary.kept();
if(const std::error_code synced=platform::sync_directory(folder)){
discard_release_manifest(project);
return std::unexpected(cannot("write",final_path,synced));
}
if(auto committed=commit_release_manifest(project);!committed){
discard_release_manifest(project);
return std::unexpected(std::move(committed.error()));
}
return PatchResult{static_cast<int>(number),entries,markers,true};
}
}
