#include <core/vfs/path.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <core/types/unicode.hpp>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace zodiac::vfs {
namespace {
namespace fs=std::filesystem;
using byte=unsigned char;
bool is_separator(char c) noexcept
{
return c=='/'||c=='\\';
}
bool is_ascii_letter(char c) noexcept
{
return static_cast<byte>((c|0x20)-'a')<26;
}
char ascii_upper(char c) noexcept
{
return static_cast<byte>(c-'a')<26?static_cast<char>(c-32):c;
}
// text equals upper (ASCII capitals and symbols) ignoring ASCII case.
bool equals_upper(std::string_view text,std::string_view upper) noexcept
{
if(text.size()!=upper.size())return false;
for(std::size_t i=0;i<text.size();++i)if(ascii_upper(text[i])!=upper[i])return false;
return true;
}
bool starts_with_upper(std::string_view text,std::string_view upper) noexcept
{
return text.size()>=upper.size()&&equals_upper(text.substr(0,upper.size()),upper);
}
bool is_device_stem(std::string_view stem) noexcept
{
while(!stem.empty()&&stem.back()==' ')stem.remove_suffix(1);
switch(stem.size()){
case 3:
return equals_upper(stem,"CON")||equals_upper(stem,"PRN")||equals_upper(stem,"AUX")||equals_upper(stem,"NUL");
case 4:
return (equals_upper(stem.substr(0,3),"COM")||equals_upper(stem.substr(0,3),"LPT"))&&static_cast<byte>(stem[3]-'0')<10;
case 5:
// COM and LPT with a superscript one, two or three: U+00B9, U+00B2 and U+00B3, two UTF-8 bytes each.
return (equals_upper(stem.substr(0,3),"COM")||equals_upper(stem.substr(0,3),"LPT"))&&stem[3]=='\xC2'&&(stem[4]=='\xB9'||stem[4]=='\xB2'||stem[4]=='\xB3');
case 6:
return equals_upper(stem,"CONIN$")||equals_upper(stem,"CLOCK$");
case 7:
return equals_upper(stem,"CONOUT$");
default:
return false;
}
}
bool is_continuation(byte b) noexcept
{
return (b&0xC0)==0x80;
}
// Bytes of the well-formed UTF-8 sequence at p (Unicode Table 3-7), or 1 for a byte that starts none.
std::size_t sequence_size(const byte* p,const byte* end) noexcept
{
const byte lead=*p;
if(lead<0xC2)return 1;
const std::ptrdiff_t left=end-p;
if(lead<0xE0)return left>=2&&is_continuation(p[1])?2:1;
if(lead<0xF0){
if(left<3)return 1;
const byte low=lead==0xE0?0xA0:0x80;
const byte high=lead==0xED?0x9F:0xBF;
return p[1]>=low&&p[1]<=high&&is_continuation(p[2])?3:1;
}
if(lead<0xF5){
if(left<4)return 1;
const byte low=lead==0xF0?0x90:0x80;
const byte high=lead==0xF4?0x8F:0xBF;
return p[1]>=low&&p[1]<=high&&is_continuation(p[2])&&is_continuation(p[3])?4:1;
}
return 1;
}
char32_t decode(const byte* p,std::size_t size) noexcept
{
switch(size){
case 2:
return static_cast<char32_t>(((p[0]&0x1Fu)<<6)|(p[1]&0x3Fu));
case 3:
return static_cast<char32_t>(((p[0]&0x0Fu)<<12)|((p[1]&0x3Fu)<<6)|(p[2]&0x3Fu));
default:
return static_cast<char32_t>(((p[0]&0x07u)<<18)|((p[1]&0x3Fu)<<12)|((p[2]&0x3Fu)<<6)|(p[3]&0x3Fu));
}
}
// to_upper maps scalar values to scalar values, so c is never a surrogate or above U+10FFFF here.
void append_utf8(std::string& out,char32_t c)
{
char bytes[4];
std::size_t size;
if(c<0x80){
bytes[0]=static_cast<char>(c);
size=1;
}
else if(c<0x800){
bytes[0]=static_cast<char>(0xC0|(c>>6));
bytes[1]=static_cast<char>(0x80|(c&0x3F));
size=2;
}
else if(c<0x10000){
bytes[0]=static_cast<char>(0xE0|(c>>12));
bytes[1]=static_cast<char>(0x80|((c>>6)&0x3F));
bytes[2]=static_cast<char>(0x80|(c&0x3F));
size=3;
}
else{
bytes[0]=static_cast<char>(0xF0|(c>>18));
bytes[1]=static_cast<char>(0x80|((c>>12)&0x3F));
bytes[2]=static_cast<char>(0x80|((c>>6)&0x3F));
bytes[3]=static_cast<char>(0x80|(c&0x3F));
size=4;
}
out.append(bytes,size);
}
std::string quote_path(std::string_view path,std::string_view rest)
{
std::string message;
message.reserve(path.size()+rest.size()+2);
message.push_back('"');
message.append(one_line(path)).push_back('"');
message.append(rest);
return message;
}
std::unexpected<std::string> invalid_name(std::string_view path)
{
return std::unexpected(quote_path(path," is not a valid file name"));
}
bool well_formed(std::string_view text) noexcept
{
const byte* p=reinterpret_cast<const byte*>(text.data());
const byte* const end=p+text.size();
while(p<end){
if(*p<0x80){
++p;
continue;
}
const std::size_t size=sequence_size(p,end);
if(size==1)return false;
p+=size;
}
return true;
}
std::expected<VfsPath,std::string> parse_host(std::string_view path,std::string_view text)
{
VfsPath result;
result.mount="host:";
// Checked before any conversion: ill-formed UTF-8 would make the wide conversion throw on Windows.
if(!well_formed(text))return invalid_name(path);
// The Win32 device namespaces (\\.\, \\?\ and their '/' forms) reach devices and raw volumes, never files.
if(text.size()>=3&&is_separator(text[0])&&is_separator(text[1])&&(text[2]=='.'||text[2]=='?')&&(text.size()==3||is_separator(text[3])))return invalid_name(path);
result.folder=text.empty()||is_separator(text.back())||text.ends_with("/.")||text.ends_with("\\.")||text.ends_with("/..")||text.ends_with("\\..")||text=="."||text=="..";
fs::path host=path_from_utf8(text);
if(!host.is_absolute()){
std::error_code error;
host=fs::absolute(host,error);
if(error)return invalid_name(path);
}
host=host.lexically_normal();
if(host.has_relative_path()&&!host.has_filename())host=host.parent_path();
for(const fs::path& part:host.relative_path()){
const std::string name=path_to_utf8(part);
if(name=="."||name=="..")continue;
if(!valid_name(name))return invalid_name(path);
}
const std::u8string generic=host.generic_u8string();
result.relative.assign(reinterpret_cast<const char*>(generic.data()),generic.size());
if(!host.has_relative_path())result.folder=true;
result.host=std::move(host);
return result;
}
}

