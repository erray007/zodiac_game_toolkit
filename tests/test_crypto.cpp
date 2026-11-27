// Tests the pack cryptography (src/core/pack/crypto.hpp, spec 8.2 of the library-and-packs design):
// - every RFC 8439 test vector (2.3.2, 2.4.2, 2.5.2, 2.6.2, 2.8.2 and A.1 to A.5, transcribed in
//   tests/fixtures/crypto/rfc8439.txt) on every keystream compiled in that the CPU runs, and through seal and open;
// - open refusing every flipped bit of cipher, tag and aad, and a wrong size, with its output zeroed;
// - with the vectorized ChaCha20 compiled in, its keystream and seal against Monocypher's on 64 random buffers of a fixed
//   seed, at every length from 0 to 1024 and at 65536, from counters 0, 1 and 0xFFFFFFFE;
// - Poly1305 after poly1305-donna-64 (poly1305.hpp) against the RFC 8439 vectors and against Monocypher's on random inputs of every
//   length from 0 to 1024 and 65536 and on extreme keys and messages, in one call and block by block;
// - Argon2id through derive_password_key against single-lane vectors of Monocypher 4.0.2's test suite (generated from
//   libsodium), its determinism, its salt dependence and its limits; BLAKE2b-256 and the file key against Python's hashlib.
#include <core/library/random.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/poly1305.hpp>
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace crypto=zodiac::crypto;
using Bytes=std::vector<std::uint8_t>;

namespace {
int hex_digit(char c)
{
if(c>='0'&&c<='9')return c-'0';
if(c>='a'&&c<='f')return c-'a'+10;
if(c>='A'&&c<='F')return c-'A'+10;
return -1;
}
Bytes from_hex(std::string_view text)
{
Bytes bytes;
int high=-1;
for(const char c:text){
if(c==' '||c==':')continue;
const int digit=hex_digit(c);
REQUIRE(digit>=0);
if(high<0)high=digit;
else{
bytes.push_back(static_cast<std::uint8_t>(high*16+digit));
high=-1;
}
}
REQUIRE(high<0);
return bytes;
}
// Two hex digits per byte, bytes separated by single spaces (so a field name such as "aad" is not one).
bool is_hex_line(std::string_view line)
{
if(line.size()%3!=2)return false;
for(std::size_t i=0;i<line.size();++i){
if(i%3==2?line[i]!=' ':hex_digit(line[i])<0)return false;
}
return true;
}
struct Vector {
std::string kind;
std::string name;
std::uint32_t counter=0;
std::map<std::string,Bytes> fields;
const Bytes& operator[](const std::string& field) const
{
const auto found=fields.find(field);
INFO(name<<" has no field "<<field);
REQUIRE(found!=fields.end());
return found->second;
}
};
const std::vector<Vector>& vectors()
{
static const std::vector<Vector> all=[]{
std::vector<Vector> read;
std::ifstream file(ZODIAC_TEST_CRYPTO_VECTORS);
REQUIRE(file.is_open());
std::string line;
Vector* open=nullptr;
Bytes* field=nullptr;
while(std::getline(file,line)){
if(!line.empty()&&line.back()=='\r')line.pop_back();
if(line.empty()||line[0]=='#')continue;
if(line.starts_with("vector ")){
REQUIRE(open==nullptr);
std::istringstream words(line.substr(7));
Vector vector;
words>>vector.kind>>vector.name;
read.push_back(std::move(vector));
open=&read.back();
field=nullptr;
}
else if(line=="end"){
REQUIRE(open!=nullptr);
open=nullptr;
field=nullptr;
}
else if(line.starts_with("counter ")){
REQUIRE(open!=nullptr);
open->counter=static_cast<std::uint32_t>(std::stoul(line.substr(8)));
}
else if(is_hex_line(line)){
REQUIRE(field!=nullptr);
const Bytes bytes=from_hex(line);
field->insert(field->end(),bytes.begin(),bytes.end());
}
else{
REQUIRE(open!=nullptr);
INFO(line);
REQUIRE(open->fields.find(line)==open->fields.end());
field=&open->fields[line];
}
}
REQUIRE(open==nullptr);
return read;
}();
return all;
}
std::vector<const Vector*> vectors_of(std::string_view kind)
{
std::vector<const Vector*> found;
for(const Vector& vector:vectors())if(vector.kind==kind)found.push_back(&vector);
return found;
}
template<class Array> Array array_of(const Bytes& bytes)
{
Array array{};
REQUIRE(bytes.size()==array.size());
std::copy(bytes.begin(),bytes.end(),array.begin());
return array;
}
std::span<const std::byte> as_bytes(const Bytes& bytes)
{
return std::as_bytes(std::span(bytes));
}
std::span<std::byte> as_writable(Bytes& bytes)
{
return std::as_writable_bytes(std::span(bytes));
}
bool all_zero(const Bytes& bytes)
{
return std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t b){return b==0;});
}
// Every keystream compiled in that this CPU runs.
std::vector<const crypto::detail::Keystream*> keystreams()
{
std::vector<const crypto::detail::Keystream*> streams{&crypto::detail::reference};
#if defined(ZODIAC_CRYPTO_SIMD)
streams.push_back(&crypto::detail::sse2);
if(crypto::detail::cpu_has_avx2())streams.push_back(&crypto::detail::avx2);
#endif
return streams;
}
}

