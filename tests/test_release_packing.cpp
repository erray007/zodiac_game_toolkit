// zodiac build's release packing and pack key (src/cli/release_packing.hpp, src/cli/pack_key.hpp; spec 8.4 of the library-and-
// packs design): a large asset is streamed into the pack (no allocation near its size), the manifest records it, the key file is
// made once, a crash's leftover temporary name of it is removed and the folder stays ignored by git, and a damaged key is kept.
#include "sparse_file.hpp"
#include <cli/pack_key.hpp>
#include <cli/release_packing.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/project_file.hpp>
#include <doctest/doctest.h>
#include <platform/entropy.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The largest single allocation through the replaceable operator new, so a test shows no buffer of an asset's size was made.
namespace {
std::size_t largest_allocation=0;
}
void* operator new(std::size_t size)
{
if(size>largest_allocation)largest_allocation=size;
if(void* memory=std::malloc(size?size:1))return memory;
throw std::bad_alloc();
}
void* operator new[](std::size_t size)
{
return ::operator new(size);
}
void operator delete(void* memory) noexcept
{
std::free(memory);
}
void operator delete[](void* memory) noexcept
{
std::free(memory);
}
void operator delete(void* memory,std::size_t) noexcept
{
std::free(memory);
}
void operator delete[](void* memory,std::size_t) noexcept
{
std::free(memory);
}

namespace fs=std::filesystem;

namespace {
struct TempDir{
fs::path path;
TempDir()
{
std::uint64_t id=0;
REQUIRE(zodiac::platform::fill_entropy(&id,sizeof id));
path=fs::temp_directory_path()/("zodiac_release_packing_"+std::to_string(id));
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
std::string read_all(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
void write_all(const fs::path& path,std::string_view bytes)
{
fs::create_directories(path.parent_path());
std::ofstream out(path,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
REQUIRE(out.good());
}
std::vector<std::string> lines_of(std::string_view text)
{
std::vector<std::string> lines;
while(!text.empty()){
const std::size_t end=text.find('\n');
lines.emplace_back(text.substr(0,end));
if(end==std::string_view::npos)break;
text.remove_prefix(end+1);
}
return lines;
}
}

TEST_CASE("a large asset is streamed into the pack without a buffer of its size")
{
const TempDir temp;
zodiac::Project project;
project.root=temp.path;
project.name="big";
// 256 MiB + 7 bytes, a hole but for the last bytes: far more than any buffer the packing may make.
constexpr std::uint64_t kSize=(std::uint64_t{256}<<20)+7;
const fs::path asset=temp.path/"music"/"theme.ogg";
fs::create_directories(asset.parent_path());
zodiac_test::make_sparse(asset,kSize-7,"the end");
const fs::path small=temp.path/"music"/"a.ogg";
write_all(small,"a");
fs::create_directories(temp.path/".zodiac");
zodiac::cli::AssetSplit split;
split.packed={small,asset};
zodiac::crypto::Key key{};
key[0]=1;
const std::string build_id(32,'7');
const fs::path pack_file=temp.path/"big.zpk.tmp";
largest_allocation=0;
const auto written=zodiac::cli::write_release_pack(project,key,split,pack_file,build_id);
const std::size_t peak=largest_allocation;
INFO(written.error_or(""));
REQUIRE(written);
CAPTURE(peak);
CHECK(peak<(std::size_t{8}<<20));
auto storage=zodiac::pack::PackStorage::open(pack_file,zodiac::pack::KeySource(key),false);
REQUIRE(storage);
CHECK((*storage)->metadata()=="build="+build_id);
const auto entry=(*storage)->stat("music/theme.ogg");
REQUIRE(entry);
CHECK(entry->size==kSize);
auto reader=(*storage)->open_reader("music/theme.ogg");
REQUIRE(reader);
std::string tail(7,'\0');
CHECK((*reader)->read_at(kSize-7,std::as_writable_bytes(std::span(tail)))==7);
CHECK(tail=="the end");
REQUIRE((*storage)->close());
// The manifest (still its temporary file until the release is in place), sorted by path.
const std::vector<std::string> manifest=lines_of(read_all(temp.path/".zodiac"/"release.manifest.tmp"));
REQUIRE(manifest.size()==5);
CHECK(manifest[0]=="zodiac-release-manifest 1");
CHECK(manifest[1]=="build "+build_id);
CHECK(manifest[2]=="next_patch 1");
CHECK(manifest[3].ends_with(" 1 music/a.ogg"));
CHECK(manifest[4].ends_with(" "+std::to_string(kSize)+" music/theme.ogg"));
REQUIRE(zodiac::cli::commit_release_manifest(project));
CHECK(fs::exists(temp.path/".zodiac"/"release.manifest"));
CHECK_FALSE(fs::exists(temp.path/".zodiac"/"release.manifest.tmp"));
}

TEST_CASE("the pack key is made once, a leftover temporary name goes, and a damaged key is kept")
{
const TempDir temp;
const fs::path folder=temp.path/".zodiac";
auto made=zodiac::cli::load_or_create_pack_key(temp.path);
INFO(made.error_or(""));
REQUIRE(made);
const std::string text=read_all(folder/"pack.key");
REQUIRE(text.size()==65);
CHECK(text.back()=='\n');
CHECK(lines_of(read_all(folder/".gitignore"))==std::vector<std::string>{"*"});
CHECK_FALSE(fs::exists(folder/"pack.key.tmp"));
// A crash between the link and the removal leaves the key under its temporary name too: the next load removes it.
write_all(folder/"pack.key.tmp",text);
// A project from before the "*" line gets it.
write_all(folder/".gitignore","pack.key\n");
auto again=zodiac::cli::load_or_create_pack_key(temp.path);
REQUIRE(again);
CHECK(*again==*made);
CHECK_FALSE(fs::exists(folder/"pack.key.tmp"));
CHECK(lines_of(read_all(folder/".gitignore"))==std::vector<std::string>{"pack.key","*"});
CHECK(read_all(folder/"pack.key")==text);
// Upper-case digits and a CRLF line end are the same key.
std::string upper=text.substr(0,64);
for(char& c:upper)if(c>='a'&&c<='f')c=static_cast<char>(c-'a'+'A');
write_all(folder/"pack.key",upper+"\r\n");
auto read_back=zodiac::cli::load_or_create_pack_key(temp.path);
REQUIRE(read_back);
CHECK(*read_back==*made);
// Damaged: refused, never replaced.
for(const std::string& damaged:{std::string("not a key\n"),text.substr(0,63)+"\n",text.substr(0,64)+"0\n",std::string()}){
write_all(folder/"pack.key",damaged);
auto refused=zodiac::cli::load_or_create_pack_key(temp.path);
REQUIRE_FALSE(refused);
CHECK(refused.error()=="error: .zodiac/pack.key is damaged (expected 64 hex digits); restore it from your backup");
CHECK(read_all(folder/"pack.key")==damaged);
}
// With no key, a leftover temporary file is a key no build used: it goes, and a new key is made.
fs::remove(folder/"pack.key");
write_all(folder/"pack.key.tmp","stale");
auto fresh=zodiac::cli::load_or_create_pack_key(temp.path);
REQUIRE(fresh);
CHECK(*fresh!=*made);
CHECK_FALSE(fs::exists(folder/"pack.key.tmp"));
}
