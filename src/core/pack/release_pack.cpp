// The release pack's trailer, its search in a signed executable and the release start-up mount (contract in release_pack.hpp).
#include <core/pack/release_pack.hpp>
#include <core/pack/pack_source.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/startup.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace zodiac::pack {
namespace {
namespace fs=std::filesystem;
constexpr int exit_start_error=2;

std::uint16_t get16(const std::byte* in) noexcept
{
return static_cast<std::uint16_t>(std::to_integer<unsigned>(in[0])|std::to_integer<unsigned>(in[1])<<8);
}
std::uint32_t get32(const std::byte* in) noexcept
{
std::uint32_t value=0;
for(int i=3;i>=0;--i)value=value<<8|std::to_integer<std::uint32_t>(in[i]);
return value;
}
std::uint64_t get64(const std::byte* in) noexcept
{
return static_cast<std::uint64_t>(get32(in))|static_cast<std::uint64_t>(get32(in+4))<<32;
}
void put32(char* out,std::uint32_t value) noexcept
{
for(int i=0;i<4;++i)out[i]=static_cast<char>((value>>(8*i))&0xFFu);
}
void put64(char* out,std::uint64_t value) noexcept
{
put32(out,static_cast<std::uint32_t>(value));
put32(out+4,static_cast<std::uint32_t>(value>>32));
}
// True when exactly size bytes were read at offset.
bool read_exact(const platform::File& file,std::uint64_t offset,std::byte* to,std::size_t size) noexcept
{
while(size>0){
const std::int64_t got=file.read_at(offset,{to,size});
if(got<=0)return false;
to+=got;
offset+=static_cast<std::uint64_t>(got);
size-=static_cast<std::size_t>(got);
}
return true;
}
std::string_view as_text(const std::byte* bytes,std::size_t size) noexcept
{
return {reinterpret_cast<const char*>(bytes),size};
}

// The PE layout image_end reads: e_lfanew at 0x3C; at it "PE\0\0" and the 20-byte COFF header (SizeOfOptionalHeader at 16), then
// the optional header: its magic, NumberOfRvaAndSizes 4 bytes before the data directories (at 112 for PE32+, 96 for PE32), and
// the security directory as the fifth 8-byte entry.
constexpr std::size_t kLfanewAt=0x3C;
constexpr std::size_t kCoffSize=24;
constexpr std::size_t kOptionalSizeAt=20;
constexpr std::size_t kSecurityIndex=4;
constexpr std::size_t kDirectorySize=8;
constexpr std::size_t kDirectoriesPe32Plus=112;
constexpr std::size_t kDirectoriesPe32=96;
constexpr std::size_t kHeadSize=kCoffSize+kDirectoriesPe32Plus+(kSecurityIndex+1)*kDirectorySize;

std::string damaged(std::string_view what)
{
std::string line="error: the game data is damaged (";
line.append(what).push_back(')');
return line;
}

std::string in_quotes(std::string_view text)
{
std::string result(1,'"');
result.append(one_line(text)).push_back('"');
return result;
}

// True when metadata (key=value lines, "\n" between them) holds the line "<key>=<value>".
bool has_line(std::string_view metadata,std::string_view key,std::string_view value) noexcept
{
while(!metadata.empty()){
const std::size_t end=metadata.find('\n');
const std::string_view line=metadata.substr(0,end);
if(line.size()==key.size()+1+value.size()&&line.starts_with(key)&&line[key.size()]=='='&&line.substr(key.size()+1)==value)return true;
if(end==std::string_view::npos)break;
metadata.remove_prefix(end+1);
}
return false;
}
bool names_build(std::string_view metadata,std::string_view build_id) noexcept
{
return has_line(metadata,"build",build_id);
}

// A file found in a patches/ folder: its number, 0 for the executable's folder and 1 for the data folder, and its path.
struct PatchFile{
std::uint64_t number;
int folder;
std::filesystem::path path;
};

// Every entry of <folder>/patches, as patch files into out or, in name order, as the naming warning. A missing folder has none.
void collect_patches(const fs::path& folder,int order,std::vector<PatchFile>& out,Diagnostics& diagnostics)
{
const fs::path patches=folder/"patches";
std::vector<std::pair<std::string,fs::path>> others;
std::error_code error;
fs::directory_iterator entry(patches,error);
if(error){
if(platform::error_kind(error)!=platform::ErrorKind::not_found)diagnostics.report(ZODIAC_LOG_ERROR,"error: cannot use patch "+in_quotes(path_to_utf8(patches))+": "+std::string(vfs::reason_text(vfs::detail::file_error(error))));
return;
}
for(const fs::directory_iterator end;entry!=end;entry.increment(error)){
fs::path path=entry->path();
std::string name=path_to_utf8(path.filename());
if(const auto number=patch_number(name))out.push_back(PatchFile{*number,order,std::move(path)});
else others.emplace_back(std::move(name),std::move(path));
}
if(error)diagnostics.report(ZODIAC_LOG_ERROR,"error: cannot use patch "+in_quotes(path_to_utf8(patches))+": "+std::string(vfs::reason_text(vfs::detail::file_error(error))));
std::ranges::sort(others,{},&std::pair<std::string,fs::path>::first);
for(const auto& [name,path]:others)diagnostics.report(ZODIAC_LOG_WARNING,"warning: ignoring "+in_quotes(path_to_utf8(path))+": patch packs are named patch_<n>.zpk");
}

// Opens and mounts one patch (mount_patches' checks); reports why when it is not used.
void mount_patch(vfs::Vfs& vfs,const crypto::Key& key,std::string_view build_id,const PatchFile& patch,Diagnostics& diagnostics)
{
const std::string shown=in_quotes(path_to_utf8(patch.path));
const auto cannot=[&](std::string_view reason){diagnostics.report(ZODIAC_LOG_ERROR,"error: cannot use patch "+shown+": "+std::string(reason));};
std::uint64_t size=0;
{
auto file=platform::File::open(patch.path,platform::FileAccess::read);
if(!file)return cannot(vfs::reason_text(vfs::detail::file_error(file.error())));
size=file->size();
}
// Through a window over the whole file: read only and without a lock, so copies of the game run side by side.
crypto::Key copy=key;
auto storage=PackStorage::open(patch.path,KeySource(copy),false,{},PackLocation{0,size});
crypto::wipe(copy.data(),copy.size());
if(!storage){
const vfs::Failure& failure=storage.error().failure;
return cannot(!failure.message.empty()?std::string_view(failure.message):vfs::reason_text(failure.code));
}
const std::string metadata=(*storage)->metadata();
if(!has_line(metadata,"base_build",build_id)){
diagnostics.report(ZODIAC_LOG_WARNING,"warning: patch "+shown+" belongs to another release of the game; it is not used");
return;
}
if(!has_line(metadata,"patch",std::to_string(patch.number))){
diagnostics.report(ZODIAC_LOG_WARNING,"warning: ignoring "+shown+": patch packs are named patch_<n>.zpk");
return;
}
try{
if(vfs.mount("/",make_pack_source(std::move(*storage),false),false,nullptr))return;
}
catch(const std::bad_alloc&){
}
cannot("out of memory");
}

// The line for a failed open of the build pack, named file (the pack's or the executable's file name).
std::string open_failure(const PackError& error,std::string_view file,bool external)
{
if(error.in_use)return "error: "+in_quotes(file)+kInUseText;
if(!error.failure.message.empty())return damaged("pack: "+error.failure.message);
if(external&&error.failure.code==vfs::FileError::not_found)return damaged("pack "+in_quotes(file)+" is missing");
return "error: "+vfs::failure_text(vfs::FileAction::read,file,error.failure);
}
}