TEST_CASE("the RFC 8439 vector file holds every section's vectors")
{
CHECK(vectors_of("block").size()==6);
CHECK(vectors_of("encrypt").size()==4);
CHECK(vectors_of("poly1305").size()==12);
CHECK(vectors_of("polykey").size()==6);
CHECK(vectors_of("aead").size()==2);
}

TEST_CASE("every keystream gives the RFC 8439 block, encryption and Poly1305 key vectors")
{
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
for(const Vector* v:vectors_of("block")){
INFO(v->name);
const Bytes& expected=(*v)["keystream"];
Bytes out(expected.size(),0xAA);
stream->run(out.data(),nullptr,out.size(),(*v)["key"].data(),(*v)["nonce"].data(),v->counter);
CHECK(out==expected);
Bytes zeros(expected.size(),0);
stream->run(zeros.data(),zeros.data(),zeros.size(),(*v)["key"].data(),(*v)["nonce"].data(),v->counter);
CHECK(zeros==expected);
}
for(const Vector* v:vectors_of("encrypt")){
INFO(v->name);
const Bytes& plain=(*v)["plain"];
Bytes out(plain.size(),0xAA);
stream->run(out.data(),plain.data(),plain.size(),(*v)["key"].data(),(*v)["nonce"].data(),v->counter);
CHECK(out==(*v)["cipher"]);
Bytes in_place=plain;
stream->run(in_place.data(),in_place.data(),in_place.size(),(*v)["key"].data(),(*v)["nonce"].data(),v->counter);
CHECK(in_place==(*v)["cipher"]);
stream->run(in_place.data(),in_place.data(),in_place.size(),(*v)["key"].data(),(*v)["nonce"].data(),v->counter);
CHECK(in_place==plain);
}
for(const Vector* v:vectors_of("polykey")){
INFO(v->name);
Bytes out(32,0xAA);
stream->run(out.data(),nullptr,out.size(),(*v)["key"].data(),(*v)["nonce"].data(),0);
CHECK(out==(*v)["polykey"]);
}
}
}

TEST_CASE("Poly1305 gives the RFC 8439 vectors")
{
for(const Vector* v:vectors_of("poly1305")){
INFO(v->name);
const Bytes& message=(*v)["message"];
std::uint8_t tag[16];
crypto::detail::poly1305(tag,message.data(),message.size(),(*v)["key"].data());
CHECK(Bytes(tag,tag+16)==(*v)["tag"]);
std::uint8_t donna[16];
crypto::detail::poly1305_donna(donna,message.data(),message.size(),(*v)["key"].data());
CHECK(Bytes(donna,donna+16)==(*v)["tag"]);
}
}

