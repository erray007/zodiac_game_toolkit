// Tests the pack format, PackStorage and the script pack object (src/core/pack/pack_format.hpp, pack_storage.hpp, the pack object
// of src/core/vfs/files.hpp; spec 8.2, 8.3 and 9 of the library-and-packs design):
// - entries at the chunk edges (Review Focus 3) and an index entry past 4 GiB through the encoder;
// - the security tests of spec 9: a wrong key, every byte of a flushed pack flipped, the same plaintext in two packs, a known OGG
//   header, and crafted key derivation settings;
// - a pack inside a window of a larger file, the registry, deletion markers, metadata, a reader across compact, wasted bytes,
//   crash truncation (spec 8.3);
// - the nonce rule: new ids for a replacement, a re-add and after a recovery, a fresh nonce for every index, and (in a
//   ZODIAC_TEST_HOOKS tree) no (file key, nonce) pair sealed twice in a stress run;
// - the script pack object through ScriptFixture's Vfs.
// The C++ tests use 64 KiB, 1 pass Argon2id or raw keys for speed; the script tests use the defaults, at most three key
// derivations each.
#include "script_fixture.hpp"
#include <core/pack/crypto.hpp>
#include <core/pack/pack_format.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pack=zodiac::pack;
namespace crypto=zodiac::crypto;
namespace fs=std::filesystem;
using zodiac::vfs::EntryKind;
using zodiac::vfs::FileError;
using zodiac_test::ScriptFixture;

