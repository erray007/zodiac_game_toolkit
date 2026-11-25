// The pack cryptography of crypto.hpp on Monocypher 4.0.2: AEAD_CHACHA20_POLY1305 assembled as RFC 8439 2.8 specifies over a
// chosen keystream (its lead batch gives the Poly1305 key and the first data bytes), Argon2id, BLAKE2b and the entropy source.
// Small secrets are wiped with crypto_wipe; the lead batch, a refused output and the Argon2 work area, which are large or on the
// hot path, with memset behind a compiler barrier, which the optimizer cannot remove either and which runs at memset's speed.
#include <core/pack/crypto.hpp>
#include <platform/entropy.hpp>
#include <monocypher.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace zodiac::crypto {
namespace {
// memset that is never removed as a dead store: the empty asm may read the bytes through its pointer operand.
void clear(void* secret,std::size_t size) noexcept
{
std::memset(secret,0,size);
asm volatile(""::"r"(secret):"memory");
}
void store64(std::uint8_t* out,std::uint64_t value) noexcept
{
for(int i=0;i<8;++i)out[i]=static_cast<std::uint8_t>(value>>(8*i));
}
const std::uint8_t* bytes_of(std::span<const std::byte> span) noexcept
{
return reinterpret_cast<const std::uint8_t*>(span.data());
}
// The RFC 8439 2.8 tag: Poly1305 over aad, zeros to a multiple of 16, cipher, zeros to a multiple of 16, then both lengths.
void aead_tag(std::uint8_t tag[16],const std::uint8_t poly_key[32],std::span<const std::byte> aad,const std::uint8_t* cipher,std::size_t size) noexcept
{
static constexpr std::uint8_t zeros[16]{};
crypto_poly1305_ctx context;
crypto_poly1305_init(&context,poly_key);
if(!aad.empty()){
crypto_poly1305_update(&context,bytes_of(aad),aad.size());
crypto_poly1305_update(&context,zeros,(0-aad.size())&15);
}
if(size!=0){
crypto_poly1305_update(&context,cipher,size);
crypto_poly1305_update(&context,zeros,(0-size)&15);
}
std::uint8_t lengths[16];
store64(lengths,aad.size());
store64(lengths+8,size);
crypto_poly1305_update(&context,lengths,16);
// crypto_poly1305_final wipes the context.
crypto_poly1305_final(&context,tag);
}
// out = in ^ keystream for the data bytes the lead batch covers (block 1 onwards of it).
void xor_lead(std::uint8_t* out,const std::uint8_t* in,const std::uint8_t* lead,std::size_t size) noexcept
{
for(std::size_t i=0;i<size;++i)out[i]=static_cast<std::uint8_t>(in[i]^lead[i]);
}
}

void wipe(void* secret,std::size_t size) noexcept
{
crypto_wipe(secret,size);
}