TEST_CASE("Poly1305 after donna-64 gives Monocypher's tags on random and extreme inputs, in one call or block by block")
{
zodiac::Xoshiro256 random;
random.seed(0xD0BB1A64u);
const auto fill=[&](std::uint8_t* bytes,std::size_t size){
for(std::size_t i=0;i<size;++i)bytes[i]=static_cast<std::uint8_t>(random.next()>>56);
};
const auto agree=[](const Bytes& message,const std::uint8_t key[32]){
std::uint8_t expected[16];
crypto::detail::poly1305(expected,message.data(),message.size(),key);
std::uint8_t tag[16];
crypto::detail::poly1305_donna(tag,message.data(),message.size(),key);
CHECK(Bytes(tag,tag+16)==Bytes(expected,expected+16));
// The same message given to the class one block at a time, then its partial end.
crypto::detail::Poly1305 mac(key);
const std::size_t whole=message.size()&~std::size_t{15};
for(std::size_t at=0;at<whole;at+=16)mac.blocks(message.data()+at,16);
if(message.size()!=whole)mac.last_partial(message.data()+whole,message.size()-whole);
std::uint8_t pieces[16];
mac.finish(pieces);
CHECK(Bytes(pieces,pieces+16)==Bytes(expected,expected+16));
};
std::size_t compared=0;
// 64 random keys and messages: buffer b covers the lengths b, b + 64, ... up to 1024, so every length from 0 to 1024 once, and
// each buffer 65536.
for(int buffer=0;buffer<64;++buffer){
std::uint8_t key[32];
fill(key,sizeof key);
Bytes data(65536);
fill(data.data(),data.size());
std::vector<std::size_t> lengths;
for(std::size_t length=static_cast<std::size_t>(buffer);length<=1024;length+=64)lengths.push_back(length);
lengths.push_back(65536);
for(const std::size_t length:lengths){
INFO("buffer "<<buffer<<", length "<<length);
agree(Bytes(data.begin(),data.begin()+static_cast<std::ptrdiff_t>(length)),key);
++compared;
}
}
CHECK(compared==1025+64);
// Extremes that drive the limbs and the final reduction to their edges: every bit r may keep after clamping and an all-ones s,
// over all-ones messages (h stays near its largest values and p), all-zero keys and messages, and r = 1 (the tag is then the
// message's sum plus s, so h can land just at or above p).
std::uint8_t widest[32];
std::fill(widest,widest+32,std::uint8_t{0xFF});
std::uint8_t zero_key[32]{};
std::uint8_t unit[32]{};
unit[0]=1;
std::fill(unit+16,unit+32,std::uint8_t{0xFF});
for(const std::uint8_t* key:{static_cast<const std::uint8_t*>(widest),static_cast<const std::uint8_t*>(zero_key),static_cast<const std::uint8_t*>(unit)}){
for(const std::size_t length:{std::size_t{0},std::size_t{1},std::size_t{15},std::size_t{16},std::size_t{17},std::size_t{31},std::size_t{32},std::size_t{33},std::size_t{64},std::size_t{1023},std::size_t{65536}}){
INFO("extreme key "<<static_cast<int>(key[0])<<", length "<<length);
agree(Bytes(length,0xFF),key);
agree(Bytes(length,0x00),key);
Bytes alternating(length);
for(std::size_t i=0;i<length;++i)alternating[i]=static_cast<std::uint8_t>(i%32<16?0xFF:0x00);
agree(alternating,key);
}
}
}

TEST_CASE("seal and open give the RFC 8439 AEAD vectors on every keystream")
{
for(const Vector* v:vectors_of("aead")){
INFO(v->name);
const crypto::Key key=array_of<crypto::Key>((*v)["key"]);
const crypto::Nonce nonce=array_of<crypto::Nonce>((*v)["nonce"]);
const crypto::Tag expected_tag=array_of<crypto::Tag>((*v)["tag"]);
const Bytes& aad=(*v)["aad"];
const Bytes& plain=(*v)["plain"];
const Bytes& cipher=(*v)["cipher"];
{
Bytes sealed(plain.size(),0xAA);
crypto::Tag tag{};
crypto::seal(key,nonce,as_bytes(aad),as_bytes(plain),as_writable(sealed),tag);
CHECK(sealed==cipher);
CHECK(tag==expected_tag);
Bytes opened(cipher.size(),0xAA);
CHECK(crypto::open(key,nonce,as_bytes(aad),as_bytes(cipher),expected_tag,as_writable(opened)));
CHECK(opened==plain);
Bytes in_place=plain;
crypto::seal(key,nonce,as_bytes(aad),as_bytes(in_place),as_writable(in_place),tag);
CHECK(in_place==cipher);
CHECK(tag==expected_tag);
CHECK(crypto::open(key,nonce,as_bytes(aad),as_bytes(in_place),tag,as_writable(in_place)));
CHECK(in_place==plain);
}
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
Bytes sealed(plain.size(),0xAA);
crypto::Tag tag{};
crypto::detail::seal_with(*stream,key,nonce,as_bytes(aad),as_bytes(plain),as_writable(sealed),tag);
CHECK(sealed==cipher);
CHECK(tag==expected_tag);
Bytes opened(cipher.size(),0xAA);
CHECK(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(cipher),expected_tag,as_writable(opened)));
CHECK(opened==plain);
}
}
}

