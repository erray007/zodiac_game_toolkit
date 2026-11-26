#include <core/payload.hpp>
#include <core/path_utf8.hpp>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zodiac {
namespace {
constexpr std::string_view kMagic="ZDPAYLD1";
constexpr uint32_t kFormatVersion=2;
// Slicing-by-8 tables: kCrcTables[0] is the classic byte table of the reflected polynomial 0xEDB88320, kCrcTables[k] advances a byte through k more zero bytes.
constexpr auto kCrcTables=[]{
std::array<std::array<uint32_t,256>,8> tables{};
for(uint32_t i=0;i<256;++i){
uint32_t crc=i;
for(int bit=0;bit<8;++bit)crc=(crc&1u)?(crc>>1)^0xEDB88320u:crc>>1;
tables[0][i]=crc;
}
for(std::size_t slice=1;slice<tables.size();++slice)for(uint32_t i=0;i<256;++i)tables[slice][i]=(tables[slice-1][i]>>8)^tables[0][tables[slice-1][i]&0xFFu];
return tables;
}();
constexpr uint32_t load_le32(const unsigned char* p) noexcept
{
return static_cast<uint32_t>(p[0])|(static_cast<uint32_t>(p[1])<<8)|(static_cast<uint32_t>(p[2])<<16)|(static_cast<uint32_t>(p[3])<<24);
}
uint32_t checked_u32(std::size_t value)
{
if(value>std::numeric_limits<uint32_t>::max())throw std::length_error("game payload field does not fit in 32 bits");
return static_cast<uint32_t>(value);
}
std::string generic_utf8(const std::filesystem::path& path)
{
const std::u8string text=path.generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
// Appends little-endian fields to a buffer reserved once for the exact payload size.
class Writer{
public:
explicit Writer(std::size_t size){out_.reserve(size);}
void u32(uint32_t value)
{
const char bytes[4]={static_cast<char>(value&0xFFu),static_cast<char>((value>>8)&0xFFu),static_cast<char>((value>>16)&0xFFu),static_cast<char>(value>>24)};
out_.append(bytes,sizeof bytes);
}
void count(std::size_t value){u32(checked_u32(value));}
void str(std::string_view text)
{
u32(checked_u32(text.size()));
out_.append(text);
}
void raw(std::string_view bytes){out_.append(bytes);}
std::string take() noexcept{return std::move(out_);}
const std::string& bytes() const noexcept{return out_;}
private:
std::string out_;
};
// Reads little-endian fields from a view; every read checks the remaining size first and fails instead of reading past the end.
class Reader{
public:
explicit Reader(std::string_view bytes) noexcept:rest_(bytes){}
bool u32(uint32_t& value) noexcept
{
if(rest_.size()<4)return false;
value=load_le32(reinterpret_cast<const unsigned char*>(rest_.data()));
rest_.remove_prefix(4);
return true;
}
bool str(std::string& text)
{
uint32_t size=0;
if(!u32(size)||size>rest_.size())return false;
text.assign(rest_.data(),size);
rest_.remove_prefix(size);
return true;
}
// A count of items that each take at least min_item bytes; refusing impossible counts up front keeps reserve() bounded by the input size.
bool count(uint32_t& value,std::size_t min_item) noexcept{return u32(value)&&value<=rest_.size()/min_item;}
bool empty() const noexcept{return rest_.empty();}
private:
std::string_view rest_;
};
std::unexpected<std::string> damaged(std::string_view reason)
{
std::string line("error: the game data is damaged (");
line.append(reason).push_back(')');
return std::unexpected(std::move(line));
}
bool read_strings(Reader& in,std::vector<std::string>& out)
{
uint32_t count=0;
if(!in.count(count,4))return false;
out.resize(count);
for(std::string& text:out)if(!in.str(text))return false;
return true;
}
bool output_mode(uint32_t value,OutputMode& mode) noexcept
{
if(value>static_cast<uint32_t>(OutputMode::never))return false;
mode=static_cast<OutputMode>(value);
return true;
}
std::size_t strings_size(const std::vector<std::string>& texts) noexcept
{
std::size_t size=4;
for(const std::string& text:texts)size+=4+text.size();
return size;
}
}

uint32_t crc32(std::string_view bytes) noexcept
{
const auto& t=kCrcTables;
const auto* p=reinterpret_cast<const unsigned char*>(bytes.data());
std::size_t n=bytes.size();
uint32_t crc=0xFFFFFFFFu;
for(;n>=8;p+=8,n-=8){
const uint32_t low=crc^load_le32(p);
const uint32_t high=load_le32(p+4);
crc=t[7][low&0xFFu]^t[6][(low>>8)&0xFFu]^t[5][(low>>16)&0xFFu]^t[4][low>>24]^t[3][high&0xFFu]^t[2][(high>>8)&0xFFu]^t[1][(high>>16)&0xFFu]^t[0][high>>24];
}
for(;n>0;++p,--n)crc=(crc>>8)^t[0][(crc^*p)&0xFFu];
return ~crc;
}

std::string serialize_payload(const GamePayload& payload)
{
const std::string file=generic_utf8(payload.diagnostics.file);
std::size_t size=kMagic.size()+4+4+payload.kit_version.size()+4+payload.game_name.size()+strings_size(payload.requested)+4+strings_size(payload.core_exclude)+4+4+file.size()+4+4+4+4+4+payload.pack_key.size()+4+4;
for(const PayloadModule& module:payload.modules)size+=4+module.name.size()+4+module.version.size()+4;
for(const PayloadScript& script:payload.scripts)size+=4+script.section.size()+4+4+script.bytecode.size();
Writer out(size);
out.raw(kMagic);
out.u32(payload.format_version);
out.str(payload.kit_version);
out.str(payload.game_name);
out.count(payload.requested.size());
for(const std::string& name:payload.requested)out.str(name);
out.count(payload.modules.size());
for(const PayloadModule& module:payload.modules){
out.str(module.name);
out.str(module.version);
out.u32(static_cast<uint32_t>(module.link));
}
out.count(payload.core_exclude.size());
for(const std::string& name:payload.core_exclude)out.str(name);
out.u32(static_cast<uint32_t>(payload.diagnostics.console));
out.str(file);
out.u32(static_cast<uint32_t>(payload.diagnostics.dialog));
out.u32(static_cast<uint32_t>(payload.pack_mode));
out.u32(payload.pack_writable?1u:0u);
out.u32(payload.host_paths?1u:0u);
out.str(payload.pack_key);
out.count(payload.scripts.size());
for(const PayloadScript& script:payload.scripts){
out.str(script.section);
out.u32(script.is_main?1u:0u);
out.str(script.bytecode);
}
out.u32(crc32(out.bytes()));
return out.take();
}

std::expected<GamePayload,std::string> parse_payload(std::string_view bytes)
{
constexpr std::size_t minimum=kMagic.size()+4+4;
if(bytes.size()>=kMagic.size()&&!bytes.starts_with(kMagic))return damaged("unknown format");
if(bytes.size()<minimum)return damaged("truncated");
const std::string_view body=bytes.substr(0,bytes.size()-4);
if(crc32(body)!=load_le32(reinterpret_cast<const unsigned char*>(bytes.data()+body.size())))return damaged("checksum mismatch");
Reader in(body.substr(kMagic.size()));
GamePayload payload;
if(!in.u32(payload.format_version))return damaged("truncated");
if(payload.format_version!=kFormatVersion){
char digits[16];
const auto end=std::to_chars(digits,digits+sizeof digits,payload.format_version).ptr;
std::string reason("unsupported version ");
reason.append(digits,end);
return damaged(reason);
}
if(!in.str(payload.kit_version)||!in.str(payload.game_name)||!read_strings(in,payload.requested))return damaged("truncated");
uint32_t count=0;
if(!in.count(count,12))return damaged("truncated");
payload.modules.resize(count);
for(PayloadModule& module:payload.modules){
uint32_t link=0;
if(!in.str(module.name)||!in.str(module.version)||!in.u32(link))return damaged("truncated");
if(link!=static_cast<uint32_t>(LinkMode::static_link)&&link!=static_cast<uint32_t>(LinkMode::dynamic_link))return damaged("invalid module link mode");
module.link=static_cast<LinkMode>(link);
}
if(!read_strings(in,payload.core_exclude))return damaged("truncated");
uint32_t console=0;
uint32_t dialog=0;
std::string file;
if(!in.u32(console)||!in.str(file)||!in.u32(dialog))return damaged("truncated");
if(!output_mode(console,payload.diagnostics.console)||!output_mode(dialog,payload.diagnostics.dialog))return damaged("invalid diagnostics setting");
payload.diagnostics.file=path_from_utf8(file);
uint32_t pack_mode=0;
uint32_t pack_writable=0;
uint32_t host_paths=0;
if(!in.u32(pack_mode)||!in.u32(pack_writable)||!in.u32(host_paths)||!in.str(payload.pack_key))return damaged("truncated");
if(pack_mode>static_cast<uint32_t>(PackMode::external))return damaged("invalid pack mode");
if(pack_writable>1||host_paths>1)return damaged("invalid pack or file setting");
if(!payload.pack_key.empty()&&payload.pack_key.size()!=kPayloadPackKeySize)return damaged("invalid pack key");
payload.pack_mode=static_cast<PackMode>(pack_mode);
payload.pack_writable=pack_writable==1;
payload.host_paths=host_paths==1;
if(!in.count(count,12))return damaged("truncated");
payload.scripts.resize(count);
for(PayloadScript& script:payload.scripts){
uint32_t is_main=0;
if(!in.str(script.section)||!in.u32(is_main)||!in.str(script.bytecode))return damaged("truncated");
if(is_main>1)return damaged("invalid script flag");
script.is_main=is_main==1;
}
if(!in.empty())return damaged("unexpected data at the end");
return payload;
}
}
