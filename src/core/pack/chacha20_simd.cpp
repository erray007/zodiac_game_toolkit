// The vectorized ChaCha20 of crypto.hpp's detail namespace (x86-64, ZODIAC_CRYPTO_SIMD): chacha20_sse2 runs 4 blocks at a time in
// SSE2 (every x86-64 CPU has it), chacha20_avx2 8 blocks at a time in AVX2 (compiled for AVX2 by target attribute, run only on
// CPUs that have it, as cpu_has_avx2 tells once). Both give exactly Monocypher's crypto_chacha20_ietf output, including its
// 64-bit counter that runs on into the first nonce word (test_crypto compares them on the RFC 8439 vectors and on random buffers).
// Layout: one vector per state word, lane j holding block j's word ("vertical"), so a quarter round is 12 lane-wise operations
// and the rows never need shuffling; after the rounds a 4 x 4 transpose of 32-bit lanes (within each 128-bit half in AVX2, then
// one cross-half permute) turns the words back into consecutive block bytes, XORed with the input as they are stored.
// A tail shorter than a whole batch is computed into a stack buffer, which is wiped: up to 64 bytes by Monocypher's scalar
// block (cheaper than any batch), up to 256 by one SSE2 batch, else by one AVX2 batch. The copy of the key in the state is
// wiped too.
// Kept by measurement (Task 6 Step 4, bench_files --op crypto_chunk, one 64 KiB seal plus open, paired A B B A against Monocypher's
// scalar keystream in one process, CI run 45651126939): AVX2 0.4405 of the reference's time on an AMD EPYC 7763 (Windows) and 0.4815
// on an AMD EPYC 9V45 (Linux); SSE2 0.5994 and 0.6991. The bar was at least 10 % faster on both.
#include <core/pack/clear.hpp>
#include <core/pack/crypto.hpp>
#include <monocypher.h>
#include <immintrin.h>
#include <cstring>