TEST_CASE("open refuses every flipped bit of cipher, tag and aad and zeroes its output")
{
for(const Vector* v:vectors_of("aead")){
INFO(v->name);
const crypto::Key key=array_of<crypto::Key>((*v)["key"]);
const crypto::Nonce nonce=array_of<crypto::Nonce>((*v)["nonce"]);
const crypto::Tag good_tag=array_of<crypto::Tag>((*v)["tag"]);
const Bytes& good_aad=(*v)["aad"];
const Bytes& good_cipher=(*v)["cipher"];
Bytes plain(good_cipher.size());
std::size_t refused=0;
std::size_t flips=0;
const auto try_open=[&](const Bytes& aad,const Bytes& cipher,const crypto::Tag& tag){
std::fill(plain.begin(),plain.end(),0xAA);
++flips;
if(!crypto::open(key,nonce,as_bytes(aad),as_bytes(cipher),tag,as_writable(plain))&&all_zero(plain))++refused;
};
for(std::size_t bit=0;bit<good_cipher.size()*8;++bit){
Bytes cipher=good_cipher;
cipher[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
try_open(good_aad,cipher,good_tag);
}
for(std::size_t bit=0;bit<good_tag.size()*8;++bit){
crypto::Tag tag=good_tag;
tag[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
try_open(good_aad,good_cipher,tag);
}
for(std::size_t bit=0;bit<good_aad.size()*8;++bit){
Bytes aad=good_aad;
aad[bit/8]^=static_cast<std::uint8_t>(1u<<(bit%8));
try_open(aad,good_cipher,good_tag);
}
CHECK(flips==(good_cipher.size()+good_tag.size()+good_aad.size())*8);
CHECK(refused==flips);
// A wrong nonce, a wrong key, a missing aad byte, a short cipher and a wrong output size are refused too.
crypto::Nonce other_nonce=nonce;
other_nonce[11]^=1;
std::fill(plain.begin(),plain.end(),0xAA);
CHECK_FALSE(crypto::open(key,other_nonce,as_bytes(good_aad),as_bytes(good_cipher),good_tag,as_writable(plain)));
CHECK(all_zero(plain));
crypto::Key other_key=key;
other_key[0]^=0x80;
std::fill(plain.begin(),plain.end(),0xAA);
CHECK_FALSE(crypto::open(other_key,nonce,as_bytes(good_aad),as_bytes(good_cipher),good_tag,as_writable(plain)));
CHECK(all_zero(plain));
std::fill(plain.begin(),plain.end(),0xAA);
CHECK_FALSE(crypto::open(key,nonce,as_bytes(good_aad).first(good_aad.size()-1),as_bytes(good_cipher),good_tag,as_writable(plain)));
CHECK(all_zero(plain));
Bytes short_plain(good_cipher.size()-1,0xAA);
CHECK_FALSE(crypto::open(key,nonce,as_bytes(good_aad),as_bytes(good_cipher).first(good_cipher.size()-1),good_tag,as_writable(short_plain)));
CHECK(all_zero(short_plain));
Bytes wrong_size(good_cipher.size()+1,0xAA);
CHECK_FALSE(crypto::open(key,nonce,as_bytes(good_aad),as_bytes(good_cipher),good_tag,as_writable(wrong_size)));
CHECK(all_zero(wrong_size));
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
Bytes cipher=good_cipher;
cipher.back()^=0x01;
std::fill(plain.begin(),plain.end(),0xAA);
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(good_aad),as_bytes(cipher),good_tag,as_writable(plain)));
CHECK(all_zero(plain));
}
}
}