bool is_temp_name(std::string_view name) noexcept
{
if(name.size()<temp_prefix.size())return false;
// ASCII case-insensitive, as Windows compares names.
for(std::size_t i=0;i<temp_prefix.size();++i){
const char c=name[i];
if((c>='A'&&c<='Z'?static_cast<char>(c+32):c)!=temp_prefix[i])return false;
}
return true;
}

namespace {
constexpr char hex_digits[]="0123456789abcdef";
constexpr std::string_view temp_suffix=".tmp";
std::mutex held_mutex;
std::unordered_set<std::string>& held_temps()
{
static std::unordered_set<std::string> names;
return names;
}
}

std::string make_temp_name(std::int64_t unix_seconds,std::uint32_t random)
{
std::string name(temp_prefix);
const auto seconds=static_cast<std::uint32_t>(unix_seconds);
for(int shift=28;shift>=0;shift-=4)name.push_back(hex_digits[(seconds>>shift)&0xF]);
for(int shift=28;shift>=0;shift-=4)name.push_back(hex_digits[(random>>shift)&0xF]);
name.append(temp_suffix);
return name;
}

std::optional<std::int64_t> temp_name_time(std::string_view name) noexcept
{
if(name.size()!=temp_prefix.size()+16+temp_suffix.size()||!name.starts_with(temp_prefix)||!name.ends_with(temp_suffix))return std::nullopt;
std::uint32_t seconds=0;
for(std::size_t i=0;i<16;++i){
const char c=name[temp_prefix.size()+i];
const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
if(digit<0)return std::nullopt;
if(i<8)seconds=(seconds<<4)|static_cast<std::uint32_t>(digit);
}
return static_cast<std::int64_t>(seconds);
}