std::uint32_t payload_checksum(std::string_view payload_bytes) noexcept
{
if(payload_bytes.size()<4)return 0;
return get32(reinterpret_cast<const std::byte*>(payload_bytes.data()+payload_bytes.size()-4));
}

std::string trailer_bytes(std::uint64_t offset,std::uint64_t size,std::uint32_t payload_crc)
{
std::string trailer(kTrailerSize,'\0');
put64(trailer.data(),offset);
put64(trailer.data()+8,size);
put32(trailer.data()+16,payload_crc);
put32(trailer.data()+20,crc32(std::string_view(trailer.data(),20)));
std::memcpy(trailer.data()+24,kTrailerMagic,8);
return trailer;
}

std::uint64_t image_end(const platform::File& file)
{
const std::uint64_t size=file.size();
std::array<std::byte,kLfanewAt+4> dos;
if(size<dos.size()||!read_exact(file,0,dos.data(),dos.size())||as_text(dos.data(),2)!="MZ")return size;
const std::uint64_t pe=get32(dos.data()+kLfanewAt);
std::array<std::byte,kHeadSize> head;
// A PE32 header with its security directory is shorter than a PE32+ one, so only what the file holds is read.
if(pe>=size)return size;
const std::size_t available=static_cast<std::size_t>(std::min<std::uint64_t>(head.size(),size-pe));
if(available<kCoffSize+2||!read_exact(file,pe,head.data(),available)||as_text(head.data(),4)!=std::string_view("PE\0\0",4))return size;
const std::uint16_t magic=get16(head.data()+kCoffSize);
const std::size_t directories=magic==0x20B?kDirectoriesPe32Plus:magic==0x10B?kDirectoriesPe32:0;
if(directories==0)return size;
const std::size_t security=directories+kSecurityIndex*kDirectorySize;
if(get16(head.data()+kOptionalSizeAt)<security+kDirectorySize||available<kCoffSize+security+kDirectorySize)return size;
if(get32(head.data()+kCoffSize+directories-4)<=kSecurityIndex)return size;
const std::uint64_t offset=get32(head.data()+kCoffSize+security);
const std::uint64_t length=get32(head.data()+kCoffSize+security+4);
if(length==0||offset>size||length!=size-offset)return size;
return offset;
}