TEST_CASE("seal and open round-trip empty messages and empty aad")
{
crypto::Key key{};
key[5]=7;
crypto::Nonce nonce{};
nonce[0]=1;
crypto::Tag tag{};
crypto::seal(key,nonce,{},{},{},tag);
CHECK(crypto::open(key,nonce,{},{},tag,{}));
crypto::Tag other=tag;
other[15]^=0x40;
CHECK_FALSE(crypto::open(key,nonce,{},{},other,{}));
const Bytes aad{1,2,3};
CHECK_FALSE(crypto::open(key,nonce,as_bytes(aad),{},tag,{}));
}

// seal's other half of this contract, aborting on a partial overlap (and on a size mismatch), has no test: doctest has no death
// tests and the abort ends the process.
TEST_CASE("seal and open take exact in-place and disjoint buffers, and open refuses a partial overlap without writing")
{
for(const Vector* v:vectors_of("aead")){
INFO(v->name);
const crypto::Key key=array_of<crypto::Key>((*v)["key"]);
const crypto::Nonce nonce=array_of<crypto::Nonce>((*v)["nonce"]);
const crypto::Tag good_tag=array_of<crypto::Tag>((*v)["tag"]);
const Bytes& aad=(*v)["aad"];
const Bytes& plain=(*v)["plain"];
const Bytes& cipher=(*v)["cipher"];
const std::size_t size=cipher.size();
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
// Disjoint, both ways.
Bytes sealed(size,0xAA);
crypto::Tag tag{};
crypto::detail::seal_with(*stream,key,nonce,as_bytes(aad),as_bytes(plain),as_writable(sealed),tag);
CHECK(sealed==cipher);
CHECK(tag==good_tag);
Bytes opened(size,0xAA);
CHECK(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(cipher),good_tag,as_writable(opened)));
CHECK(opened==plain);
// Exactly in place, both ways.
Bytes in_place=plain;
crypto::detail::seal_with(*stream,key,nonce,as_bytes(aad),as_bytes(in_place),as_writable(in_place),tag);
CHECK(in_place==cipher);
CHECK(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(in_place),tag,as_writable(in_place)));
CHECK(in_place==plain);
// Disjoint but adjacent (out right after in, and right before it): no shared byte, so allowed.
Bytes adjacent(2*size);
std::copy(cipher.begin(),cipher.end(),adjacent.begin());
CHECK(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(adjacent).first(size),good_tag,as_writable(adjacent).subspan(size)));
CHECK(std::equal(plain.begin(),plain.end(),adjacent.begin()+static_cast<std::ptrdiff_t>(size)));
// Partial overlaps, the output shifted by k bytes after or before the input: refused, and neither range is written.
for(const std::size_t k:{std::size_t{1},std::size_t{16},std::size_t{63},std::size_t{64},size/2,size-1}){
INFO("shift "<<k);
Bytes after(size+k,0x5C);
std::copy(cipher.begin(),cipher.end(),after.begin());
const Bytes after_before=after;
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(after).first(size),good_tag,as_writable(after).subspan(k,size)));
CHECK(after==after_before);
Bytes before(size+k,0x5C);
std::copy(cipher.begin(),cipher.end(),before.begin()+static_cast<std::ptrdiff_t>(k));
const Bytes before_before=before;
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(before).subspan(k,size),good_tag,as_writable(before).first(size)));
CHECK(before==before_before);
}
}
// The public open refuses a partial overlap too.
Bytes shifted(size+8,0);
std::copy(cipher.begin(),cipher.end(),shifted.begin());
const Bytes unchanged=shifted;
CHECK_FALSE(crypto::open(key,nonce,as_bytes(aad),as_bytes(shifted).first(size),good_tag,as_writable(shifted).subspan(8,size)));
CHECK(shifted==unchanged);
}
}