namespace detail {
void chacha20_reference(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const std::uint8_t* nonce,std::uint32_t counter) noexcept
{
crypto_chacha20_ietf(out,in,size,key,nonce,counter);
}

void poly1305(std::uint8_t tag[16],const std::uint8_t* message,std::size_t size,const std::uint8_t key[32]) noexcept
{
crypto_poly1305(tag,message,size,key);
}

const Keystream& dispatched() noexcept
{
#if defined(ZODIAC_CRYPTO_SIMD)
static const Keystream& chosen=cpu_has_avx2()?avx2:sse2;
return chosen;
#else
return reference;
#endif
}

void seal_with(const Keystream& stream,const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> plain,std::span<std::byte> cipher,Tag& tag) noexcept
{
const std::size_t size=plain.size();
if(cipher.size()!=size||size>kMaxSealBytes)[[unlikely]]std::abort();
const std::uint8_t* in=bytes_of(plain);
std::uint8_t* out=reinterpret_cast<std::uint8_t*>(cipher.data());
alignas(64) std::uint8_t lead[kMaxLead];
stream.run(lead,nullptr,stream.lead,key.data(),nonce.data(),0);
const std::size_t first=std::min(size,stream.lead-64);
xor_lead(out,in,lead+64,first);
if(size>first)stream.run(out+first,in+first,size-first,key.data(),nonce.data(),static_cast<std::uint32_t>(stream.lead/64));
aead_tag(tag.data(),lead,aad,out,size);
clear(lead,stream.lead);
}

bool open_with(const Keystream& stream,const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> cipher,const Tag& tag,std::span<std::byte> plain) noexcept
{
const std::size_t size=cipher.size();
std::uint8_t* out=reinterpret_cast<std::uint8_t*>(plain.data());
if(plain.size()!=size||size>kMaxSealBytes)[[unlikely]]{
if(!plain.empty())clear(out,plain.size());
return false;
}
const std::uint8_t* in=bytes_of(cipher);
alignas(64) std::uint8_t lead[kMaxLead];
stream.run(lead,nullptr,stream.lead,key.data(),nonce.data(),0);
std::uint8_t expected[16];
aead_tag(expected,lead,aad,in,size);
// Constant time: crypto_verify16 reads every byte whatever the first difference.
const bool authentic=crypto_verify16(expected,tag.data())==0;
crypto_wipe(expected,sizeof expected);
if(authentic){
const std::size_t first=std::min(size,stream.lead-64);
xor_lead(out,in,lead+64,first);
if(size>first)stream.run(out+first,in+first,size-first,key.data(),nonce.data(),static_cast<std::uint32_t>(stream.lead/64));
}
else if(size!=0)clear(out,size);
clear(lead,stream.lead);
return authentic;
}
}

void seal(const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> plain,std::span<std::byte> cipher,Tag& tag) noexcept
{
detail::seal_with(detail::dispatched(),key,nonce,aad,plain,cipher,tag);
}

bool open(const Key& key,const Nonce& nonce,std::span<const std::byte> aad,std::span<const std::byte> cipher,const Tag& tag,std::span<std::byte> plain) noexcept
{
return detail::open_with(detail::dispatched(),key,nonce,aad,cipher,tag,plain);
}

Key derive_password_key(std::string_view password,std::span<const std::uint8_t,16> salt,std::uint32_t kib,std::uint32_t passes)
{
if(kib<kMinKdfKib||kib>kMaxKdfKib||passes<1||passes>kMaxKdfPasses)throw std::invalid_argument("the key derivation settings are out of range");
if(password.size()>0xFFFFFFFFu)throw std::invalid_argument("the password is too long");
// Argon2's blocks are 1 KiB of 64-bit words; Monocypher needs the work area aligned for them. Left uninitialized: Argon2
// writes every block before it reads it.
const std::size_t work_bytes=std::size_t{kib}*1024;
const std::unique_ptr<std::uint64_t[]> work=std::make_unique_for_overwrite<std::uint64_t[]>(work_bytes/8);
const crypto_argon2_config config{CRYPTO_ARGON2_ID,kib,passes,1};
const crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(password.data()),salt.data(),static_cast<std::uint32_t>(password.size()),static_cast<std::uint32_t>(salt.size())};
Key key;
crypto_argon2(key.data(),static_cast<std::uint32_t>(key.size()),work.get(),config,inputs,crypto_argon2_no_extras);
clear(work.get(),work_bytes);
return key;
}

Key derive_file_key(const Key& master,std::span<const std::uint8_t,16> salt) noexcept
{
static constexpr char label[]="zodiac pack key v1";
crypto_blake2b_ctx context;
crypto_blake2b_keyed_init(&context,32,master.data(),master.size());
crypto_blake2b_update(&context,reinterpret_cast<const std::uint8_t*>(label),sizeof label-1);
crypto_blake2b_update(&context,salt.data(),salt.size());
Key key;
// crypto_blake2b_final wipes the context.
crypto_blake2b_final(&context,key.data());
return key;
}

std::array<std::uint8_t,32> hash(std::span<const std::byte> data) noexcept
{
std::array<std::uint8_t,32> digest;
crypto_blake2b(digest.data(),digest.size(),bytes_of(data),data.size());
return digest;
}

void random_bytes(std::span<std::byte> out)
{
if(out.empty())return;
if(!platform::fill_entropy(out.data(),out.size()))throw std::runtime_error("the entropy source failed");
}
}