namespace {
const pack::PackOptions kFast{64,1,16};

// A folder of its own, removed at the end.
struct TempDir{
fs::path path;
TempDir():path(zodiac_test::unique_temp_folder())
{
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
for(std::size_t i=0;i<key.size();++i)key[i]=static_cast<std::uint8_t>(seed+i*7);
return key;
}
std::span<const std::byte> span_of(std::string_view text)
{
return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
std::span<std::byte> writable(std::string& text)
{
return {reinterpret_cast<std::byte*>(text.data()),text.size()};
}
// size pseudo-random bytes of a seed.
std::string data_of(std::size_t size,std::uint64_t seed)
{
std::mt19937_64 random(seed);
std::string bytes(size,'\0');
for(char& c:bytes)c=static_cast<char>(random()>>56);
return bytes;
}
std::string file_bytes(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
REQUIRE(in.good());
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void put_bytes(const fs::path& path,std::string_view bytes)
{
fs::create_directories(path.parent_path());
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
REQUIRE(out.good());
}
std::shared_ptr<pack::PackStorage> make(const fs::path& path,const pack::KeySource& key,const pack::PackOptions& options=kFast)
{
auto storage=pack::PackStorage::open(path,key,true,options);
if(!storage)INFO(storage.error().failure.message);
REQUIRE(storage);
return std::move(*storage);
}
std::shared_ptr<pack::PackStorage> reopen(const fs::path& path,const pack::KeySource& key,std::optional<pack::PackLocation> window={})
{
auto storage=pack::PackStorage::open(path,key,false,{},window);
if(!storage)INFO(storage.error().failure.message<<" / "<<zodiac::vfs::reason_text(storage.error().failure.code));
REQUIRE(storage);
return std::move(*storage);
}
// The text of a failed open: the pack's own text, else the reason.
std::string open_error(const fs::path& path,const pack::KeySource& key,std::optional<pack::PackLocation> window={})
{
auto storage=pack::PackStorage::open(path,key,false,{},window);
REQUIRE_FALSE(storage);
const zodiac::vfs::Failure& failure=storage.error().failure;
return failure.message.empty()?std::string(zodiac::vfs::reason_text(failure.code)):failure.message;
}
void add(pack::PackStorage& storage,std::string_view name,std::string_view bytes)
{
REQUIRE(storage.add(name,span_of(bytes),0));
}
std::expected<std::string,FileError> read_entry(const pack::PackStorage& storage,std::string_view name)
{
auto reader=storage.open_reader(name);
if(!reader)return std::unexpected(reader.error());
std::string bytes(static_cast<std::size_t>((*reader)->size()),'\0');
std::size_t done=0;
while(done<bytes.size()){
const std::int64_t got=(*reader)->read_at(done,writable(bytes).subspan(done));
if(got<0)return std::unexpected(zodiac::vfs::read_error(got));
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
bytes.resize(done);
return bytes;
}
std::string read_ok(const pack::PackStorage& storage,std::string_view name)
{
auto bytes=read_entry(storage,name);
INFO(name);
REQUIRE(bytes);
return *bytes;
}
std::span<const std::byte,pack::kHeaderSize> header_span(const std::string& file)
{
return std::span<const std::byte,pack::kHeaderSize>(reinterpret_cast<const std::byte*>(file.data()),pack::kHeaderSize);
}
std::span<const std::byte,pack::kFooterSize> footer_span(const std::string& file,std::size_t at)
{
return std::span<const std::byte,pack::kFooterSize>(reinterpret_cast<const std::byte*>(file.data()+at),pack::kFooterSize);
}
// The index the complete footer at the end of file names, opened with raw key key.
pack::Index index_of(const std::string& file,const crypto::Key& key)
{
REQUIRE(file.size()>=pack::kHeaderSize+pack::kMinIndexLength+pack::kFooterSize);
const auto header=pack::decode_header(header_span(file));
REQUIRE(header);
crypto::Key file_key=crypto::derive_file_key(key,header->salt);
pack::Footer footer;
REQUIRE(pack::inspect_footer(footer_span(file,file.size()-pack::kFooterSize),file.size()-pack::kFooterSize,footer)==pack::FooterState::complete);
std::string plain=file.substr(static_cast<std::size_t>(footer.index_offset),static_cast<std::size_t>(footer.index_length-pack::kTagSize));
crypto::Tag tag;
std::memcpy(tag.data(),file.data()+footer.index_offset+plain.size(),tag.size());
REQUIRE(crypto::open(file_key,footer.nonce,header_span(file),span_of(plain),tag,writable(plain)));
auto index=pack::decode_index(span_of(plain));
REQUIRE(index);
return std::move(*index);
}
pack::IndexEntry entry_of(const pack::Index& index,std::string_view name)
{
for(const pack::IndexEntry& entry:index.entries)if(entry.name==name)return entry;
FAIL("no entry "<<name);
return {};
}
// The nonces of every complete footer in file.
std::vector<crypto::Nonce> footer_nonces(const std::string& file)
{
std::vector<crypto::Nonce> nonces;
for(std::size_t at=file.find(pack::kFooterMagic);at!=std::string::npos;at=file.find(pack::kFooterMagic,at+1)){
if(at+pack::kFooterMagic.size()<pack::kFooterSize)continue;
const std::size_t start=at+pack::kFooterMagic.size()-pack::kFooterSize;
pack::Footer footer;
if(pack::inspect_footer(footer_span(file,start),start,footer)==pack::FooterState::complete)nonces.push_back(footer.nonce);
}
return nonces;
}
std::vector<std::string> names_of(const pack::PackStorage& storage)
{
return storage.list();
}
}

TEST_CASE("entries at the chunk edges read back exactly, and reads inside and across chunks give the right bytes")
{
TempDir dir;
const fs::path path=dir.path/"edges.zpk";
const crypto::Key key=test_key(1);
std::map<std::string,std::string> data;
for(const std::size_t size:{std::size_t{0},std::size_t{1},std::size_t{65535},std::size_t{65536},std::size_t{65537},std::size_t{196615}})data["e"+std::to_string(size)]=data_of(size,size+1);
{
// chunk_log2 16 explicitly, so the edges stay at 64 KiB whatever the default becomes.
const auto storage=make(path,key,pack::PackOptions{64,1,16});
for(const auto& [name,bytes]:data)add(*storage,name,bytes);
for(const auto& [name,bytes]:data)CHECK(read_ok(*storage,name)==bytes);
REQUIRE(storage->flush());
}
const auto storage=reopen(path,key);
for(const auto& [name,bytes]:data){
CHECK(read_ok(*storage,name)==bytes);
const auto info=storage->stat(name);
REQUIRE(info);
CHECK(info->size==bytes.size());
}
const std::string& big=data["e196615"];
const auto reader=*storage->open_reader("e196615");
std::string three(3,'\0');
CHECK(reader->read_at(65535,writable(three))==3);
CHECK(three==big.substr(65535,3));
std::string at_edge(10,'\0');
CHECK(reader->read_at(65536,writable(at_edge))==10);
CHECK(at_edge==big.substr(65536,10));
// Chunks 0, 1 and 2 in one read.
std::string spanning(2*65536+10,'\0');
CHECK(reader->read_at(65530,writable(spanning))==static_cast<std::int64_t>(spanning.size()));
CHECK(spanning==big.substr(65530,spanning.size()));
// Byte by byte across an edge (the cached chunk), then the end.
for(std::uint64_t at=131070;at<131074;++at){
std::string one(1,'\0');
CHECK(reader->read_at(at,writable(one))==1);
CHECK(one[0]==big[static_cast<std::size_t>(at)]);
}
CHECK(reader->read_at(196613,writable(three))==2);
CHECK(three.substr(0,2)==big.substr(196613,2));
CHECK(reader->read_at(196615,writable(three))==0);
CHECK(reader->read_at(1u<<30,writable(three))==0);
}

TEST_CASE("an index entry past 4 GiB round-trips through the encoder and decoder")
{
pack::Index index;
index.generation=7;
index.wasted_bytes=0x1'2345'6789;
index.metadata="build=ab\n";
pack::IndexEntry entry;
entry.name="music/long.ogg";
entry.id=0x0123'4567'89AB'CDEF;
entry.size=0x2'0000'0000;
entry.offset=0x1'0000'0010;
entry.modified_utc_ms=-5;
index.entries.push_back(entry);
pack::IndexEntry marker;
marker.name="gone.txt";
marker.deleted=true;
index.entries.push_back(marker);
const std::vector<std::byte> plain=pack::encode_index(index);
const auto decoded=pack::decode_index(plain);
REQUIRE(decoded);
CHECK(decoded->generation==7);
CHECK(decoded->wasted_bytes==0x1'2345'6789);
CHECK(decoded->metadata=="build=ab\n");
REQUIRE(decoded->entries.size()==2);
CHECK(decoded->entries[0].name==entry.name);
CHECK(decoded->entries[0].id==entry.id);
CHECK(decoded->entries[0].size==0x2'0000'0000);
CHECK(decoded->entries[0].offset==0x1'0000'0010);
CHECK(decoded->entries[0].modified_utc_ms==-5);
CHECK_FALSE(decoded->entries[0].deleted);
CHECK(decoded->entries[1].deleted);
// The data region decides: 8 GiB of chunks from past 4 GiB fit below 2^40, not below 2^33.
CHECK(pack::validate_index(*decoded,16,std::uint64_t{1}<<40));
CHECK_FALSE(pack::validate_index(*decoded,16,std::uint64_t{1}<<33));
CHECK(*pack::stored_size(0x2'0000'0000,16)==0x2'0000'0000+(0x2'0000'0000>>16)*16);
// A broken structure is refused: every cut and a byte too many.
for(std::size_t cut=0;cut<plain.size();++cut)CHECK_FALSE(pack::decode_index(std::span(plain).first(cut)));
std::vector<std::byte> longer=plain;
longer.push_back(std::byte{0});
CHECK_FALSE(pack::decode_index(longer));
}

TEST_CASE("a wrong key fails at open")
{
TempDir dir;
const fs::path raw=dir.path/"raw.zpk";
const fs::path password=dir.path/"password.zpk";
{
const auto storage=make(raw,test_key(1));
add(*storage,"a","alpha");
REQUIRE(storage->flush());
const auto other=make(password,std::string("right"));
add(*other,"a","alpha");
REQUIRE(other->flush());
}
CHECK(open_error(raw,test_key(2))==pack::kWrongKeyText);
CHECK(open_error(raw,std::string("right"))==pack::kWrongKeyText);
CHECK(open_error(password,std::string("wrong"))==pack::kWrongKeyText);
CHECK(open_error(password,test_key(1))==pack::kWrongKeyText);
CHECK(read_ok(*reopen(password,std::string("right")),"a")=="alpha");
CHECK(open_error(dir.path/"missing.zpk",test_key(1))=="file not found");
}

// Every byte of a flushed two-entry pack flipped in turn: the open fails with the wrong key text (a version byte reads as a
// newer format), or every entry reads exactly its bytes or fails with damaged data; a byte of the header, of the live entries,
// index or footer always gives one of the failures, so the pack never opens at an older generation. Only the superseded empty
// index and footer of the creation are dead bytes.
TEST_CASE("flipping any byte of a flushed pack is refused and never returns other bytes")
{
TempDir dir;
const fs::path path=dir.path/"flip.zpk";
const crypto::Key key=test_key(3);
const std::string a=data_of(300,1);
const std::string b=data_of(5000,2);
{
const auto storage=make(path,key,pack::PackOptions{64,1,12});
add(*storage,"a",a);
add(*storage,"b",b);
REQUIRE(storage->flush());
}
const std::string good=file_bytes(path);
// The creation's empty index (no metadata, no entry) and its footer.
const std::size_t dead_begin=pack::kHeaderSize;
const std::size_t dead_end=pack::kHeaderSize+pack::kMinIndexLength+pack::kFooterSize;
std::size_t refused=0;
// One byte changed in place, and put back after: the file is never rewritten whole.
auto patch=zodiac::platform::File::open(path,zodiac::platform::FileAccess::read_write);
REQUIRE(patch);
const auto put_byte=[&](std::size_t at,char value){
const std::byte byte{static_cast<unsigned char>(value)};
REQUIRE(patch->write_at(at,std::span(&byte,1)));
};
// Whether the pack with byte at flipped refused it (at open or on a read).
const auto refuses=[&](std::size_t at){
CAPTURE(at);
// A window of the whole file: the same open, read only (so no system scans a file opened for writing each time).
auto storage=pack::PackStorage::open(path,key,false,{},pack::PackLocation{0,good.size()});
const bool dead=at>=dead_begin&&at<dead_end;
if(!storage){
const bool version=at>=8&&at<12;
CHECK(storage.error().failure.message==(version?pack::kNewerFormatText:pack::kWrongKeyText));
CHECK_FALSE(dead);
return true;
}
bool damaged=false;
for(const auto& [name,bytes]:{std::pair<const char*,const std::string*>{"a",&a},{"b",&b}}){
const auto read=read_entry(**storage,name);
if(read)CHECK(*read==*bytes);
else{
CHECK(read.error()==FileError::damaged);
damaged=true;
}
}
CHECK((dead||damaged));
return damaged;
};
for(std::size_t at=0;at<good.size();++at){
put_byte(at,static_cast<char>(good[at]^0xFF));
refused+=refuses(at);
put_byte(at,good[at]);
}
CHECK(file_bytes(path)==good);
CHECK(refused==good.size()-(dead_end-dead_begin));
}

TEST_CASE("the same plaintext in two packs with the same password gives different ciphertext")
{
TempDir dir;
const std::string plain=data_of(1000,9);
std::string first;
std::string second;
for(const char* name:{"one.zpk","two.zpk"}){
{
const auto storage=make(dir.path/name,std::string("same password"));
add(*storage,"same.bin",plain);
REQUIRE(storage->flush());
}
// The first entry starts right after the creation's index and footer.
const std::string file=file_bytes(dir.path/name);
(first.empty()?first:second)=file.substr(pack::kHeaderSize+pack::kMinIndexLength+pack::kFooterSize,plain.size()+pack::kTagSize);
}
CHECK(first!=second);
CHECK(first.substr(0,plain.size())!=plain);
}

// Known plaintext (spec 9): two entries start with one real 27-byte OGG page header and go on with different data over three
// chunks. The keystream that entry 1's header gives away decrypts no other 27-byte window of either entry.
TEST_CASE("a known OGG page header reveals no other bytes")
{
TempDir dir;
const fs::path path=dir.path/"ogg.zpk";
const crypto::Key key=test_key(5);
// "OggS", version 0, the first page's header type, granule 0, a serial number, page 0, its CRC, one segment.
static constexpr unsigned char header[27]={'O','g','g','S',0x00,0x02,0,0,0,0,0,0,0,0,0x3A,0x74,0x1B,0x59,0,0,0,0,0x8C,0x6F,0x52,0xD4,0x01};
const std::string ogg(reinterpret_cast<const char*>(header),sizeof header);
const std::size_t size=2*65536+5000;
const std::string one=ogg+data_of(size-ogg.size(),11);
const std::string two=ogg+data_of(size-ogg.size(),12);
{
const auto storage=make(path,key,pack::PackOptions{64,1,16});
add(*storage,"one.ogg",one);
add(*storage,"two.ogg",two);
REQUIRE(storage->flush());
}
const std::string file=file_bytes(path);
const pack::Index index=index_of(file,key);
// Each entry's ciphertext without its tags.
const auto cipher_of=[&](const pack::IndexEntry& entry){
std::string cipher;
for(std::uint64_t at=0;at<entry.size;at+=65536){
const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(65536,entry.size-at));
cipher.append(file,static_cast<std::size_t>(entry.offset+(at/65536)*(65536+pack::kTagSize)),length);
}
return cipher;
};
const std::string c1=cipher_of(entry_of(index,"one.ogg"));
const std::string c2=cipher_of(entry_of(index,"two.ogg"));
REQUIRE(c1.size()==size);
REQUIRE(c2.size()==size);
std::string keystream(27,'\0');
for(std::size_t i=0;i<27;++i)keystream[i]=static_cast<char>(c1[i]^ogg[i]);
const auto reveals=[&](const std::string& cipher,const std::string& plain,std::size_t at){
for(std::size_t i=0;i<27;++i)if(static_cast<char>(cipher[at+i]^keystream[i])!=plain[at+i])return false;
return true;
};
std::size_t windows=0;
std::size_t revealed=0;
for(std::size_t at=1;at+27<=size;++at,++windows)revealed+=reveals(c1,one,at);
for(std::size_t at=0;at+27<=size;++at,++windows)revealed+=reveals(c2,two,at);
CHECK(windows==2*(size-26)-1);
CHECK(revealed==0);
CHECK(c2.substr(0,27)!=c1.substr(0,27));
}

TEST_CASE("crafted key derivation settings are refused as damage, and the largest allowed ones open")
{
TempDir dir;
const fs::path path=dir.path/"kdf.zpk";
{
const auto storage=make(path,std::string("pw"));
add(*storage,"a","alpha");
REQUIRE(storage->flush());
}
const std::string good=file_bytes(path);
const auto patched=[&](std::size_t at,std::uint32_t value,std::size_t width){
std::string bytes=good;
for(std::size_t i=0;i<width;++i)bytes[at+i]=static_cast<char>(value>>(8*i));
return bytes;
};
// kdf_kib at 20, kdf_passes at 24, chunk_log2 at 16.
for(const auto& bytes:{patched(20,65537,4),patched(24,9,4),patched(16,30,1),patched(16,11,1),patched(20,7,4),patched(24,0,4)}){
put_bytes(path,bytes);
CHECK(open_error(path,std::string("pw"))==pack::kWrongKeyText);
}
const fs::path largest=dir.path/"largest.zpk";
{
const auto storage=make(largest,std::string("pw"),pack::PackOptions{65536,8,16});
add(*storage,"a","alpha");
REQUIRE(storage->flush());
}
const auto storage=reopen(largest,std::string("pw"));
CHECK(read_ok(*storage,"a")=="alpha");
CHECK_THROWS_AS(pack::PackStorage::open(dir.path/"too_much.zpk",std::string("pw"),true,pack::PackOptions{65537,1,16}),std::invalid_argument);
CHECK_FALSE(fs::exists(dir.path/"too_much.zpk"));
}

TEST_CASE("a pack inside a window of a larger file opens there, and never reads outside it")
{
TempDir dir;
const fs::path path=dir.path/"window.zpk";
const crypto::Key key=test_key(7);
const std::string e1=data_of(3000,21);
const std::string e2=data_of(70000,22);
std::string two_flushes;
std::string later_footer;
{
const auto storage=make(path,key);
add(*storage,"e1",e1);
REQUIRE(storage->flush());
add(*storage,"e2",e2);
REQUIRE(storage->flush());
two_flushes=file_bytes(path);
add(*storage,"e3","three");
REQUIRE(storage->flush());
const std::string three=file_bytes(path);
later_footer=three.substr(three.size()-pack::kFooterSize);
}
// 4096 bytes before, then 60 bytes and a later flush's footer after.
const fs::path container=dir.path/"container.bin";
put_bytes(container,data_of(4096,23)+two_flushes+data_of(60,24)+later_footer);
const std::uint64_t size=two_flushes.size();
{
const auto storage=reopen(container,key,pack::PackLocation{4096,size});
CHECK(storage->read_only());
CHECK(names_of(*storage)==std::vector<std::string>{"e1","e2"});
CHECK(read_ok(*storage,"e1")==e1);
CHECK(read_ok(*storage,"e2")==e2);
CHECK(storage->add("x",span_of("x"),0).error()==FileError::access_denied);
}
{
// One byte short: the last footer is incomplete, so the pack opens at its own previous flush inside the window.
const auto storage=reopen(container,key,pack::PackLocation{4096,size-1});
CHECK(names_of(*storage)==std::vector<std::string>{"e1"});
CHECK(read_ok(*storage,"e1")==e1);
}
CHECK(open_error(container,key,pack::PackLocation{4096,size+200})==pack::kWrongKeyText);
// The registry keys on the offset: a small pack at 0 and this one at 4096 of one file are two storages.
const fs::path small=dir.path/"small.zpk";
{
const auto storage=make(small,key);
add(*storage,"s","small");
REQUIRE(storage->flush());
}
std::string first=file_bytes(small);
REQUIRE(first.size()<=4096);
const std::uint64_t small_size=first.size();
first.resize(4096,'\0');
const fs::path both=dir.path/"both.bin";
put_bytes(both,first+two_flushes);
const auto at_zero=reopen(both,key,pack::PackLocation{0,small_size});
const auto at_4096=reopen(both,key,pack::PackLocation{4096,size});
CHECK(at_zero.get()!=at_4096.get());
CHECK(reopen(both,key,pack::PackLocation{4096,size}).get()==at_4096.get());
CHECK(names_of(*at_zero)==std::vector<std::string>{"s"});
CHECK(names_of(*at_4096)==std::vector<std::string>{"e1","e2"});
}

TEST_CASE("a deletion marker is stat's deleted kind, left out of list and count, and survives a reopen")
{
TempDir dir;
const fs::path path=dir.path/"marker.zpk";
const crypto::Key key=test_key(8);
{
const auto storage=make(path,key);
add(*storage,"a","alpha");
add(*storage,"b","beta");
REQUIRE(storage->add_deletion_marker("b"));
CHECK(storage->stat("b")->kind==EntryKind::deleted);
CHECK(storage->stat("B")->kind==EntryKind::deleted);
CHECK(storage->open_reader("b").error()==FileError::not_found);
CHECK(names_of(*storage)==std::vector<std::string>{"a"});
CHECK(storage->count()==1);
REQUIRE(storage->flush());
}
const auto storage=reopen(path,key);
CHECK(storage->stat("b")->kind==EntryKind::deleted);
CHECK(storage->stat("a")->kind==EntryKind::file);
CHECK(names_of(*storage)==std::vector<std::string>{"a"});
CHECK(storage->count()==1);
CHECK(storage->add_deletion_marker("../x").error()==FileError::bad_path);
}

TEST_CASE("metadata survives a flush and a reopen exactly")
{
TempDir dir;
const fs::path path=dir.path/"meta.zpk";
const crypto::Key key=test_key(9);
{
const auto storage=make(path,key);
CHECK(storage->metadata().empty());
REQUIRE(storage->set_metadata("build=ab\n"));
REQUIRE(storage->flush());
}
CHECK(reopen(path,key)->metadata()=="build=ab\n");
}

TEST_CASE("the registry shares one storage per file and checks the key")
{
TempDir dir;
const fs::path path=dir.path/"shared.zpk";
const auto first=make(path,std::string("pw"));
const auto second=reopen(path,std::string("pw"));
CHECK(first.get()==second.get());
// Another spelling of the same file.
CHECK(reopen(dir.path/"."/"shared.zpk",std::string("pw")).get()==first.get());
CHECK(open_error(path,std::string("other"))==pack::kWrongKeyText);
CHECK(open_error(path,test_key(1))==pack::kWrongKeyText);
// Closed, it leaves the registry: the next open is a storage of its own.
add(*first,"a","alpha");
REQUIRE(first->close());
CHECK_FALSE(second->is_open());
CHECK(second->stat("a").error()==FileError::not_found);
const auto third=reopen(path,std::string("pw"));
CHECK(third.get()!=first.get());
CHECK(read_ok(*third,"a")=="alpha");
}

TEST_CASE("a reader opened before compact and a replacement reads its own bytes")
{
TempDir dir;
const fs::path path=dir.path/"snapshot.zpk";
const crypto::Key key=test_key(10);
const std::string a=data_of(200000,31);
const auto storage=make(path,key);
add(*storage,"a",a);
add(*storage,"b",data_of(5000,32));
REQUIRE(storage->flush());
const auto reader=*storage->open_reader("a");
REQUIRE(storage->remove("b"));
REQUIRE(storage->compact());
add(*storage,"a","replaced");
REQUIRE(storage->flush());
std::string got(a.size(),'\0');
CHECK(reader->read_at(0,writable(got))==static_cast<std::int64_t>(a.size()));
CHECK(got==a);
CHECK(read_ok(*storage,"a")=="replaced");
}

TEST_CASE("replacements, removals and flushes waste bytes, and compact reclaims them")
{
TempDir dir;
const fs::path path=dir.path/"wasted.zpk";
const crypto::Key key=test_key(11);
const std::uint64_t first_index=pack::kMinIndexLength+pack::kFooterSize;
const std::uint64_t a_stored=*pack::stored_size(1000,16);
const std::uint64_t b_stored=*pack::stored_size(70000,16);
const auto storage=make(path,key);
CHECK(storage->wasted_bytes()==0);
add(*storage,"a",data_of(1000,41));
add(*storage,"b",data_of(70000,42));
add(*storage,"c","sea");
REQUIRE(storage->flush());
// The creation's empty index and footer.
CHECK(storage->wasted_bytes()==first_index);
const std::string file=file_bytes(path);
const pack::Index index=index_of(file,key);
pack::Footer footer;
REQUIRE(pack::inspect_footer(footer_span(file,file.size()-pack::kFooterSize),file.size()-pack::kFooterSize,footer)==pack::FooterState::complete);
CHECK(index.wasted_bytes==first_index);
add(*storage,"a","new a");
CHECK(storage->wasted_bytes()==first_index+a_stored);
REQUIRE(storage->remove("b"));
CHECK(storage->wasted_bytes()==first_index+a_stored+b_stored);
CHECK_FALSE(*storage->remove("b"));
REQUIRE(storage->flush());
CHECK(storage->wasted_bytes()==first_index+a_stored+b_stored+footer.index_length+pack::kFooterSize);
// Nothing changed: nothing written, nothing wasted.
const std::uint64_t size=fs::file_size(path);
REQUIRE(storage->flush());
CHECK(fs::file_size(path)==size);
REQUIRE(storage->compact());
CHECK(storage->wasted_bytes()==0);
CHECK(names_of(*storage)==std::vector<std::string>{"a","c"});
CHECK(read_ok(*storage,"a")=="new a");
CHECK(read_ok(*storage,"c")=="sea");
CHECK_FALSE(fs::exists(dir.path/"wasted.zpk.compact.tmp"));
CHECK(fs::file_size(path)<size);
// Nothing but the header, the two entries, one index and its footer.
const std::string after=file_bytes(path);
const pack::Index compacted=index_of(after,key);
CHECK(compacted.wasted_bytes==0);
REQUIRE(pack::inspect_footer(footer_span(after,after.size()-pack::kFooterSize),after.size()-pack::kFooterSize,footer)==pack::FooterState::complete);
CHECK(footer.index_offset==pack::kHeaderSize+*pack::stored_size(5,16)+*pack::stored_size(3,16));
CHECK(std::min(entry_of(compacted,"a").offset,entry_of(compacted,"c").offset)==pack::kHeaderSize);
}

// Spec 8.3: add a, flush, add b. Copy A before b's flush, copy B after it. Every truncation of either that cuts into what came
// after a's flush opens at exactly {a}; one inside the creation's footer leaves no complete footer and is damaged, and one inside
// a's footer opens at the creation's empty state, the last complete flush.
TEST_CASE("a crash-truncated pack opens at its last flush, and the first write after truncates the file")
{
TempDir dir;
const fs::path path=dir.path/"crash.zpk";
const crypto::Key key=test_key(12);
const std::string a=data_of(3000,51);
const std::string b=data_of(10000,52);
std::string copy_a;
std::string copy_b;
std::size_t after_a=0;
{
const auto storage=make(path,key,pack::PackOptions{64,1,12});
add(*storage,"a",a);
REQUIRE(storage->flush());
after_a=static_cast<std::size_t>(fs::file_size(path));
add(*storage,"b",b);
copy_a=file_bytes(path);
REQUIRE(storage->flush());
copy_b=file_bytes(path);
}
const std::size_t b_stored=static_cast<std::size_t>(*pack::stored_size(b.size(),12));
REQUIRE(copy_a.size()==after_a+b_stored);
int copy=0;
const auto open_cut=[&](const std::string& source,std::size_t size){
const fs::path cut=dir.path/("cut"+std::to_string(++copy)+".zpk");
put_bytes(cut,std::string_view(source).substr(0,size));
return cut;
};
for(const auto& [source,size]:{std::pair<const std::string*,std::size_t>{&copy_a,after_a},{&copy_a,after_a+1},{&copy_a,after_a+b_stored/2},{&copy_b,copy_a.size()},{&copy_b,copy_b.size()-1}}){
CAPTURE(size);
const auto storage=reopen(open_cut(*source,size),key);
CHECK(names_of(*storage)==std::vector<std::string>{"a"});
CHECK(read_ok(*storage,"a")==a);
}
const std::size_t first_footer=pack::kHeaderSize+pack::kMinIndexLength;
CHECK(open_error(open_cut(copy_a,first_footer+20),key)==pack::kWrongKeyText);
CHECK(names_of(*reopen(open_cut(copy_a,after_a-20),key)).empty());
// After a recovery from the middle of b's chunks, the first write truncates the file to a's footer.
const fs::path recovered=open_cut(copy_a,after_a+b_stored/2);
{
const auto storage=reopen(recovered,key);
add(*storage,"c","sea");
REQUIRE(storage->flush());
}
const std::string file=file_bytes(recovered);
pack::Footer footer;
REQUIRE(pack::inspect_footer(footer_span(file,file.size()-pack::kFooterSize),file.size()-pack::kFooterSize,footer)==pack::FooterState::complete);
CHECK(footer.index_offset==after_a+*pack::stored_size(3,12));
const auto storage=reopen(recovered,key);
CHECK(names_of(*storage)==std::vector<std::string>{"a","c"});
CHECK(read_ok(*storage,"a")==a);
CHECK(read_ok(*storage,"c")=="sea");
}

TEST_CASE("a replacement and a re-add after remove take new ids, and so do entries added after a recovery")
{
TempDir dir;
const fs::path path=dir.path/"ids.zpk";
const crypto::Key key=test_key(13);
const auto storage=make(path,key);
add(*storage,"x","one");
REQUIRE(storage->flush());
const std::uint64_t first=entry_of(index_of(file_bytes(path),key),"x").id;
add(*storage,"x","two");
REQUIRE(storage->flush());
const std::uint64_t replaced=entry_of(index_of(file_bytes(path),key),"x").id;
REQUIRE(storage->remove("x"));
add(*storage,"x","three");
REQUIRE(storage->flush());
const std::uint64_t readded=entry_of(index_of(file_bytes(path),key),"x").id;
CHECK(first!=0);
CHECK(replaced!=first);
CHECK(readded!=first);
CHECK(readded!=replaced);
// b was flushed in copy B with an id; cut before that index, b is lost, and the b added after the recovery has another id.
add(*storage,"b","bee");
const std::string before=file_bytes(path);
REQUIRE(storage->flush());
const std::string after=file_bytes(path);
const std::uint64_t lost=entry_of(index_of(after,key),"b").id;
const fs::path cut=dir.path/"cut.zpk";
put_bytes(cut,before);
const auto recovered=reopen(cut,key);
CHECK_FALSE(recovered->stat("b"));
add(*recovered,"b","bee again");
REQUIRE(recovered->flush());
const pack::Index index=index_of(file_bytes(cut),key);
CHECK(entry_of(index,"b").id!=lost);
CHECK(entry_of(index,"b").id!=entry_of(index,"x").id);
}

TEST_CASE("every flush seals its index under a nonce of its own")
{
TempDir dir;
const fs::path path=dir.path/"nonces.zpk";
const auto storage=make(path,test_key(14));
for(int i=0;i<64;++i){
add(*storage,"n"+std::to_string(i%5),std::to_string(i));
REQUIRE(storage->flush());
}
const std::vector<crypto::Nonce> nonces=footer_nonces(file_bytes(path));
CHECK(nonces.size()==65);
CHECK(std::set<crypto::Nonce>(nonces.begin(),nonces.end()).size()==nonces.size());
}

#if defined(ZODIAC_TEST_HOOKS)
// The nonce rule end to end: every seal of a stress run of adds, replacements, removals, flushes, compacts, a crash recovery of
// a copy (the same salt, so the same file key) and writes after it, recorded through the test hook. No (file key, nonce) pair
// is sealed twice, and every entry still reads back.
TEST_CASE("no file key and nonce pair is ever sealed twice")
{
TempDir dir;
const fs::path path=dir.path/"stress.zpk";
const crypto::Key key=test_key(15);
std::vector<pack::testing::SealRecord> seals;
pack::testing::pack_record_seals(&seals);
struct Stop{
~Stop(){pack::testing::pack_record_seals(nullptr);}
} stop;
std::map<std::string,std::string> expected;
std::mt19937_64 random(77);
{
const auto storage=make(path,key,pack::PackOptions{64,1,12});
for(int step=0;step<400;++step){
const std::string name="e"+std::to_string(random()%24);
const std::uint64_t action=random()%10;
if(action<6){
const std::string bytes=data_of(static_cast<std::size_t>(random()%9000),random());
add(*storage,name,bytes);
expected[name]=bytes;
}
else if(action<8){
REQUIRE(storage->remove(name));
expected.erase(name);
}
else if(action==8)REQUIRE(storage->flush());
else REQUIRE(storage->compact());
}
REQUIRE(storage->flush());
for(const auto& [name,bytes]:expected)CHECK(read_ok(*storage,name)==bytes);
// A copy cut in the middle of its last chunks: recovered, it writes under the same file key.
add(*storage,"tail",data_of(20000,5));
const std::string copy=file_bytes(path);
const fs::path cut=dir.path/"stress_cut.zpk";
put_bytes(cut,std::string_view(copy).substr(0,copy.size()-7000));
const auto recovered=reopen(cut,key);
for(int i=0;i<20;++i)add(*recovered,"r"+std::to_string(i),data_of(5000,static_cast<std::uint64_t>(i)));
REQUIRE(recovered->flush());
REQUIRE(recovered->compact());
}
std::set<std::pair<std::array<std::uint8_t,32>,crypto::Nonce>> distinct;
std::size_t chunks=0;
for(const pack::testing::SealRecord& seal:seals){
distinct.emplace(seal.key_tag,seal.nonce);
chunks+=!seal.index;
}
CHECK(distinct.size()==seals.size());
CHECK(chunks>1000);
std::printf("pack seal log checked %zu seals, %zu of them chunks\n",seals.size(),chunks);
}
#endif

namespace {
// Runs "string main(){<body>}" and returns the text.
std::string text_of(ScriptFixture& fixture,std::string_view body)
{
const std::string source="string main(){"+std::string(body)+"}";
INFO(source);
const int result=fixture.run(source.c_str(),"string main()");
INFO(fixture.exception_string());
REQUIRE(result==asEXECUTION_FINISHED);
return fixture.returned_string();
}
}

TEST_CASE("the script pack round-trips Turkish names, finds them case-insensitively and lists them by bytes")
{
ScriptFixture fixture;
put_bytes(fixture.root()/"in"/"source.txt","from disk");
CHECK(text_of(fixture,
"pack p; if(!p.open(\"t.zpk\", \"k\", true))return p.error;"
"if(!p.add(\"ses/IŞIK.ogg\", \"light\"))return p.error;"
"if(!p.add(\"b.txt\", \"bee\"))return p.error;"
"if(!p.add(\"A.txt\", \"ay\"))return p.error;"
"if(!p.add_file(\"in/source.txt\", \"in\\\\disk.txt\"))return p.error;"
"string s=p.exists(\"ses/ışık.ogg\")+\",\"+p.exists(\"SES/işik.OGG\")+\",\"+p.exists(\"ses/İŞİK.ogg\")+\",\"+p.exists(\"SES/A.OGG\")+\",\"+p.exists(\"a.TXT\");"
"s+=\"|\"+p.list().join(\",\")+\"|\"+p.count+\"|\"+p.size(\"IN/DISK.TXT\")+\",\"+p.size(\"none\")+\"|\"+p.read(\"ses/ışık.ogg\");"
"if(!p.extract(\"in/disk.txt\", \"data:/out.txt\"))return p.error;"
"s+=\"|\"+read_file(\"data:/out.txt\");"
"if(p.add(\"../x\", \"y\"))return \"added\";"
"s+=\"|\"+p.error;"
"if(p.remove(\"none\"))return \"removed\";"
"s+=\"|\"+p.error;"
"p.close();"
"if(!p.open(\"t.zpk\", \"k\"))return p.error;"
"return s+\"|\"+p.list().join(\",\")+\"|\"+p.read(\"in/disk.txt\");")==
"true,true,false,false,true|A.txt,b.txt,in/disk.txt,ses/IŞIK.ogg|4|9,-1|light|from disk|\"../x\" is not a valid entry name|cannot delete \"none\" from the pack: file not found|A.txt,b.txt,in/disk.txt,ses/IŞIK.ogg|from disk");
}

TEST_CASE("a second pack object reads what the first wrote, and closing one leaves the storage to the other")
{
ScriptFixture fixture;
CHECK(text_of(fixture,
"pack p; if(!p.open(\"m.zpk\", \"k\", true))return p.error; p.add(\"x\", \"1\"); p.close();"
"pack q; if(!q.open(\"m.zpk\", \"k\"))return q.error;"
"pack r; if(!r.open(\"m.zpk\", \"k\"))return r.error;"
"string s=q.read(\"x\"); q.close();"
"return s+\",\"+q.is_open+\",\"+r.is_open+\",\"+r.read(\"x\");")=="1,false,true,1");
const std::string missing=text_of(fixture,"pack p; if(!p.open(\"m.zpk\", \"k\"))return p.error; string s; try{ p.read(\"missing\"); }catch{ s=exception_info(); } return s;");
CHECK(missing=="cannot read \"missing\" from the pack: file not found");
}

TEST_CASE("the script pack's open failures and a closed pack have exact texts")
{
ScriptFixture fixture;
CHECK(text_of(fixture,
"pack p; string s=p.open(\"none.zpk\", \"k\")+\",\"+p.error+\",\"+p.is_open;"
"s+=\"|\"+p.add(\"a\", \"b\")+\",\"+p.error+\",\"+p.exists(\"a\")+\",\"+p.count+\",\"+p.size(\"a\")+\",\"+p.wasted_bytes+\",\"+p.list().length();"
"if(!p.open(\"w.zpk\", \"right\", true))return p.error;"
"p.add(\"a\", \"b\");"
"pack q; s+=\"|\"+q.open(\"w.zpk\", \"wrong\")+\",\"+q.error;"
"write_file(\"plain.txt\", \"not a pack at all, but long enough to have a header, an index and a footer in it, which it has not: more bytes here\");"
"s+=\"|\"+q.open(\"plain.txt\", \"k\")+\",\"+q.error;"
"return s;")==
"false,cannot read \"none.zpk\": file not found,false|false,pack is not open,false,0,-1,0,0|false,wrong key or damaged pack|false,wrong key or damaged pack");
}

TEST_CASE("the script pack leaves deletion markers out of exists and list")
{
ScriptFixture fixture;
fs::create_directories(fixture.root());
{
const auto storage=make(fixture.root()/"marked.zpk",std::string("k"));
add(*storage,"a","alpha");
add(*storage,"b","beta");
REQUIRE(storage->add_deletion_marker("b"));
REQUIRE(storage->flush());
}
CHECK(text_of(fixture,"pack p; if(!p.open(\"marked.zpk\", \"k\"))return p.error; return p.exists(\"a\")+\",\"+p.exists(\"b\")+\",\"+p.list().join(\",\")+\",\"+p.count+\",\"+p.size(\"b\");")=="true,false,a,1,-1");
}