// A size mismatch that also overlaps (a caller bug twice over): refused, and the cipher it shares bytes with is never zeroed,
// whichever of the two is longer and wherever the output starts (in place included). A disjoint mismatch is still zeroed.
TEST_CASE("open refuses a size mismatch without writing into a cipher the output overlaps")
{
for(const Vector* v:vectors_of("aead")){
INFO(v->name);
const crypto::Key key=array_of<crypto::Key>((*v)["key"]);
const crypto::Nonce nonce=array_of<crypto::Nonce>((*v)["nonce"]);
const crypto::Tag good_tag=array_of<crypto::Tag>((*v)["tag"]);
const Bytes& aad=(*v)["aad"];
const Bytes& cipher=(*v)["cipher"];
const std::size_t size=cipher.size();
REQUIRE(size>=17);
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
for(const std::size_t shift:{std::size_t{0},std::size_t{1},std::size_t{16},size-1}){
for(const std::size_t plain_size:{size-1,size+1,size+64}){
INFO("shift "<<shift<<", output size "<<plain_size);
// The output starts shift bytes into the cipher.
Bytes after(size+shift+plain_size,0x5C);
std::copy(cipher.begin(),cipher.end(),after.begin());
const Bytes after_before=after;
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(after).first(size),good_tag,as_writable(after).subspan(shift,plain_size)));
CHECK(after==after_before);
// The cipher starts shift bytes into the output (shift 0 is in place).
if(shift>=plain_size)continue;
Bytes before(shift+size+plain_size,0x5C);
std::copy(cipher.begin(),cipher.end(),before.begin()+static_cast<std::ptrdiff_t>(shift));
const Bytes before_before=before;
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(before).subspan(shift,size),good_tag,as_writable(before).first(plain_size)));
CHECK(before==before_before);
}
}
// Disjoint and of the wrong size: zeroed.
Bytes disjoint(size+1,0xAA);
CHECK_FALSE(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(cipher),good_tag,as_writable(disjoint)));
CHECK(std::all_of(disjoint.begin(),disjoint.end(),[](std::uint8_t b){return b==0;}));
}
// The public open keeps the cipher too.
Bytes shared(2*size+1,0x5C);
std::copy(cipher.begin(),cipher.end(),shared.begin());
const Bytes unchanged=shared;
CHECK_FALSE(crypto::open(key,nonce,as_bytes(aad),as_bytes(shared).first(size),good_tag,as_writable(shared).subspan(1,size+1)));
CHECK(shared==unchanged);
}
}

#if defined(ZODIAC_CRYPTO_SIMD)
TEST_CASE("the vectorized ChaCha20 gives the reference keystream and seal on random buffers")
{
const std::vector<const crypto::detail::Keystream*> streams=keystreams();
REQUIRE(streams.size()>=2);
constexpr std::uint32_t counters[]{0,1,0xFFFFFFFEu};
zodiac::Xoshiro256 random;
random.seed(0x5EA1ED2026u);
const auto fill=[&](std::uint8_t* bytes,std::size_t size){
for(std::size_t i=0;i<size;++i)bytes[i]=static_cast<std::uint8_t>(random.next()>>56);
};
std::size_t compared=0;
for(int buffer=0;buffer<64;++buffer){
crypto::Key key;
crypto::Nonce nonce;
fill(key.data(),key.size());
fill(nonce.data(),nonce.size());
Bytes data(65536);
fill(data.data(),data.size());
Bytes aad(static_cast<std::size_t>(buffer)*3);
fill(aad.data(),aad.size());
// Buffer b covers the lengths b, b + 64, b + 128, ... up to 1024, so the 64 buffers cover every length from 0 to 1024 once,
// and each of them 65536.
std::vector<std::size_t> lengths;
for(std::size_t length=static_cast<std::size_t>(buffer);length<=1024;length+=64)lengths.push_back(length);
lengths.push_back(65536);
for(const std::size_t length:lengths){
INFO("buffer "<<buffer<<", length "<<length);
for(const std::uint32_t counter:counters){
INFO("counter "<<counter);
Bytes expected(length,0x55);
crypto::detail::chacha20_reference(expected.data(),data.data(),length,key.data(),nonce.data(),counter);
Bytes expected_stream(length,0x55);
crypto::detail::chacha20_reference(expected_stream.data(),nullptr,length,key.data(),nonce.data(),counter);
for(const crypto::detail::Keystream* stream:streams){
INFO(stream->name);
Bytes out(length,0xAA);
stream->run(out.data(),data.data(),length,key.data(),nonce.data(),counter);
CHECK(out==expected);
Bytes keystream(length,0xAA);
stream->run(keystream.data(),nullptr,length,key.data(),nonce.data(),counter);
CHECK(keystream==expected_stream);
Bytes in_place(data.begin(),data.begin()+static_cast<std::ptrdiff_t>(length));
stream->run(in_place.data(),in_place.data(),length,key.data(),nonce.data(),counter);
CHECK(in_place==expected);
++compared;
}
}
const std::span<const std::byte> plain=as_bytes(data).first(length);
Bytes expected(length,0x55);
crypto::Tag expected_tag{};
crypto::detail::seal_with(crypto::detail::reference,key,nonce,as_bytes(aad),plain,as_writable(expected),expected_tag);
for(const crypto::detail::Keystream* stream:streams){
INFO(stream->name);
Bytes sealed(length,0xAA);
crypto::Tag tag{};
crypto::detail::seal_with(*stream,key,nonce,as_bytes(aad),plain,as_writable(sealed),tag);
CHECK(sealed==expected);
CHECK(tag==expected_tag);
Bytes opened(length,0xAA);
CHECK(crypto::detail::open_with(*stream,key,nonce,as_bytes(aad),as_bytes(sealed),tag,as_writable(opened)));
CHECK(std::equal(opened.begin(),opened.end(),data.begin()));
}
}
}
CHECK(compared==(1025+64)*3*(streams.size()));
// The dispatched keystream is the fastest this CPU runs.
CHECK(&crypto::detail::dispatched()==(crypto::detail::cpu_has_avx2()?&crypto::detail::avx2:&crypto::detail::sse2));
}