std::expected<PackLocation,std::string> find_embedded_pack(const platform::File& file,std::uint32_t payload_crc)
{
const std::uint64_t end=image_end(file);
if(end<kTrailerSize)return std::unexpected(std::string("the executable holds no pack"));
std::array<std::byte,kTrailerSize+kTrailerAlignment-1> tail;
const std::size_t window=static_cast<std::size_t>(std::min<std::uint64_t>(end,tail.size()));
if(!read_exact(file,end-window,tail.data(),window))return std::unexpected(std::string("the executable cannot be read"));
for(std::size_t pad=0;pad+kTrailerSize<=window;++pad){
// Alignment before the end is zero bytes only.
if(pad>0&&tail[window-pad]!=std::byte{0})break;
const std::byte* trailer=tail.data()+window-pad-kTrailerSize;
if(as_text(trailer+24,8)!=std::string_view(kTrailerMagic,8))continue;
if(get32(trailer+20)!=crc32(as_text(trailer,20)))return std::unexpected(std::string("the pack trailer is damaged"));
if(get32(trailer+16)!=payload_crc)return std::unexpected(std::string("the pack belongs to another build"));
const std::uint64_t offset=get64(trailer);
const std::uint64_t size=get64(trailer+8);
const std::uint64_t start=end-pad-kTrailerSize;
if(offset>start||size>start-offset)return std::unexpected(std::string("the pack trailer is damaged"));
return PackLocation{offset,size};
}
return std::unexpected(std::string("the executable holds no pack"));
}

std::string patch_file_name(std::uint64_t n)
{
return "patch_"+std::to_string(n)+".zpk";
}

std::optional<std::uint64_t> patch_number(std::string_view file_name) noexcept
{
constexpr std::string_view prefix="patch_";
constexpr std::string_view suffix=".zpk";
if(file_name.size()<=prefix.size()+suffix.size()||!file_name.starts_with(prefix)||!file_name.ends_with(suffix))return std::nullopt;
const std::string_view digits=file_name.substr(prefix.size(),file_name.size()-prefix.size()-suffix.size());
// Decimal, at least 1, no leading zero: one name per number.
if(digits.front()<'1'||digits.front()>'9')return std::nullopt;
std::uint64_t value=0;
const auto [end,error]=std::from_chars(digits.data(),digits.data()+digits.size(),value);
if(error!=std::errc()||end!=digits.data()+digits.size())return std::nullopt;
return value;
}

