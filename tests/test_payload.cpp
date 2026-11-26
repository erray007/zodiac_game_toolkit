#include <core/payload.hpp>
#include <doctest/doctest.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
zodiac::GamePayload sample()
{
zodiac::GamePayload payload;
payload.kit_version="0.3.0";
payload.game_name="Kara \xC5\x9F\x65hir \xE2\x98\x85";
payload.requested={"example","dep_child"};
payload.modules={{"example","1.2.3",zodiac::LinkMode::static_link},{"dep_child","0.1.0",zodiac::LinkMode::dynamic_link}};
payload.core_exclude={"window"};
payload.diagnostics.console=zodiac::OutputMode::always;
payload.diagnostics.file="logs/game.log";
payload.diagnostics.dialog=zodiac::OutputMode::never;
payload.pack_mode=zodiac::PackMode::external;
payload.pack_writable=true;
payload.host_paths=true;
payload.pack_key=std::string("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\xF0\xF1\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\xFA\xFB\xFC\xFD\xFE\xFF",32);
payload.build_id="0123456789abcdef0123456789abcdef";
payload.scripts={{"main.zs",true,std::string("\x00\x01\xFF bytecode \x00",14)}};
return payload;
}
}

TEST_CASE("crc32 matches the IEEE check values")
{
CHECK(zodiac::crc32("")==0x00000000u);
CHECK(zodiac::crc32("a")==0xE8B7BE43u);
CHECK(zodiac::crc32("123456789")==0xCBF43926u);
CHECK(zodiac::crc32("The quick brown fox jumps over the lazy dog")==0x414FA339u);
}

TEST_CASE("payload round trip keeps every field")
{
const zodiac::GamePayload input=sample();
const std::string bytes=zodiac::serialize_payload(input);
CHECK(bytes.starts_with("ZDPAYLD1"));
const auto parsed=zodiac::parse_payload(bytes);
INFO(parsed.error_or(""));
REQUIRE(parsed.has_value());
CHECK(*parsed==input);
CHECK(parsed->format_version==2);
CHECK(parsed->pack_mode==zodiac::PackMode::external);
CHECK(parsed->pack_writable);
CHECK(parsed->host_paths);
CHECK(parsed->pack_key==input.pack_key);
CHECK(parsed->build_id==input.build_id);
}

TEST_CASE("payload defaults describe a release without a pack or host paths")
{
const zodiac::GamePayload defaults;
CHECK(defaults.format_version==2);
CHECK(defaults.pack_mode==zodiac::PackMode::none);
CHECK_FALSE(defaults.pack_writable);
CHECK_FALSE(defaults.host_paths);
CHECK(defaults.pack_key.empty());
}

TEST_CASE("payload round trips every pack mode and flag")
{
for(const zodiac::PackMode mode:{zodiac::PackMode::none,zodiac::PackMode::embedded,zodiac::PackMode::external}){
for(const bool writable:{false,true}){
for(const bool host_paths:{false,true}){
CAPTURE(static_cast<int>(mode));
CAPTURE(writable);
CAPTURE(host_paths);
zodiac::GamePayload input=sample();
input.pack_mode=mode;
input.pack_writable=writable;
input.host_paths=host_paths;
input.pack_key=mode==zodiac::PackMode::none?std::string():std::string(32,'k');
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(input));
INFO(parsed.error_or(""));
REQUIRE(parsed.has_value());
CHECK(*parsed==input);
}
}
}
}

namespace {
// The payload's bytes with the CRC recomputed, so a test reaches the check after it.
std::string resealed(std::string bytes)
{
bytes.resize(bytes.size()-4);
const std::uint32_t crc=zodiac::crc32(bytes);
for(int i=0;i<4;++i)bytes.push_back(static_cast<char>((crc>>(8*i))&0xFFu));
return bytes;
}
// Offset of the u32 pack_mode: right after the diagnostics file and dialog of sample(), which the test finds by its text.
std::size_t pack_mode_offset(const std::string& bytes)
{
const std::size_t file=bytes.find("logs/game.log");
REQUIRE(file!=std::string::npos);
return file+std::string_view("logs/game.log").size()+4;
}
void put_u32(std::string& bytes,std::size_t at,std::uint32_t value)
{
for(int i=0;i<4;++i)bytes[at+static_cast<std::size_t>(i)]=static_cast<char>((value>>(8*i))&0xFFu);
}
}

