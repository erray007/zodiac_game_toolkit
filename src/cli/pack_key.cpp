// The release pack key file (contract in cli/pack_key.hpp).
#include <cli/pack_key.hpp>
#include <core/text.hpp>
#include <platform/console.hpp>
#include <platform/file.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace zodiac::cli {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t kHexDigits=64;
// The digits, "\r\n": the longest file that can hold a key.
constexpr std::size_t kLongestFile=kHexDigits+2;
constexpr char kKeyName[]=".zodiac/pack.key";
constexpr char kIgnoreName[]=".zodiac/.gitignore";

std::string damaged()
{
return std::string("error: ")+kKeyName+" is damaged (expected 64 hex digits); restore it from your backup";
}
std::string cannot(std::string_view verb,std::string_view name,std::string_view reason)
{
std::string line="error: cannot ";
line.append(verb).append(" \"").append(name).append("\": ").append(reason);
return line;
}
std::string cannot(std::string_view verb,std::string_view name,const std::error_code& error)
{
return cannot(verb,name,one_line(error_text(error)));
}
int hex_value(char c) noexcept
{
if(c>='0'&&c<='9')return c-'0';
if(c>='a'&&c<='f')return c-'a'+10;
if(c>='A'&&c<='F')return c-'A'+10;
return -1;
}

// Wipes a buffer of key text when it goes out of scope.
template<std::size_t N>
struct SecretText{
std::array<char,N> bytes{};
~SecretText(){crypto::wipe(bytes.data(),bytes.size());}
};

// The key in the file at path. When the file cannot be opened the error is empty and open_error holds the system's failure;
// every other error is the whole line.
std::expected<crypto::Key,std::string> read_key(const fs::path& path,std::error_code& open_error)
{
auto file=platform::File::open(path,platform::FileAccess::read);
if(!file){
open_error=file.error();
return std::unexpected(std::string());
}
// One byte more than the longest valid file, so a longer one is seen.
SecretText<kLongestFile+1> text;
std::size_t size=0;
while(size<text.bytes.size()){
const std::int64_t got=file->read_at(size,std::as_writable_bytes(std::span(text.bytes)).subspan(size));
if(got<0)return std::unexpected(cannot("read",kKeyName,std::error_code(static_cast<int>(-got),std::generic_category())));
if(got==0)break;
size+=static_cast<std::size_t>(got);
}
std::string_view line(text.bytes.data(),size);
if(line.ends_with("\r\n"))line.remove_suffix(2);
else if(line.ends_with('\n'))line.remove_suffix(1);
if(line.size()!=kHexDigits)return std::unexpected(damaged());
crypto::Key key;
for(std::size_t i=0;i<key.size();++i){
const int high=hex_value(line[2*i]);
const int low=hex_value(line[2*i+1]);
if(high<0||low<0){
crypto::wipe(key.data(),key.size());
return std::unexpected(damaged());
}
key[i]=static_cast<std::uint8_t>(high<<4|low);
}
return key;
}

// Adds a "pack.key" line to .zodiac/.gitignore unless one is there.
std::expected<void,std::string> ignore_key(const fs::path& folder)
{
const fs::path path=folder/".gitignore";
auto file=platform::File::open(path,platform::FileAccess::create_or_open);
if(!file)return std::unexpected(cannot("write",kIgnoreName,file.error()));
const std::uint64_t size=file->size();
std::string text(static_cast<std::size_t>(size),'\0');
if(size>0){
const std::int64_t got=file->read_at(0,std::as_writable_bytes(std::span(text)));
if(got<0)return std::unexpected(cannot("read",kIgnoreName,std::error_code(static_cast<int>(-got),std::generic_category())));
text.resize(static_cast<std::size_t>(got));
}
for(std::string_view rest=text;!rest.empty();){
const std::size_t end=rest.find('\n');
std::string_view line=rest.substr(0,end);
if(line.ends_with('\r'))line.remove_suffix(1);
if(line=="pack.key"||line=="/pack.key")return {};
if(end==std::string_view::npos)break;
rest.remove_prefix(end+1);
}
std::string added;
if(!text.empty()&&!text.ends_with('\n'))added.push_back('\n');
added.append("pack.key\n");
if(auto written=file->write_at(text.size(),std::as_bytes(std::span(added)));!written)return std::unexpected(cannot("write",kIgnoreName,written.error()));
return {};
}

std::expected<crypto::Key,std::string> create_key(const fs::path& folder,const fs::path& path)
{
std::error_code error;
fs::create_directories(folder,error);
if(error)return std::unexpected(cannot("write",kKeyName,error));
if(auto ignored=ignore_key(folder);!ignored)return std::unexpected(std::move(ignored.error()));
crypto::Key key;
try{
crypto::random_bytes(std::as_writable_bytes(std::span(key)));
}
catch(const std::runtime_error&){
return std::unexpected(cannot("write",kKeyName,"the entropy source failed"));
}
SecretText<kHexDigits+1> text;
constexpr char digits[]="0123456789abcdef";
for(std::size_t i=0;i<key.size();++i){
text.bytes[2*i]=digits[key[i]>>4];
text.bytes[2*i+1]=digits[key[i]&0xFu];
}
text.bytes[kHexDigits]='\n';
const fs::path temporary=folder/"pack.key.tmp";
// A temporary file a crashed build left behind holds no key anyone used: it never got the real name.
platform::discard_file(temporary);
const auto fail=[&](std::string line)->std::expected<crypto::Key,std::string>{
crypto::wipe(key.data(),key.size());
platform::discard_file(temporary);
return std::unexpected(std::move(line));
};
{
auto file=platform::File::open(temporary,platform::FileAccess::create_private);
if(!file)return fail(cannot("write",kKeyName,file.error()));
if(auto written=file->write_at(0,std::as_bytes(std::span(text.bytes)));!written)return fail(cannot("write",kKeyName,written.error()));
if(auto synced=file->sync_data();!synced)return fail(cannot("write",kKeyName,synced.error()));
}
fs::create_hard_link(temporary,path,error);
if(error){
// Made by another build in between: that key is the game's, and this one was never used.
std::error_code exists_error;
if(fs::exists(path,exists_error)){
crypto::wipe(key.data(),key.size());
platform::discard_file(temporary);
std::error_code open_error;
auto existing=read_key(path,open_error);
if(!existing&&existing.error().empty())return std::unexpected(cannot("read",kKeyName,open_error));
return existing;
}
// No hard links on this file system: a rename, now that the name was seen free.
error.clear();
fs::rename(temporary,path,error);
if(error)return fail(cannot("write",kKeyName,error));
}
platform::discard_file(temporary);
const std::error_code synced=platform::sync_directory(folder);
// The key has its name now, so the developer hears about it even when the folder's flush failed (and the build stops).
platform::write_stdout_line("created .zodiac/pack.key; back it up, later builds and patches need it");
if(synced)return fail(cannot("write",kKeyName,synced));
return key;
}
}

std::expected<crypto::Key,std::string> load_or_create_pack_key(const fs::path& root)
{
const fs::path folder=root/".zodiac";
const fs::path path=folder/"pack.key";
std::error_code open_error;
auto key=read_key(path,open_error);
if(key||!key.error().empty())return key;
if(platform::error_kind(open_error)!=platform::ErrorKind::not_found)return std::unexpected(cannot("read",kKeyName,open_error));
return create_key(folder,path);
}
}