void mount_patches(vfs::Vfs& vfs,const crypto::Key& key,std::string_view build_id,const fs::path& exe_dir,const fs::path& data_dir,Diagnostics& diagnostics)
{
std::vector<PatchFile> patches;
collect_patches(exe_dir,0,patches,diagnostics);
if(!data_dir.empty())collect_patches(data_dir,1,patches,diagnostics);
// Oldest first, as they are mounted: the highest number in front, of one number the data folder's.
std::ranges::sort(patches,[](const PatchFile& a,const PatchFile& b){return a.number!=b.number?a.number<b.number:a.folder<b.folder;});
for(const PatchFile& patch:patches)mount_patch(vfs,key,build_id,patch,diagnostics);
}

crypto::Key release_pack_key(const GamePayload& payload) noexcept
{
crypto::Key key{};
std::memcpy(key.data(),payload.pack_key.data(),std::min(key.size(),payload.pack_key.size()));
return key;
}

int mount_release(vfs::Vfs& vfs,const GamePayload& payload,std::uint32_t payload_crc,const fs::path& executable,Diagnostics& diagnostics)
{
const fs::path folder=executable.parent_path();
const fs::path data_dir=vfs::mount_game_folders(vfs,folder,payload.game_name,payload.host_paths,false);
if(payload.pack_mode==PackMode::none)return 0;
const auto refuse=[&diagnostics](const std::string& line){
diagnostics.report(ZODIAC_LOG_ERROR,line);
return exit_start_error;
};
if(payload.pack_key.size()!=kPayloadPackKeySize)return refuse(damaged("invalid pack key"));
if(payload.build_id.size()!=kPayloadBuildIdSize)return refuse(damaged("invalid build id"));
const bool external=payload.pack_mode==PackMode::external;
if(payload.pack_writable&&!external)return refuse(damaged("invalid pack or file setting"));
const std::string shown=external?payload.game_name+".zpk":path_to_utf8(executable.filename());
const fs::path host=external?folder/path_from_utf8(shown):executable;
// The writable pack opens as a pack file of its own; every other one through a window, read only and without a lock (so copies
// of a game with a read-only external pack run side by side): the whole file, or the part the trailer names.
std::optional<PackLocation> window;
if(!payload.pack_writable){
auto file=platform::File::open(host,platform::FileAccess::read);
if(!file)return refuse(open_failure(PackError{vfs::FileAction::read,vfs::Failure{vfs::detail::file_error(file.error()),{}}},shown,external));
if(external)window=PackLocation{0,file->size()};
else{
auto location=find_embedded_pack(*file,payload_crc);
if(!location)return refuse(damaged(std::string("pack: ")+kWrongKeyText));
window=*location;
}
}
crypto::Key key=release_pack_key(payload);
// Wiped on every way out; open wipes its own copy.
struct Wipe{
crypto::Key& key;
~Wipe(){crypto::wipe(key.data(),key.size());}
} wipe{key};
auto storage=PackStorage::open(host,KeySource(key),false,{},window);
if(!storage)return refuse(open_failure(storage.error(),shown,external));
// The pack must be the one this executable was built with: an external pack left from another build (a failed rename of the new
// one, an old copy put back) would otherwise serve stale assets without a word. An embedded one is tied by the trailer already.
if(!names_build((*storage)->metadata(),payload.build_id))return refuse(external?damaged("pack "+in_quotes(shown)+" is from another build"):damaged(std::string("pack: ")+kWrongKeyText));
const bool writable=payload.pack_writable&&!(*storage)->read_only();
(*storage)->set_log(shown,[&diagnostics](std::string_view line){diagnostics.report(ZODIAC_LOG_ERROR,line);});
std::shared_ptr<vfs::Source> source;
try{
source=make_pack_source(*storage,writable);
}
catch(const std::bad_alloc&){
return refuse("error: cannot mount the game data: out of memory");
}
// After mount_game_folders, so the pack is in front of the folder at "/"; permanent (no owner) for the game's life.
if(!vfs.mount("/",std::move(source),writable,nullptr))return refuse("error: cannot mount the game data: out of memory");
// In front of the build pack: the patches made for this build (each checked on its own; a bad one never stops the game).
mount_patches(vfs,key,payload.build_id,folder,data_dir,diagnostics);
return 0;
}
}
