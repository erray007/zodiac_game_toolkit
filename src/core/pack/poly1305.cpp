// Poly1305 after poly1305-donna-64 (contract in poly1305.hpp): donna's 44/44/42-bit limbs and arithmetic, its loads and stores as
// little-endian 64-bit words.
#include <core/pack/clear.hpp>
#include <core/pack/crypto.hpp>
#include <core/pack/poly1305.hpp>
#include <bit>
#include <cstring>

namespace zodiac::crypto::detail {
namespace {
using u64=std::uint64_t;
using u128=unsigned __int128;
constexpr u64 kMask44=0xfffffffffff;
constexpr u64 kMask42=0x3ffffffffff;

u64 load64(const std::uint8_t* bytes) noexcept
{
u64 value;
std::memcpy(&value,bytes,8);
if constexpr(std::endian::native==std::endian::big)value=std::byteswap(value);
return value;
}
void store64(std::uint8_t* bytes,u64 value) noexcept
{
if constexpr(std::endian::native==std::endian::big)value=std::byteswap(value);
std::memcpy(bytes,&value,8);
}
}

Poly1305::Poly1305(const std::uint8_t key[32]) noexcept
{
// r &= 0xffffffc0ffffffc0ffffffc0fffffff
const u64 t0=load64(key);
const u64 t1=load64(key+8);
r_[0]=t0&0xffc0fffffff;
r_[1]=((t0>>44)|(t1<<20))&0xfffffc0ffff;
r_[2]=(t1>>24)&0x00ffffffc0f;
h_[0]=0;
h_[1]=0;
h_[2]=0;
pad_[0]=load64(key+16);
pad_[1]=load64(key+24);
}

Poly1305::~Poly1305()
{
// finish has wiped the state already; this covers an object that never finished.
clear(this,sizeof *this);
}

void Poly1305::run(const std::uint8_t* m,std::size_t bytes,u64 high_bit) noexcept
{
const u64 r0=r_[0];
const u64 r1=r_[1];
const u64 r2=r_[2];
const u64 s1=r1*(5<<2);
const u64 s2=r2*(5<<2);
u64 h0=h_[0];
u64 h1=h_[1];
u64 h2=h_[2];
while(bytes>=16){
// h += m[i]
const u64 t0=load64(m);
const u64 t1=load64(m+8);
h0+=t0&kMask44;
h1+=((t0>>44)|(t1<<20))&kMask44;
h2+=((t1>>24)&kMask42)|high_bit;
// h *= r
u128 d0=u128{h0}*r0+u128{h1}*s2+u128{h2}*s1;
u128 d1=u128{h0}*r1+u128{h1}*r0+u128{h2}*s2;
u128 d2=u128{h0}*r2+u128{h1}*r1+u128{h2}*r0;
// (partial) h %= p
u64 c=static_cast<u64>(d0>>44);
h0=static_cast<u64>(d0)&kMask44;
d1+=c;
c=static_cast<u64>(d1>>44);
h1=static_cast<u64>(d1)&kMask44;
d2+=c;
c=static_cast<u64>(d2>>42);
h2=static_cast<u64>(d2)&kMask42;
h0+=c*5;
c=h0>>44;
h0&=kMask44;
h1+=c;
m+=16;
bytes-=16;
}
h_[0]=h0;
h_[1]=h1;
h_[2]=h2;
}

void Poly1305::last_partial(const std::uint8_t* message,std::size_t bytes) noexcept
{
std::uint8_t block[16]{};
std::memcpy(block,message,bytes);
block[bytes]=1;
run(block,16,0);
clear(block,sizeof block);
}

void Poly1305::finish(std::uint8_t tag[16]) noexcept
{
u64 h0=h_[0];
u64 h1=h_[1];
u64 h2=h_[2];
// fully carry h
u64 c=h1>>44;
h1&=kMask44;
h2+=c;
c=h2>>42;
h2&=kMask42;
h0+=c*5;
c=h0>>44;
h0&=kMask44;
h1+=c;
c=h1>>44;
h1&=kMask44;
h2+=c;
c=h2>>42;
h2&=kMask42;
h0+=c*5;
c=h0>>44;
h0&=kMask44;
h1+=c;
// compute h + -p
u64 g0=h0+5;
c=g0>>44;
g0&=kMask44;
u64 g1=h1+c;
c=g1>>44;
g1&=kMask44;
u64 g2=h2+c-(u64{1}<<42);
// select h if h < p, or h + -p if h >= p (constant time)
c=(g2>>63)-1;
g0&=c;
g1&=c;
g2&=c;
c=~c;
h0=(h0&c)|g0;
h1=(h1&c)|g1;
h2=(h2&c)|g2;
// h = (h + pad)
const u64 t0=pad_[0];
const u64 t1=pad_[1];
h0+=t0&kMask44;
c=h0>>44;
h0&=kMask44;
h1+=(((t0>>44)|(t1<<20))&kMask44)+c;
c=h1>>44;
h1&=kMask44;
h2+=((t1>>24)&kMask42)+c;
h2&=kMask42;
// mac = h % (2^128)
store64(tag,h0|(h1<<44));
store64(tag+8,(h1>>20)|(h2<<24));
clear(this,sizeof *this);
}

void poly1305(std::uint8_t tag[16],const std::uint8_t* message,std::size_t size,const std::uint8_t key[32]) noexcept
{
Poly1305 mac(key);
const std::size_t whole=size&~std::size_t{15};
mac.blocks(message,whole);
if(size!=whole)mac.last_partial(message+whole,size-whole);
mac.finish(tag);
}
}