void hold_temp(const std::string& name)
{
const std::lock_guard lock(held_mutex);
held_temps().insert(name);
}

void release_temp(const std::string& name) noexcept
{
const std::lock_guard lock(held_mutex);
held_temps().erase(name);
}

bool temp_held(std::string_view name)
{
const std::lock_guard lock(held_mutex);
return held_temps().contains(std::string(name));
}

bool valid_name(std::string_view name) noexcept
{
if(name.empty()||name=="."||name=="..")return false;
const char last=name.back();
if(last=='.'||last==' ')return false;
if(is_temp_name(name))return false;
const byte* p=reinterpret_cast<const byte*>(name.data());
const byte* const end=p+name.size();
while(p<end){
const byte b=*p;
// Ill-formed UTF-8 names nothing on Windows (no UTF-16 for it) and a raw-byte name on Linux: refused on both.
if(b>=0x80){
const std::size_t size=sequence_size(p,end);
if(size==1)return false;
p+=size;
continue;
}
++p;
if(b<0x20||b==0x7F)return false;
switch(static_cast<char>(b)){
case '<':
case '>':
case ':':
case '"':
case '|':
case '?':
case '*':
case '/':
case '\\':
return false;
default:
break;
}
}
return !is_device_stem(name.substr(0,name.find('.')));
}

void append_fold(std::string& out,std::string_view utf8)
{
const byte* p=reinterpret_cast<const byte*>(utf8.data());
const byte* const end=p+utf8.size();
while(p<end){
if(*p<0x80){
out.push_back(ascii_upper(static_cast<char>(*p)));
++p;
continue;
}
const std::size_t size=sequence_size(p,end);
if(size==1){
out.push_back(static_cast<char>(*p));
++p;
continue;
}
const char32_t c=decode(p,size);
const char32_t upper=unicode::to_upper(c);
if(upper==c)out.append(reinterpret_cast<const char*>(p),size);
else append_utf8(out,upper);
p+=size;
}
}

std::string fold_name(std::string_view utf8)
{
std::string out;
out.reserve(utf8.size());
append_fold(out,utf8);
return out;
}

std::expected<VfsPath,std::string> parse_path(std::string_view path,bool host_paths)
{
const bool drive=path.size()>=2&&is_ascii_letter(path[0])&&path[1]==':';
const bool network=path.size()>=2&&is_separator(path[0])&&is_separator(path[1]);
const bool prefixed=starts_with_upper(path,"HOST:");
if(drive||network||prefixed){
if(!host_paths)return std::unexpected(quote_path(path," is a host path; set [files] host_paths = true to use it"));
return parse_host(path,prefixed?path.substr(5):path);
}
VfsPath result;
std::string_view rest=path;
if(starts_with_upper(rest,"DATA:")){
result.mount="data:";
rest.remove_prefix(5);
}
result.relative.reserve(rest.size());
result.folder=true;
std::size_t at=0;
while(at<rest.size()){
if(is_separator(rest[at])){
++at;
result.folder=true;
continue;
}
std::size_t cut=at;
while(cut<rest.size()&&!is_separator(rest[cut]))++cut;
const std::string_view segment=rest.substr(at,cut-at);
at=cut;
result.folder=false;
if(segment=="."){
result.folder=true;
continue;
}
if(segment==".."){
if(result.relative.empty())return std::unexpected(quote_path(path," leaves its mount"));
const std::size_t slash=result.relative.rfind('/');
result.relative.resize(slash==std::string::npos?0:slash);
result.folder=true;
continue;
}
if(!valid_name(segment))return invalid_name(path);
if(!result.relative.empty())result.relative.push_back('/');
result.relative.append(segment);
}
if(result.relative.empty())result.folder=true;
return result;
}
}