namespace zodiac::crypto::detail {
namespace {
// The state words that do not change from block to block: constants, key and nonce words 14 and 15. Words 12 and 13 are the
// 64-bit counter, block counter in its low half and nonce word 0 in its high half, as Monocypher's ChaCha20 computes them.
struct State {
std::uint32_t word[16];
std::uint64_t counter;
};
std::uint32_t load32(const std::uint8_t* bytes) noexcept
{
std::uint32_t value;
std::memcpy(&value,bytes,4);
return value;
}
void start(State& state,const std::uint8_t* key,const std::uint8_t* nonce,std::uint32_t counter) noexcept
{
state.word[0]=0x61707865u;
state.word[1]=0x3320646eu;
state.word[2]=0x79622d32u;
state.word[3]=0x6b206574u;
for(int i=0;i<8;++i)state.word[4+i]=load32(key+4*i);
state.word[12]=0;
state.word[13]=0;
state.word[14]=load32(nonce+4);
state.word[15]=load32(nonce+8);
state.counter=counter+(std::uint64_t{load32(nonce)}<<32);
}
void xor_bytes(std::uint8_t* out,const std::uint8_t* in,const std::uint8_t* keystream,std::size_t size) noexcept
{
if(in!=nullptr)for(std::size_t i=0;i<size;++i)out[i]=static_cast<std::uint8_t>(in[i]^keystream[i]);
else std::memcpy(out,keystream,size);
}
// Up to 64 bytes from the state's counter: Monocypher's scalar block, whose 64-bit counter and 8-byte nonce are words 12 to 15.
void scalar_tail(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const State& state) noexcept
{
std::uint8_t nonce[8];
std::memcpy(nonce,&state.word[14],8);
crypto_chacha20_djb(out,in,size,key,nonce,state.counter);
}

// ---- SSE2: 4 blocks ----
#define ZODIAC_ROTL128(x,n) _mm_or_si128(_mm_slli_epi32((x),(n)),_mm_srli_epi32((x),32-(n)))
#define ZODIAC_ROTL128_16(x) _mm_shufflehi_epi16(_mm_shufflelo_epi16((x),0xB1),0xB1)
#define ZODIAC_QR128(a,b,c,d) \
a=_mm_add_epi32(a,b);d=_mm_xor_si128(d,a);d=ZODIAC_ROTL128_16(d); \
c=_mm_add_epi32(c,d);b=_mm_xor_si128(b,c);b=ZODIAC_ROTL128(b,12); \
a=_mm_add_epi32(a,b);d=_mm_xor_si128(d,a);d=ZODIAC_ROTL128(d,8); \
c=_mm_add_epi32(c,d);b=_mm_xor_si128(b,c);b=ZODIAC_ROTL128(b,7)

// Words 12 and 13 of 4 consecutive blocks from counter.
[[gnu::always_inline]] inline void counters128(std::uint64_t counter,__m128i& low,__m128i& high) noexcept
{
const std::uint32_t base=static_cast<std::uint32_t>(counter);
if(base<=0xFFFFFFFFu-3)[[likely]]{
low=_mm_add_epi32(_mm_set1_epi32(static_cast<int>(base)),_mm_set_epi32(3,2,1,0));
high=_mm_set1_epi32(static_cast<int>(counter>>32));
return;
}
alignas(16) std::uint32_t l[4];
alignas(16) std::uint32_t h[4];
for(int j=0;j<4;++j){
const std::uint64_t c=counter+static_cast<std::uint64_t>(j);
l[j]=static_cast<std::uint32_t>(c);
h[j]=static_cast<std::uint32_t>(c>>32);
}
low=_mm_load_si128(reinterpret_cast<const __m128i*>(l));
high=_mm_load_si128(reinterpret_cast<const __m128i*>(h));
}
// Stores the 16 bytes of word group g of the 4 blocks (words 4g to 4g+3 of each), XORed with in when it is not null.
template<bool with_input>
[[gnu::always_inline]] inline void store_group128(std::uint8_t* out,const std::uint8_t* in,__m128i a,__m128i b,__m128i c,__m128i d) noexcept
{
const __m128i t0=_mm_unpacklo_epi32(a,b);
const __m128i t1=_mm_unpacklo_epi32(c,d);
const __m128i t2=_mm_unpackhi_epi32(a,b);
const __m128i t3=_mm_unpackhi_epi32(c,d);
__m128i r[4]{_mm_unpacklo_epi64(t0,t1),_mm_unpackhi_epi64(t0,t1),_mm_unpacklo_epi64(t2,t3),_mm_unpackhi_epi64(t2,t3)};
for(int j=0;j<4;++j){
if constexpr(with_input)r[j]=_mm_xor_si128(r[j],_mm_loadu_si128(reinterpret_cast<const __m128i*>(in+64*j)));
_mm_storeu_si128(reinterpret_cast<__m128i*>(out+64*j),r[j]);
}
}
// 256 bytes: out = in ^ keystream of blocks counter to counter+3 (the keystream alone without input).
template<bool with_input>
[[gnu::always_inline]] inline void batch4(std::uint8_t* out,const std::uint8_t* in,const State& s,std::uint64_t counter) noexcept
{
__m128i c12,c13;
counters128(counter,c12,c13);
__m128i x0=_mm_set1_epi32(static_cast<int>(s.word[0])),x1=_mm_set1_epi32(static_cast<int>(s.word[1]));
__m128i x2=_mm_set1_epi32(static_cast<int>(s.word[2])),x3=_mm_set1_epi32(static_cast<int>(s.word[3]));
__m128i x4=_mm_set1_epi32(static_cast<int>(s.word[4])),x5=_mm_set1_epi32(static_cast<int>(s.word[5]));
__m128i x6=_mm_set1_epi32(static_cast<int>(s.word[6])),x7=_mm_set1_epi32(static_cast<int>(s.word[7]));
__m128i x8=_mm_set1_epi32(static_cast<int>(s.word[8])),x9=_mm_set1_epi32(static_cast<int>(s.word[9]));
__m128i x10=_mm_set1_epi32(static_cast<int>(s.word[10])),x11=_mm_set1_epi32(static_cast<int>(s.word[11]));
__m128i x12=c12,x13=c13;
__m128i x14=_mm_set1_epi32(static_cast<int>(s.word[14])),x15=_mm_set1_epi32(static_cast<int>(s.word[15]));
for(int round=0;round<10;++round){
ZODIAC_QR128(x0,x4,x8,x12);
ZODIAC_QR128(x1,x5,x9,x13);
ZODIAC_QR128(x2,x6,x10,x14);
ZODIAC_QR128(x3,x7,x11,x15);
ZODIAC_QR128(x0,x5,x10,x15);
ZODIAC_QR128(x1,x6,x11,x12);
ZODIAC_QR128(x2,x7,x8,x13);
ZODIAC_QR128(x3,x4,x9,x14);
}
#define ZODIAC_ADD128(i) x##i=_mm_add_epi32(x##i,_mm_set1_epi32(static_cast<int>(s.word[i])))
ZODIAC_ADD128(0);ZODIAC_ADD128(1);ZODIAC_ADD128(2);ZODIAC_ADD128(3);
ZODIAC_ADD128(4);ZODIAC_ADD128(5);ZODIAC_ADD128(6);ZODIAC_ADD128(7);
ZODIAC_ADD128(8);ZODIAC_ADD128(9);ZODIAC_ADD128(10);ZODIAC_ADD128(11);
ZODIAC_ADD128(14);ZODIAC_ADD128(15);
#undef ZODIAC_ADD128
x12=_mm_add_epi32(x12,c12);
x13=_mm_add_epi32(x13,c13);
store_group128<with_input>(out,in,x0,x1,x2,x3);
store_group128<with_input>(out+16,with_input?in+16:nullptr,x4,x5,x6,x7);
store_group128<with_input>(out+32,with_input?in+32:nullptr,x8,x9,x10,x11);
store_group128<with_input>(out+48,with_input?in+48:nullptr,x12,x13,x14,x15);
}
#undef ZODIAC_QR128
#undef ZODIAC_ROTL128_16
#undef ZODIAC_ROTL128
// Fewer than 256 bytes from the state's counter: one scalar block up to 64 bytes, else one SSE2 batch through a buffer.
void tail128(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const State& state) noexcept
{
if(size<=64){
scalar_tail(out,in,size,key,state);
return;
}
alignas(16) std::uint8_t buffer[256];
batch4<false>(buffer,nullptr,state,state.counter);
xor_bytes(out,in,buffer,size);
clear(buffer,sizeof buffer);
}

// ---- AVX2: 8 blocks ----
#define ZODIAC_ROTL256(x,n) _mm256_or_si256(_mm256_slli_epi32((x),(n)),_mm256_srli_epi32((x),32-(n)))
#define ZODIAC_QR256(a,b,c,d) \
a=_mm256_add_epi32(a,b);d=_mm256_xor_si256(d,a);d=_mm256_shuffle_epi8(d,rot16); \
c=_mm256_add_epi32(c,d);b=_mm256_xor_si256(b,c);b=ZODIAC_ROTL256(b,12); \
a=_mm256_add_epi32(a,b);d=_mm256_xor_si256(d,a);d=_mm256_shuffle_epi8(d,rot8); \
c=_mm256_add_epi32(c,d);b=_mm256_xor_si256(b,c);b=ZODIAC_ROTL256(b,7)

// Words 12 and 13 of 8 consecutive blocks from counter.
[[gnu::target("avx2"),gnu::always_inline]] inline void counters256(std::uint64_t counter,__m256i& low,__m256i& high) noexcept
{
const std::uint32_t base=static_cast<std::uint32_t>(counter);
if(base<=0xFFFFFFFFu-7)[[likely]]{
low=_mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(base)),_mm256_set_epi32(7,6,5,4,3,2,1,0));
high=_mm256_set1_epi32(static_cast<int>(counter>>32));
return;
}
alignas(32) std::uint32_t l[8];
alignas(32) std::uint32_t h[8];
for(int j=0;j<8;++j){
const std::uint64_t c=counter+static_cast<std::uint64_t>(j);
l[j]=static_cast<std::uint32_t>(c);
h[j]=static_cast<std::uint32_t>(c>>32);
}
low=_mm256_load_si256(reinterpret_cast<const __m256i*>(l));
high=_mm256_load_si256(reinterpret_cast<const __m256i*>(h));
}
// The 4 x 4 transpose of 32-bit lanes within each 128-bit half: r[j] holds words of block j in its low half and of block j+4
// in its high half.
[[gnu::target("avx2"),gnu::always_inline]] inline void transpose256(__m256i a,__m256i b,__m256i c,__m256i d,__m256i r[4]) noexcept
{
const __m256i t0=_mm256_unpacklo_epi32(a,b);
const __m256i t1=_mm256_unpacklo_epi32(c,d);
const __m256i t2=_mm256_unpackhi_epi32(a,b);
const __m256i t3=_mm256_unpackhi_epi32(c,d);
r[0]=_mm256_unpacklo_epi64(t0,t1);
r[1]=_mm256_unpackhi_epi64(t0,t1);
r[2]=_mm256_unpacklo_epi64(t2,t3);
r[3]=_mm256_unpackhi_epi64(t2,t3);
}
// Stores 32 bytes of each of the 8 blocks at offset (0 or 32): words 8h to 8h+3 come from low, 8h+4 to 8h+7 from high.
template<bool with_input>
[[gnu::target("avx2"),gnu::always_inline]] inline void store_half256(std::uint8_t* out,const std::uint8_t* in,const __m256i low[4],const __m256i high[4]) noexcept
{
for(int j=0;j<4;++j){
__m256i first=_mm256_permute2x128_si256(low[j],high[j],0x20);
__m256i second=_mm256_permute2x128_si256(low[j],high[j],0x31);
if constexpr(with_input){
first=_mm256_xor_si256(first,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(in+64*j)));
second=_mm256_xor_si256(second,_mm256_loadu_si256(reinterpret_cast<const __m256i*>(in+64*(j+4))));
}
_mm256_storeu_si256(reinterpret_cast<__m256i*>(out+64*j),first);
_mm256_storeu_si256(reinterpret_cast<__m256i*>(out+64*(j+4)),second);
}
}
// 512 bytes: out = in ^ keystream of blocks counter to counter+7 (the keystream alone without input).
template<bool with_input>
[[gnu::target("avx2"),gnu::always_inline]] inline void batch8(std::uint8_t* out,const std::uint8_t* in,const State& s,std::uint64_t counter) noexcept
{
const __m256i rot16=_mm256_set_epi8(13,12,15,14,9,8,11,10,5,4,7,6,1,0,3,2,13,12,15,14,9,8,11,10,5,4,7,6,1,0,3,2);
const __m256i rot8=_mm256_set_epi8(14,13,12,15,10,9,8,11,6,5,4,7,2,1,0,3,14,13,12,15,10,9,8,11,6,5,4,7,2,1,0,3);
__m256i c12,c13;
counters256(counter,c12,c13);
__m256i x0=_mm256_set1_epi32(static_cast<int>(s.word[0])),x1=_mm256_set1_epi32(static_cast<int>(s.word[1]));
__m256i x2=_mm256_set1_epi32(static_cast<int>(s.word[2])),x3=_mm256_set1_epi32(static_cast<int>(s.word[3]));
__m256i x4=_mm256_set1_epi32(static_cast<int>(s.word[4])),x5=_mm256_set1_epi32(static_cast<int>(s.word[5]));
__m256i x6=_mm256_set1_epi32(static_cast<int>(s.word[6])),x7=_mm256_set1_epi32(static_cast<int>(s.word[7]));
__m256i x8=_mm256_set1_epi32(static_cast<int>(s.word[8])),x9=_mm256_set1_epi32(static_cast<int>(s.word[9]));
__m256i x10=_mm256_set1_epi32(static_cast<int>(s.word[10])),x11=_mm256_set1_epi32(static_cast<int>(s.word[11]));
__m256i x12=c12,x13=c13;
__m256i x14=_mm256_set1_epi32(static_cast<int>(s.word[14])),x15=_mm256_set1_epi32(static_cast<int>(s.word[15]));
for(int round=0;round<10;++round){
ZODIAC_QR256(x0,x4,x8,x12);
ZODIAC_QR256(x1,x5,x9,x13);
ZODIAC_QR256(x2,x6,x10,x14);
ZODIAC_QR256(x3,x7,x11,x15);
ZODIAC_QR256(x0,x5,x10,x15);
ZODIAC_QR256(x1,x6,x11,x12);
ZODIAC_QR256(x2,x7,x8,x13);
ZODIAC_QR256(x3,x4,x9,x14);
}
#define ZODIAC_ADD256(i) x##i=_mm256_add_epi32(x##i,_mm256_set1_epi32(static_cast<int>(s.word[i])))
ZODIAC_ADD256(0);ZODIAC_ADD256(1);ZODIAC_ADD256(2);ZODIAC_ADD256(3);
ZODIAC_ADD256(4);ZODIAC_ADD256(5);ZODIAC_ADD256(6);ZODIAC_ADD256(7);
ZODIAC_ADD256(8);ZODIAC_ADD256(9);ZODIAC_ADD256(10);ZODIAC_ADD256(11);
ZODIAC_ADD256(14);ZODIAC_ADD256(15);
#undef ZODIAC_ADD256
x12=_mm256_add_epi32(x12,c12);
x13=_mm256_add_epi32(x13,c13);
__m256i low[4],high[4];
transpose256(x0,x1,x2,x3,low);
transpose256(x4,x5,x6,x7,high);
store_half256<with_input>(out,in,low,high);
transpose256(x8,x9,x10,x11,low);
transpose256(x12,x13,x14,x15,high);
store_half256<with_input>(out+32,with_input?in+32:nullptr,low,high);
}
#undef ZODIAC_QR256
#undef ZODIAC_ROTL256
}