TEST_CASE("only format version 2 is read")
{
for(const std::uint32_t version:{0u,1u,3u,0xFFFFFFFFu}){
CAPTURE(version);
zodiac::GamePayload input=sample();
input.format_version=version;
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(input));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (unsupported version "+std::to_string(version)+")");
}
}

TEST_CASE("a pack key of another size than 32 bytes is damage")
{
for(const std::size_t size:{std::size_t{1},std::size_t{31},std::size_t{33},std::size_t{64}}){
CAPTURE(size);
zodiac::GamePayload input=sample();
input.pack_key.assign(size,'k');
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(input));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (invalid pack key)");
}
}

TEST_CASE("a build id that is not empty or 32 lowercase hex digits is damage")
{
for(const std::string& id:{std::string("abc"),std::string(33,'a'),std::string("0123456789ABCDEF0123456789abcdef"),std::string("0123456789abcdef0123456789abcdeg")}){
CAPTURE(id);
zodiac::GamePayload input=sample();
input.build_id=id;
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(input));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (invalid build id)");
}
zodiac::GamePayload none=sample();
none.build_id.clear();
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(none));
REQUIRE(parsed.has_value());
CHECK(parsed->build_id.empty());
}

TEST_CASE("a pack mode or flag out of range is damage")
{
const std::string bytes=zodiac::serialize_payload(sample());
const std::size_t mode=pack_mode_offset(bytes);
{
std::string bad=bytes;
put_u32(bad,mode,3);
const auto parsed=zodiac::parse_payload(resealed(bad));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (invalid pack mode)");
}
for(const std::size_t field:{mode+4,mode+8}){
CAPTURE(field-mode);
std::string bad=bytes;
put_u32(bad,field,2);
const auto parsed=zodiac::parse_payload(resealed(bad));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (invalid pack or file setting)");
}
// The offsets are the fields': written back as they were, the payload reads again.
std::string same=bytes;
put_u32(same,mode,static_cast<std::uint32_t>(zodiac::PackMode::external));
put_u32(same,mode+4,1);
put_u32(same,mode+8,1);
CHECK(zodiac::parse_payload(resealed(same)).has_value());
}

TEST_CASE("payload with no modules and no scripts round trips")
{
zodiac::GamePayload input;
input.game_name="g";
const auto parsed=zodiac::parse_payload(zodiac::serialize_payload(input));
REQUIRE(parsed.has_value());
CHECK(*parsed==input);
}

TEST_CASE("every flipped byte is reported as damage")
{
const std::string bytes=zodiac::serialize_payload(sample());
for(std::size_t i=0;i<bytes.size();++i){
for(const unsigned char mask:{0x01u,0x80u,0xFFu}){
std::string damaged=bytes;
damaged[i]=static_cast<char>(static_cast<unsigned char>(damaged[i])^mask);
const auto parsed=zodiac::parse_payload(damaged);
INFO("byte ",i," mask ",static_cast<unsigned>(mask));
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error().starts_with("error: the game data is damaged"));
CHECK(parsed.error().find('\n')==std::string::npos);
}
}
}

TEST_CASE("every truncation is reported as damage")
{
const std::string bytes=zodiac::serialize_payload(sample());
for(std::size_t size=0;size<bytes.size();++size){
// A separate buffer of exactly size bytes, so a read past the end is caught by sanitizers and debug iterators.
const std::string truncated=bytes.substr(0,size);
const auto parsed=zodiac::parse_payload(truncated);
INFO("size ",size);
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error().starts_with("error: the game data is damaged"));
}
}

TEST_CASE("checksum mismatch is named")
{
std::string bytes=zodiac::serialize_payload(sample());
bytes.back()=static_cast<char>(bytes.back()^0x10);
const auto parsed=zodiac::parse_payload(bytes);
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error()=="error: the game data is damaged (checksum mismatch)");
}

TEST_CASE("appended bytes are reported as damage")
{
const std::string bytes=zodiac::serialize_payload(sample())+"x";
const auto parsed=zodiac::parse_payload(bytes);
REQUIRE_FALSE(parsed.has_value());
CHECK(parsed.error().starts_with("error: the game data is damaged"));
}