TEST_CASE("the vectorized ChaCha20 gives the RFC 8439 vectors through seal")
{
for(const Vector* v:vectors_of("aead")){
INFO(v->name);
const crypto::Key key=array_of<crypto::Key>((*v)["key"]);
const crypto::Nonce nonce=array_of<crypto::Nonce>((*v)["nonce"]);
Bytes reference_out((*v)["plain"].size());
crypto::Tag reference_tag{};
crypto::detail::seal_with(crypto::detail::reference,key,nonce,as_bytes((*v)["aad"]),as_bytes((*v)["plain"]),as_writable(reference_out),reference_tag);
for(const crypto::detail::Keystream* stream:keystreams()){
INFO(stream->name);
Bytes out((*v)["plain"].size());
crypto::Tag tag{};
crypto::detail::seal_with(*stream,key,nonce,as_bytes((*v)["aad"]),as_bytes((*v)["plain"]),as_writable(out),tag);
CHECK(out==reference_out);
CHECK(tag==reference_tag);
}
}
}
#endif

TEST_CASE("derive_password_key is Argon2id with one lane")
{
// Single-lane Argon2id vectors of Monocypher 4.0.2's tests/vectors.h (generated there from libsodium): blocks, passes,
// password, salt, hash.
struct Known {
std::uint32_t kib;
std::uint32_t passes;
const char* password;
const char* salt;
const char* hash;
};
constexpr Known known[]{
{508,3,"d19cfb8cb3940aba546f0be57895e2cc","869fe55aab069c5abcf9e7ba6444a846","ff76f317ce99c1b00e342ae5b630ef8815ff43c436dd27602024377ff5cf1712"},
{512,3,"314f716af9c22022fa159dbb4b4d3153","f999b20ab4769eb1d01c057c5295ed04","53e428dced631b164d84cf1f965594e5df8e90eddc9a6f571415a9f2a5a2a464"},
};
for(const Known& k:known){
INFO(k.hash);
const Bytes password=from_hex(k.password);
const auto salt=array_of<std::array<std::uint8_t,16>>(from_hex(k.salt));
const crypto::Key key=crypto::derive_password_key(std::string_view(reinterpret_cast<const char*>(password.data()),password.size()),salt,k.kib,k.passes);
CHECK(Bytes(key.begin(),key.end())==from_hex(k.hash));
}
}