void chacha20_sse2(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const std::uint8_t* nonce,std::uint32_t counter) noexcept
{
State state;
start(state,key,nonce,counter);
if(in!=nullptr){
for(;size>=256;size-=256,out+=256,in+=256,state.counter+=4)batch4<true>(out,in,state,state.counter);
}
else{
for(;size>=256;size-=256,out+=256,state.counter+=4)batch4<false>(out,nullptr,state,state.counter);
}
if(size!=0)tail128(out,in,size,key,state);
clear(&state,sizeof state);
}

[[gnu::target("avx2")]] void chacha20_avx2(std::uint8_t* out,const std::uint8_t* in,std::size_t size,const std::uint8_t* key,const std::uint8_t* nonce,std::uint32_t counter) noexcept
{
State state;
start(state,key,nonce,counter);
if(in!=nullptr){
for(;size>=512;size-=512,out+=512,in+=512,state.counter+=8)batch8<true>(out,in,state,state.counter);
}
else{
for(;size>=512;size-=512,out+=512,state.counter+=8)batch8<false>(out,nullptr,state,state.counter);
}
if(size>256){
alignas(32) std::uint8_t buffer[512];
batch8<false>(buffer,nullptr,state,state.counter);
xor_bytes(out,in,buffer,size);
clear(buffer,sizeof buffer);
}
else if(size!=0)tail128(out,in,size,key,state);
clear(&state,sizeof state);
}

bool cpu_has_avx2() noexcept
{
__builtin_cpu_init();
return __builtin_cpu_supports("avx2");
}
}
