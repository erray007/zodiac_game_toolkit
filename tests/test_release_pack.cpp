// The release pack's trailer and its search (src/core/pack/release_pack.hpp; spec 8.4 and Review Focus 5 of the library-and-packs
// design): a trailer round-trips; a trailer of another payload, a damaged one and a file too short for one are refused; a pack inside
// a generated PE image followed by an attribute certificate table (what a signing tool appends) is found and read whole through its
// window, also when the signer aligned the table itself; a security directory that does not end at the file's end leaves the end
// where the file ends. Also the dev mode release pack check (vfs.hpp): a write that lands in a writable pack is not warned about.
#include <core/pack/crypto.hpp>
#include <core/pack/pack_source.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/pack/release_pack.hpp>
#include <core/payload.hpp>
#include <core/vfs/folder_source.hpp>
#include <core/vfs/vfs.hpp>
#include <doctest/doctest.h>
#include <platform/entropy.hpp>
#include <platform/file.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pack=zodiac::pack;
namespace crypto=zodiac::crypto;
namespace fs=std::filesystem;

namespace {
struct TempDir{
fs::path path;
TempDir()
{
std::uint64_t id=0;
REQUIRE(zodiac::platform::fill_entropy(&id,sizeof id));
path=fs::temp_directory_path()/("zodiac_release_pack_"+std::to_string(id));
fs::create_directories(path);
}
~TempDir()
{
std::error_code error;
fs::remove_all(path,error);
}
TempDir(const TempDir&)=delete;
TempDir& operator=(const TempDir&)=delete;
};

crypto::Key test_key(std::uint8_t seed)
{
crypto::Key key;
for(std::size_t i=0;i<key.size();++i)key[i]=static_cast<std::uint8_t>(seed+i*13);
return key;
}
std::string read_all(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void write_all(const fs::path& path,std::string_view bytes)
{
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
REQUIRE(out.good());
}
std::string bytes_of(std::size_t size,std::uint64_t seed)
{
std::mt19937_64 random(seed);
std::string bytes(size,'\0');
for(char& c:bytes)c=static_cast<char>(random()>>56);
return bytes;
}
void put16(std::string& out,std::size_t at,std::uint16_t value)
{
out[at]=static_cast<char>(value&0xFF);
out[at+1]=static_cast<char>(value>>8);
}
void put32(std::string& out,std::size_t at,std::uint32_t value)
{
for(int i=0;i<4;++i)out[at+i]=static_cast<char>((value>>(8*i))&0xFF);
}
zodiac::platform::File open_read(const fs::path& path)
{
auto file=zodiac::platform::File::open(path,zodiac::platform::FileAccess::read);
REQUIRE(file);
return std::move(*file);
}

// The entries a test pack holds: sizes around a chunk edge and a Turkish name.
const std::vector<std::pair<std::string,std::string>>& entries()
{
static const std::vector<std::pair<std::string,std::string>> list{
{"data/a.txt","alpha"},
{"sounds/\xC5\x9F\x61rk\xC4\xB1.ogg",bytes_of(65537,1)},
{"empty.bin",""},
{"maps/big.bin",bytes_of(3*65536+7,2)},
};
return list;
}
// A pack holding entries(), as bytes.
std::string pack_bytes(const fs::path& folder,const crypto::Key& key)
{
const fs::path path=folder/"build.zpk";
{
auto storage=pack::PackStorage::open(path,pack::KeySource(key),true);
REQUIRE(storage);
for(const auto& [name,data]:entries())REQUIRE((*storage)->add(name,std::as_bytes(std::span(data)),1234));
REQUIRE((*storage)->close());
}
std::string bytes=read_all(path);
fs::remove(path);
return bytes;
}
// A minimal PE32+ (or PE32) image of image_size bytes: DOS header, PE signature, COFF header and an optional header with 16 data
// directories, all zero but the magic, the directory count and the optional header's size.
std::string pe_image(std::size_t image_size,bool pe32_plus=true)
{
std::string image(image_size,'\0');
image[0]='M';
image[1]='Z';
constexpr std::size_t pe=0x80;
put32(image,0x3C,pe);
image.replace(pe,4,std::string_view("PE\0\0",4));
put16(image,pe+4,pe32_plus?0x8664:0x14C);
const std::size_t optional=pe+24;
const std::size_t directories=pe32_plus?112:96;
put16(image,pe+20,static_cast<std::uint16_t>(directories+16*8));
put16(image,optional,pe32_plus?0x20B:0x10B);
put32(image,optional+directories-4,16);
return image;
}
// Sets the security directory of a pe_image.
void set_security(std::string& image,std::uint32_t offset,std::uint32_t size,bool pe32_plus=true)
{
const std::size_t directory=0x80+24+(pe32_plus?112:96)+4*8;
put32(image,directory,offset);
put32(image,directory+4,size);
}
// image + pack + zero padding to the trailer's alignment + trailer, as zodiac build's append_pack lays it out.
std::string embed(std::string image,std::string_view pack_data,std::uint32_t payload_crc)
{
const std::uint64_t offset=image.size();
image.append(pack_data);
while((image.size()+pack::kTrailerSize)%pack::kTrailerAlignment)image.push_back('\0');
image.append(pack::trailer_bytes(offset,pack_data.size(),payload_crc));
return image;
}
// A WIN_CERTIFICATE table of one certificate: dwLength, wRevision 0x0200, wCertificateType 2 (PKCS#7), the bytes, padded to 8.
std::string certificate_table(std::size_t bytes)
{
std::string table(8+bytes,'\x5A');
put32(table,0,static_cast<std::uint32_t>(8+bytes));
put16(table,4,0x0200);
put16(table,6,2);
while(table.size()%8)table.push_back('\0');
return table;
}
// Every entry reads back exactly through the window.
void check_entries(const fs::path& file,const pack::PackLocation& window,const crypto::Key& key)
{
auto storage=pack::PackStorage::open(file,pack::KeySource(key),false,{},window);
REQUIRE(storage);
CHECK((*storage)->read_only());
CHECK((*storage)->count()==entries().size());
for(const auto& [name,data]:entries()){
CAPTURE(name);
auto reader=(*storage)->open_reader(name);
REQUIRE(reader);
std::string back(static_cast<std::size_t>((*reader)->size()),'\0');
if(!back.empty())REQUIRE((*reader)->read_at(0,std::as_writable_bytes(std::span(back)))==static_cast<std::int64_t>(back.size()));
CHECK(back==data);
}
}
}

TEST_CASE("a trailer round-trips, and one of another payload, a damaged one and a short file are refused")
{
const TempDir temp;
const std::string trailer=pack::trailer_bytes(0x0102030405060708u,0x1112131415161718u,0xA1B2C3D4u);
REQUIRE(trailer.size()==pack::kTrailerSize);
CHECK(trailer.substr(24)=="ZDTRAIL1");
CHECK(static_cast<unsigned char>(trailer[0])==0x08);
CHECK(static_cast<unsigned char>(trailer[16])==0xD4);
const std::string filler=bytes_of(4096,3);
const std::string data=bytes_of(1000,4);
const fs::path path=temp.path/"game.exe";
write_all(path,embed(filler,data,0xA1B2C3D4u));
{
const auto file=open_read(path);
// Not a PE image: the end is the file's.
CHECK(pack::image_end(file)==file.size());
const auto found=pack::find_embedded_pack(file,0xA1B2C3D4u);
REQUIRE(found);
CHECK(found->offset==4096);
CHECK(found->size==1000);
const auto other=pack::find_embedded_pack(file,0xA1B2C3D5u);
REQUIRE_FALSE(other);
CHECK(other.error()=="the pack belongs to another build");
}
// A changed offset breaks the trailer's CRC.
std::string damaged=embed(filler,data,7);
damaged[damaged.size()-pack::kTrailerSize]^=0x01;
write_all(path,damaged);
{
const auto file=open_read(path);
CHECK_FALSE(pack::find_embedded_pack(file,7));
}
// A pack that would end inside the trailer (offset + size past it), with a correct CRC.
std::string past=filler;
past.append(pack::trailer_bytes(4000,200,7));
write_all(path,past);
{
const auto file=open_read(path);
CHECK_FALSE(pack::find_embedded_pack(file,7));
}
// Files shorter than a trailer, the trailer itself cut by one byte: refused without a read outside them.
for(const std::size_t size:{std::size_t{0},std::size_t{1},std::size_t{31}}){
CAPTURE(size);
write_all(path,trailer.substr(trailer.size()-size));
const auto file=open_read(path);
CHECK(pack::image_end(file)==size);
CHECK_FALSE(pack::find_embedded_pack(file,0xA1B2C3D4u));
}
{
// Exactly a trailer whose pack is empty and starts at 0: found, and the window is empty.
const std::string exact=pack::trailer_bytes(0,0,9);
write_all(path,exact);
const auto file=open_read(path);
const auto found=pack::find_embedded_pack(file,9);
REQUIRE(found);
CHECK(found->size==0);
}
// The payload's own CRC is its last four bytes.
CHECK(pack::payload_checksum(std::string_view("\x01\x02\x03\x04\x05",5))==0x05040302u);
CHECK(pack::payload_checksum("abc")==0);
}

TEST_CASE("a signed executable's pack is found before its certificate table and reads whole through its window")
{
const TempDir temp;
const crypto::Key key=test_key(5);
const std::string pack_data=pack_bytes(temp.path,key);
const fs::path path=temp.path/"game.exe";
for(const bool pe32_plus:{true,false}){
CAPTURE(pe32_plus);
std::string image=embed(pe_image(1536,pe32_plus),pack_data,0xC0FFEEu);
// A signing tool's layout: the table starts on a multiple of 8 (the trailer already ends on one) and runs to the file's end.
REQUIRE(image.size()%8==0);
const std::uint32_t table_at=static_cast<std::uint32_t>(image.size());
const std::string table=certificate_table(1203);
image.append(table);
set_security(image,table_at,static_cast<std::uint32_t>(table.size()),pe32_plus);
write_all(path,image);
{
const auto file=open_read(path);
CHECK(pack::image_end(file)==table_at);
const auto found=pack::find_embedded_pack(file,0xC0FFEEu);
REQUIRE(found);
CHECK(found->offset==1536);
CHECK(found->size==pack_data.size());
check_entries(path,*found,key);
}
// The same image with something after the table: the directory no longer ends at the file's end, so the end is the file's,
// and no trailer is there.
std::string longer=image;
longer.append(16,'\x33');
write_all(path,longer);
{
const auto file=open_read(path);
CHECK(pack::image_end(file)==file.size());
CHECK_FALSE(pack::find_embedded_pack(file,0xC0FFEEu));
}
// An empty directory, or one that runs past the file: the file's end.
std::string empty=image;
set_security(empty,table_at,0,pe32_plus);
write_all(path,empty);
{
const auto file=open_read(path);
CHECK(pack::image_end(file)==file.size());
}
std::string past=image;
set_security(past,table_at,static_cast<std::uint32_t>(table.size()+8),pe32_plus);
write_all(path,past);
{
const auto file=open_read(path);
CHECK(pack::image_end(file)==file.size());
}
}
// A signer that aligned the table itself after a trailer ending off the alignment: up to 7 zero bytes are stepped over.
std::string image=pe_image(1536);
const std::uint64_t offset=image.size();
image.append(pack_data);
if((image.size()+pack::kTrailerSize)%8==0)image.push_back('\0');
image.append(pack::trailer_bytes(offset,pack_data.size(),42));
REQUIRE(image.size()%8!=0);
while(image.size()%8)image.push_back('\0');
const std::uint32_t table_at=static_cast<std::uint32_t>(image.size());
const std::string table=certificate_table(64);
image.append(table);
set_security(image,table_at,static_cast<std::uint32_t>(table.size()));
write_all(path,image);
const auto file=open_read(path);
const auto found=pack::find_embedded_pack(file,42);
REQUIRE(found);
CHECK(found->offset==offset);
check_entries(path,*found,key);
// A DOS header pointing past the file is no PE image.
std::string broken=pe_image(256);
put32(broken,0x3C,0x7FFFFFF0u);
write_all(path,broken);
const auto short_file=open_read(path);
CHECK(pack::image_end(short_file)==256);
}

TEST_CASE("dev mode warns about a write to a packed path only when it lands in a folder")
{
const TempDir temp;
zodiac::vfs::Vfs vfs;
std::vector<std::string> lines;
vfs.set_log([&lines](zodiac_log_level,std::string_view line){lines.emplace_back(line);});
vfs.set_dev_mode(true);
REQUIRE(vfs.mount("/",std::make_shared<zodiac::vfs::FolderSource>(temp.path/"root",false,true),true,nullptr));
vfs.set_release_pack_check([](std::string_view relative){return relative.starts_with("data/");});
const crypto::Key key=test_key(9);
auto storage=pack::PackStorage::open(temp.path/"save.zpk",pack::KeySource(key),true);
REQUIRE(storage);
const int owner=0;
REQUIRE(vfs.mount("/",pack::make_pack_source(*storage,true),true,&owner));
const std::string_view text="saved";
// Into the writable pack: the release could write it there too.
REQUIRE(vfs.write_file("data/a.txt",std::as_bytes(std::span(text))));
REQUIRE(vfs.copy("data/a.txt","data/b.txt"));
CHECK(lines.empty());
CHECK_FALSE(fs::exists(temp.path/"root"/"data"/"a.txt"));
CHECK(vfs.unmount(&owner)==1);
// Into the folder: warned once.
REQUIRE(vfs.create_directory("data",false));
REQUIRE(vfs.write_file("data/a.txt",std::as_bytes(std::span(text))));
REQUIRE(vfs.write_file("data/a.txt",std::as_bytes(std::span(text))));
CHECK(lines==std::vector<std::string>{"warning: \"data/a.txt\" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead"});
REQUIRE((*storage)->close());
}