TEST_CASE("derive_password_key is deterministic for a salt and differs for another salt or password")
{
std::array<std::uint8_t,16> salt{};
for(std::size_t i=0;i<salt.size();++i)salt[i]=static_cast<std::uint8_t>(i*17+3);
const crypto::Key first=crypto::derive_password_key("correct horse",salt,256,2);
const crypto::Key again=crypto::derive_password_key("correct horse",salt,256,2);
CHECK(first==again);
std::array<std::uint8_t,16> other_salt=salt;
other_salt[15]^=1;
CHECK(crypto::derive_password_key("correct horse",other_salt,256,2)!=first);
CHECK(crypto::derive_password_key("correct horsf",salt,256,2)!=first);
CHECK(crypto::derive_password_key("correct horse",salt,264,2)!=first);
CHECK(crypto::derive_password_key("correct horse",salt,256,3)!=first);
CHECK(crypto::derive_password_key("",salt,crypto::kMinKdfKib,1)!=crypto::derive_password_key(std::string_view("\0",1),salt,crypto::kMinKdfKib,1));
}

TEST_CASE("derive_password_key refuses settings out of range")
{
const std::array<std::uint8_t,16> salt{};
CHECK_THROWS_AS((void)crypto::derive_password_key("x",salt,crypto::kMinKdfKib-1,1),std::invalid_argument);
CHECK_THROWS_AS((void)crypto::derive_password_key("x",salt,crypto::kMaxKdfKib+1,1),std::invalid_argument);
CHECK_THROWS_AS((void)crypto::derive_password_key("x",salt,crypto::kMinKdfKib,0),std::invalid_argument);
CHECK_THROWS_AS((void)crypto::derive_password_key("x",salt,crypto::kMinKdfKib,crypto::kMaxKdfPasses+1),std::invalid_argument);
CHECK_NOTHROW((void)crypto::derive_password_key("x",salt,crypto::kMinKdfKib,crypto::kMaxKdfPasses));
CHECK(crypto::kDefaultKdfKib==8192);
CHECK(crypto::kDefaultKdfPasses==3);
CHECK(crypto::kMaxKdfKib==65536);
CHECK(crypto::kMaxKdfPasses==8);
}

TEST_CASE("hash is BLAKE2b-256 and derive_file_key its keyed form over the label and the salt")
{
// Expected values from Python's hashlib.blake2b(digest_size=32).
const auto empty_hash=crypto::hash({});
CHECK(Bytes(empty_hash.begin(),empty_hash.end())==from_hex("0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"));
const std::string_view abc="abc";
const auto abc_hash=crypto::hash(std::as_bytes(std::span(abc)));
CHECK(Bytes(abc_hash.begin(),abc_hash.end())==from_hex("bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319"));
// hashlib.blake2b(b"zodiac pack key v1" + bytes(range(100, 116)), key=bytes(range(32)), digest_size=32).
crypto::Key master;
for(std::size_t i=0;i<master.size();++i)master[i]=static_cast<std::uint8_t>(i);
std::array<std::uint8_t,16> salt;
for(std::size_t i=0;i<salt.size();++i)salt[i]=static_cast<std::uint8_t>(100+i);
const crypto::Key file_key=crypto::derive_file_key(master,salt);
CHECK(Bytes(file_key.begin(),file_key.end())==from_hex("51a988cf5de401a222689a04fde7dd95d86de6d9e3efd8a227a072615a9a0159"));
salt[0]^=1;
CHECK(crypto::derive_file_key(master,salt)!=file_key);
// hashlib.blake2b(b"zodiac pack index v1", key=bytes(range(32)), digest_size=32).
const crypto::Key index_key=crypto::derive_index_key(master);
CHECK(Bytes(index_key.begin(),index_key.end())==from_hex("823f6d585dbfdd2f4b6386a809766caa2beec7b2b34184f5937226dcbbaba86a"));
CHECK(index_key!=master);
}

TEST_CASE("random_bytes fills from the entropy source")
{
std::array<std::byte,64> first{};
std::array<std::byte,64> second{};
crypto::random_bytes(first);
crypto::random_bytes(second);
CHECK(first!=second);
CHECK(std::any_of(first.begin(),first.end(),[](std::byte b){return b!=std::byte{0};}));
crypto::random_bytes({});
}

TEST_CASE("wipe zeroes")
{
std::array<std::uint8_t,40> secret;
secret.fill(0xC3);
crypto::wipe(secret.data(),secret.size());
CHECK(std::all_of(secret.begin(),secret.end(),[](std::uint8_t b){return b==0;}));
}
