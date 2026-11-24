// zodiac::String (string.hpp): UTF-8 scanning with word-at-a-time ASCII fast paths, the heap block (recycled through a
// per-thread cache) and its sparse character index, the stable and the keyed table hash, inline short copies, an SSE2
// substring search, searching on character boundaries, case mapping, NFC runs, and number formatting and parsing.
#include <core/types/heap.hpp>
#include <core/types/string.hpp>
#include <core/types/unicode.hpp>
#include <platform/entropy.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale.h>
#include <new>
#include <stdlib.h>
#include <string>
#include <system_error>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace zodiac {
namespace {
using byte=unsigned char;
using string_detail::meta_ascii;
using string_detail::meta_length;
using string_detail::meta_unknown;
constexpr std::uint64_t high_bits=0x8080808080808080u;
constexpr std::uint64_t low_bits=0x0101010101010101u;
constexpr std::uint32_t max_size=static_cast<std::uint32_t>(String::max_byte_length);
constexpr std::uint32_t step=static_cast<std::uint32_t>(String::index_step);
constexpr std::size_t npos=std::string_view::npos;

[[noreturn]] void too_long()
{
throw std::bad_alloc();
}
inline const byte* ubytes(const char* p) noexcept
{
return reinterpret_cast<const byte*>(p);
}
inline std::uint64_t load_word(const byte* p) noexcept
{
std::uint64_t word;
std::memcpy(&word,p,8);
return word;
}
inline void store_word(char* p,std::uint64_t word) noexcept
{
std::memcpy(p,&word,8);
}
// memcpy for ranges that do not overlap. Up to 64 bytes are copied inline with overlapping loads that stay inside the
// source, all done before the stores (the C runtime's memcpy costs a call through the import table or the PLT, more than
// the copy itself for the short pieces scripts copy and concatenate); longer ranges go to memcpy.
inline void copy_bytes(char* d,const char* s,std::size_t n) noexcept
{
if(n>64){
std::memcpy(d,s,n);
return;
}
const byte* from=ubytes(s);
if(n>=16){
#if defined(__SSE2__)
const __m128i a=_mm_loadu_si128(reinterpret_cast<const __m128i*>(from));
const __m128i b=_mm_loadu_si128(reinterpret_cast<const __m128i*>(from+n-16));
if(n>32){
const __m128i c=_mm_loadu_si128(reinterpret_cast<const __m128i*>(from+16));
const __m128i e=_mm_loadu_si128(reinterpret_cast<const __m128i*>(from+n-32));
_mm_storeu_si128(reinterpret_cast<__m128i*>(d),a);
_mm_storeu_si128(reinterpret_cast<__m128i*>(d+16),c);
_mm_storeu_si128(reinterpret_cast<__m128i*>(d+n-32),e);
_mm_storeu_si128(reinterpret_cast<__m128i*>(d+n-16),b);
return;
}
_mm_storeu_si128(reinterpret_cast<__m128i*>(d),a);
_mm_storeu_si128(reinterpret_cast<__m128i*>(d+n-16),b);
return;
#else
std::memcpy(d,s,n);
return;
#endif
}
if(n>=8){
const std::uint64_t a=load_word(from),b=load_word(from+n-8);
store_word(d,a);
store_word(d+n-8,b);
return;
}
if(n>=4){
std::uint32_t a,b;
std::memcpy(&a,s,4);
std::memcpy(&b,s+n-4,4);
std::memcpy(d,&a,4);
std::memcpy(d+n-4,&b,4);
return;
}
if(n>=2){
std::uint16_t a,b;
std::memcpy(&a,s,2);
std::memcpy(&b,s+n-2,2);
std::memcpy(d,&a,2);
std::memcpy(d+n-2,&b,2);
return;
}
if(n==1)*d=*s;
}
// Bytes in front of the first byte with its high bit set, for a word loaded from memory whose high bits are not all clear.
inline unsigned ascii_prefix(std::uint64_t word) noexcept
{
const std::uint64_t high=word&high_bits;
if constexpr(std::endian::native==std::endian::little){
return static_cast<unsigned>(std::countr_zero(high))>>3;
}else{
return static_cast<unsigned>(std::countl_zero(high))>>3;
}
}
inline bool has_byte(std::uint64_t word,byte value) noexcept
{
const std::uint64_t x=word^(low_bits*value);
return ((x-low_bits)&~x&high_bits)!=0;
}
// Case mapping of eight ASCII bytes at once (every byte below 0x80, so the additions never carry between bytes).
inline std::uint64_t ascii_lower(std::uint64_t word) noexcept
{
const std::uint64_t at_least_a=word+low_bits*(0x80-'A');
const std::uint64_t above_z=word+low_bits*(0x80-'Z'-1);
return word|(((at_least_a^above_z)&high_bits)>>2);
}
inline std::uint64_t ascii_upper(std::uint64_t word) noexcept
{
const std::uint64_t at_least_a=word+low_bits*(0x80-'a');
const std::uint64_t above_z=word+low_bits*(0x80-'z'-1);
return word^(((at_least_a^above_z)&high_bits)>>2);
}
inline bool is_continuation(byte b) noexcept
{
return (b&0xC0)==0x80;
}
// Bytes of the character at p: the length of the well-formed sequence starting there (Unicode Table 3-7), or 1.
inline std::uint32_t char_size(const byte* p,const byte* end) noexcept
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
// The code point of the character of size bytes at p (size from char_size); U+FFFD for an invalid byte.
inline char32_t decode(const byte* p,std::uint32_t size) noexcept
{
switch(size){
case 1:
return p[0]<0x80?static_cast<char32_t>(p[0]):char32_t{0xFFFD};
case 2:
return static_cast<char32_t>(((p[0]&0x1Fu)<<6)|(p[1]&0x3Fu));
case 3:
return static_cast<char32_t>(((p[0]&0x0Fu)<<12)|((p[1]&0x3Fu)<<6)|(p[2]&0x3Fu));
default:
return static_cast<char32_t>(((p[0]&0x07u)<<18)|((p[1]&0x3Fu)<<12)|((p[2]&0x3Fu)<<6)|(p[3]&0x3Fu));
}
}
// Writes the UTF-8 encoding of c (U+FFFD for surrogates and values above U+10FFFF) and returns its size.
inline std::uint32_t encode(char32_t c,char* out) noexcept
{
if(c<0x80){
out[0]=static_cast<char>(c);
return 1;
}
if(c<0x800){
out[0]=static_cast<char>(0xC0|(c>>6));
out[1]=static_cast<char>(0x80|(c&0x3F));
return 2;
}
if(c>0x10FFFF||(c>=0xD800&&c<0xE000))c=0xFFFD;
if(c<0x10000){
out[0]=static_cast<char>(0xE0|(c>>12));
out[1]=static_cast<char>(0x80|((c>>6)&0x3F));
out[2]=static_cast<char>(0x80|(c&0x3F));
return 3;
}
out[0]=static_cast<char>(0xF0|(c>>18));
out[1]=static_cast<char>(0x80|((c>>12)&0x3F));
out[2]=static_cast<char>(0x80|((c>>6)&0x3F));
out[3]=static_cast<char>(0x80|(c&0x3F));
return 4;
}
// Characters in [p, end), and whether every byte is ASCII.
std::uint32_t count_chars(const byte* p,const byte* end,bool& ascii) noexcept
{
std::uint32_t count=0;
bool all_ascii=true;
while(p<end){
if(end-p>=8){
const std::uint64_t word=load_word(p);
if((word&high_bits)==0){
p+=8;
count+=8;
continue;
}
const unsigned prefix=ascii_prefix(word);
p+=prefix;
count+=prefix;
}else if(*p<0x80){
++p;
++count;
continue;
}
all_ascii=false;
p+=char_size(p,end);
++count;
}
ascii=all_ascii;
return count;
}
// The position n characters after p (or end, if fewer remain).
const byte* skip_chars(const byte* p,const byte* end,std::uint32_t n) noexcept
{
while(n!=0&&p<end){
if(n>=8&&end-p>=8){
const std::uint64_t word=load_word(p);
if((word&high_bits)==0){
p+=8;
n-=8;
continue;
}
const unsigned prefix=ascii_prefix(word);
if(prefix!=0){
p+=prefix;
n-=prefix;
continue;
}
}
p+=*p<0x80?1:char_size(p,end);
--n;
}
return p;
}
// True when p (in [begin, end]) is not inside a well-formed sequence. Only a sequence starting at the nearest preceding
// non-continuation byte can cover p, so at most three bytes are examined.
inline bool is_boundary(const byte* begin,const byte* end,const byte* p) noexcept
{
if(p==begin||p==end||!is_continuation(*p))return true;
for(std::ptrdiff_t k=1;k<=3&&p-k>=begin;++k){
const byte* start=p-k;
if(!is_continuation(*start))return !(*start>=0xC2&&static_cast<std::ptrdiff_t>(char_size(start,end))>k);
}
return true;
}
// The start of the character ending at the boundary e (begin < e, begin a boundary).
inline const byte* previous_char(const byte* begin,const byte* e) noexcept
{
if(!is_continuation(e[-1]))return e-1;
for(std::ptrdiff_t k=2;k<=4&&e-k>=begin;++k){
const byte* start=e-k;
if(!is_continuation(*start))return static_cast<std::ptrdiff_t>(char_size(start,e))==k?start:e-1;
}
return e-1;
}
// Substring search. libc++'s find is a memchr for the first byte and a memcmp per hit, two calls that dominate for the
// short needles and texts scripts search (glibc's are fast, the Windows C runtime's are not). With SSE2 (every x86-64
// target) search tests 16 candidate starts per step against the needle's first and last bytes, compares a candidate
// with overlapping word loads, and ends on an overlapping block masked to the candidates not yet tested instead of a
// byte loop; texts too short for one block are scanned byte by byte. Every load stays inside the text and the needle.
// Other targets use std::string_view::find. Its arguments are four scalars so both x86-64 ABIs pass them in registers.
inline std::uint32_t load_u32(const byte* p) noexcept
{
std::uint32_t value;
std::memcpy(&value,p,4);
return value;
}
inline std::uint16_t load_u16(const byte* p) noexcept
{
std::uint16_t value;
std::memcpy(&value,p,2);
return value;
}
// True when the n bytes at a and b are equal (n >= 2). Long needles are rare: string_detail::same_long_bytes keeps their
// loop out of search, which leaves its scan with few live registers.
inline bool same_bytes(const byte* a,const byte* b,std::size_t n) noexcept
{
if(n>16)return string_detail::same_long_bytes(reinterpret_cast<const char*>(a),reinterpret_cast<const char*>(b),n);
if(n>=8)return ((load_word(a)^load_word(b))|(load_word(a+n-8)^load_word(b+n-8)))==0;
if(n>=4)return ((load_u32(a)^load_u32(b))|(load_u32(a+n-4)^load_u32(b+n-4)))==0;
return ((load_u16(a)^load_u16(b))|(load_u16(a+n-2)^load_u16(b+n-2)))==0;
}
#if defined(__SSE2__)&&!defined(__GLIBC__)
// Bit k set when the candidate start at p+k has the needle's first byte there and its last byte at last+k.
inline unsigned candidates(const byte* p,const byte* last,__m128i first_byte,__m128i last_byte) noexcept
{
const __m128i a=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)),first_byte);
const __m128i b=_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(last)),last_byte);
return static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(a,b)));
}
// Offset of the first byte c in t[0, size), or npos. Out of line, so search's one-byte case is a tail call that does not
// pay for the registers of its other loops.
[[gnu::noinline]] std::size_t find_byte(const byte* t,std::size_t size,byte c) noexcept
{
const __m128i v=_mm_set1_epi8(static_cast<char>(c));
std::size_t i=0;
for(;i+16<=size;i+=16){
const auto mask=static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t+i)),v)));
if(mask)return i+static_cast<std::size_t>(std::countr_zero(mask));
}
if(i==size)return npos;
if(size>=16){
// The last block overlaps the one before; the shift drops the i-base bytes already tested.
const std::size_t base=size-16;
const auto mask=static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t+base)),v)))>>(i-base);
return mask?i+static_cast<std::size_t>(std::countr_zero(mask)):npos;
}
for(;i<size;++i)if(t[i]==c)return i;
return npos;
}
#endif
// Offset of the first occurrence of s[0, n) in t[0, size), or npos; an empty needle matches at 0.
std::size_t search(const byte* t,std::size_t size,const byte* s,std::size_t n) noexcept
{
if(n>size)return npos;
if(n==0)return 0;
#if defined(__GLIBC__)
// glibc's memchr (AVX2 or EVEX, chosen at load time) scans for the first byte faster than SSE2 can, so it finds the
// candidates and each is verified inline: libc++'s find makes the same memchr call plus a memcmp call per candidate
// (the SSE2 scan below measured 1.09 to 1.11 against it on CI).
const byte* const end=t+(size-n+1);
for(const byte* p=t;p<end;++p){
p=static_cast<const byte*>(std::memchr(p,s[0],static_cast<std::size_t>(end-p)));
if(!p)return npos;
if(n==1||(p[n-1]==s[n-1]&&same_bytes(p,s,n)))return static_cast<std::size_t>(p-t);
}
return npos;
#elif defined(__SSE2__)
if(n==1)return find_byte(t,size,s[0]);
// Candidate starts are [0, stop); candidate k's last byte is last[k].
const std::size_t stop=size-n+1;
const byte* last=t+n-1;
const __m128i first_byte=_mm_set1_epi8(static_cast<char>(s[0]));
const __m128i last_byte=_mm_set1_epi8(static_cast<char>(s[n-1]));
std::size_t i=0;
for(;i+16<=stop;i+=16){
for(unsigned mask=candidates(t+i,last+i,first_byte,last_byte);mask;mask&=mask-1){
const std::size_t at=i+static_cast<std::size_t>(std::countr_zero(mask));
if(same_bytes(t+at,s,n))return at;
}
}
if(i==stop)return npos;
if(stop>=16){
// The last block overlaps the one before (its loads end at the text's last byte); the shift drops the candidates
// already tested.
const std::size_t base=stop-16;
for(unsigned mask=candidates(t+base,last+base,first_byte,last_byte)>>(i-base);mask;mask&=mask-1){
const std::size_t at=i+static_cast<std::size_t>(std::countr_zero(mask));
if(same_bytes(t+at,s,n))return at;
}
return npos;
}
for(;i<stop;++i)if(t[i]==s[0]&&last[i]==s[n-1]&&same_bytes(t+i,s,n))return i;
return npos;
#else
return std::string_view(reinterpret_cast<const char*>(t),size).find(std::string_view(reinterpret_cast<const char*>(s),n));
#endif
}
// Byte offset of the first occurrence of needle at or after from, or npos (std::string_view::find's result).
inline std::size_t search(std::string_view text,std::string_view needle,std::size_t from) noexcept
{
if(from>text.size())return npos;
const std::size_t pos=search(ubytes(text.data())+from,text.size()-from,ubytes(needle.data()),needle.size());
return pos==npos?npos:pos+from;
}
// Byte offset of the first match at or after from that covers whole characters.
std::size_t find_aligned(std::string_view text,std::string_view needle,std::size_t from) noexcept
{
const byte* begin=ubytes(text.data());
const byte* end=begin+text.size();
for(;;){
const std::size_t pos=search(text,needle,from);
if(pos==npos||(is_boundary(begin,end,begin+pos)&&is_boundary(begin,end,begin+pos+needle.size())))return pos;
from=pos+1;
}
}
// Byte offset of the last match starting at or before from that covers whole characters.
std::size_t rfind_aligned(std::string_view text,std::string_view needle,std::size_t from) noexcept
{
const byte* begin=ubytes(text.data());
const byte* end=begin+text.size();
for(;;){
const std::size_t pos=text.rfind(needle,from);
if(pos==npos||(is_boundary(begin,end,begin+pos)&&is_boundary(begin,end,begin+pos+needle.size())))return pos;
if(pos==0)return npos;
from=pos-1;
}
}
inline std::uint32_t joined_meta(std::uint32_t a,std::uint32_t b,std::string_view b_bytes) noexcept
{
// Appending cannot change earlier characters unless the new bytes start with a continuation byte that completes a
// sequence the old end left open; otherwise counts add up.
if(a==meta_unknown||b==meta_unknown)return meta_unknown;
if(!b_bytes.empty()&&is_continuation(static_cast<byte>(b_bytes[0])))return meta_unknown;
return ((a&meta_length)+(b&meta_length))|(a&b&meta_ascii);
}
// The longest text format_number writes: fixed notation is used only for 1e-7 <= |v| < 1e21, so it is at most a sign,
// "0.", six zeros and 17 significant digits (26 bytes) or a sign and 21 integer digits; shortest scientific is at most
// 24 bytes ("-2.2250738585072014e-308"). float needs fewer.
constexpr std::size_t number_buffer=32;
static_assert(number_buffer>=26);
// Shortest round-trip text of value (std::to_chars), JavaScript's choice of notation: fixed for 1e-7 <= |value| < 1e21,
// otherwise scientific without leading zeros in the exponent; "nan", "inf", "-inf", and "0" for both zeros.
template<typename T>
std::uint32_t format_number(T value,char* out) noexcept
{
if(std::isnan(value)){
std::memcpy(out,"nan",3);
return 3;
}
if(std::isinf(value)){
if(value<0){
std::memcpy(out,"-inf",4);
return 4;
}
std::memcpy(out,"inf",3);
return 3;
}
if(value==0){
out[0]='0';
return 1;
}
const T magnitude=value<0?-value:value;
if(magnitude>=static_cast<T>(1e-7)&&magnitude<static_cast<T>(1e21)){
return static_cast<std::uint32_t>(std::to_chars(out,out+number_buffer,value,std::chars_format::fixed).ptr-out);
}
char* end=std::to_chars(out,out+number_buffer,value,std::chars_format::scientific).ptr;
char* digits=std::find(out,end,'e')+2;
char* first=digits;
while(end-first>1&&*first=='0')++first;
std::memmove(digits,first,static_cast<std::size_t>(end-first));
end-=first-digits;
return static_cast<std::uint32_t>(end-out);
}

// The hashes: wyhash's structure (multiply-fold mixing of 8-byte little-endian reads). It needs the 64x64->128 multiply of
// unsigned __int128, which clang has on every 64-bit target Zodiac builds for.
#if !defined(__SIZEOF_INT128__)
#error "zodiac::String's hash needs unsigned __int128 (a 64-bit clang target)"
#endif
static_assert(sizeof(unsigned __int128)==16,"the hash folds a 128-bit product");
constexpr std::uint64_t hash_seed=0x2D358DCCAA6C78A5u;
constexpr std::uint64_t secret0=0xA0761D6478BD642Fu;
constexpr std::uint64_t secret1=0xE7037ED1A0B428DBu;
constexpr std::uint64_t secret2=0x8EBC6AF09C88C6DBu;
constexpr std::uint64_t secret3=0x589965CC75374CC3u;
constexpr void multiply(std::uint64_t& a,std::uint64_t& b) noexcept
{
const unsigned __int128 product=static_cast<unsigned __int128>(a)*b;
a=static_cast<std::uint64_t>(product);
b=static_cast<std::uint64_t>(product>>64);
}
constexpr std::uint64_t mix(std::uint64_t a,std::uint64_t b) noexcept
{
multiply(a,b);
return a^b;
}
// The values a hash of the wyhash structure is keyed with: the starting state and the four mixing secrets.
struct HashKeys {
std::uint64_t seed;
std::uint64_t secret0;
std::uint64_t secret1;
std::uint64_t secret2;
std::uint64_t secret3;
};
// String::hash(): wyhash's public secrets and a fixed seed, folded at compile time.
constexpr HashKeys stable_keys{hash_seed^mix(hash_seed^secret0,secret1),secret0,secret1,secret2,secret3};
// String::table_hash(): the seed and every secret drawn per process. Keying only the seed would not do: wyhash multiplies
// (input word ^ secret) by another word, so input words equal to a public secret zero the product and erase the seed and
// every other word mixed into it, which yields any number of keys with one hash whatever the seed (the flooding test builds
// such keys). With every secret unknown, no condition on the input alone zeroes or cancels a product: each multiply takes an
// input word xor a random secret times an input word xor the random-keyed state, so whether two inputs collide depends on
// the secrets, and an attacker who never sees a table hash cannot aim at it. Constant-initialized to zeros, filled by
// draw_table_keys before any other dynamic initializer, then never written again. Naturally aligned: a 64-byte alignment
// measured no faster (dictionary inserts and lookups were 0-3 % slower with it, within code-placement noise).
constinit HashKeys table_keys{};
#if !defined(NDEBUG)
constinit bool table_keys_drawn=false;
#endif
// Fills table_keys from the operating system's entropy source. Should the source fail, the clock and the address of a static
// seed a splitmix64 stream instead (still unknown to scripts and players, though weaker). Priority 101, the first one a
// program may use, so it runs before every other dynamic initializer (default priority) of the program, which may hash
// strings already. The secrets are uniform 64-bit words: wyhash's own secret maker only improves the mixing of a fixed public
// choice, and a random word is that poor with negligible chance (Go's runtime keys its wyhash-style hash the same way).
void splitmix_words(std::uint64_t state,std::uint64_t (&words)[5]) noexcept
{
for(std::uint64_t& word:words){
state+=0x9E3779B97F4A7C15u;
std::uint64_t value=state;
value=(value^(value>>30))*0xBF58476D1CE4E5B9u;
value=(value^(value>>27))*0x94D049BB133111EBu;
word=value^(value>>31);
}
}
// In a tree configured with ZODIAC_TEST_HOOKS=ON only (see platform/entropy.hpp), ZODIAC_TABLE_HASH_SEED=<n> replaces the
// drawn keys with the splitmix64 stream of n, so benches and tests can repeat one table layout in many processes.
[[gnu::constructor(101)]] void draw_table_keys() noexcept
{
std::uint64_t words[5];
#if defined(ZODIAC_TEST_HOOKS)
std::uint64_t fixed=0;
if(platform::test_hook_table_hash_seed(fixed))splitmix_words(fixed,words);
else
#endif
if(!platform::fill_entropy(words,sizeof words)){
static const char anchor=0;
splitmix_words(static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())^reinterpret_cast<std::uintptr_t>(&anchor),words);
}
table_keys=HashKeys{words[0],words[1],words[2],words[3],words[4]};
#if !defined(NDEBUG)
table_keys_drawn=true;
#endif
}
inline std::uint64_t read64(const byte* p) noexcept
{
std::uint64_t value;
std::memcpy(&value,p,8);
if constexpr(std::endian::native==std::endian::big)value=std::byteswap(value);
return value;
}
inline std::uint64_t read32(const byte* p) noexcept
{
std::uint32_t value;
std::memcpy(&value,p,4);
if constexpr(std::endian::native==std::endian::big)value=std::byteswap(value);
return value;
}

// The heap block: this header, then capacity bytes and the NUL. index is the sparse character index or null:
// index[0] entries built, index[1] entries allocated, index[2+k] the byte offset of character k*step.
struct HeapHeader {
std::uint32_t* index;
};
// Blocks of up to block_cache_limit bytes come in size classes of block_class_step bytes and are recycled through a
// per-thread cache of up to block_cache_depth blocks per class: the C runtime's allocator costs more than everything else
// a short heap string does (a copy, a concatenation, a dictionary key), and scripts make and drop those all the time. A
// block may be freed on any thread; it then goes to that thread's cache. A thread's cache is freed when the thread ends
// (at most block_classes*block_cache_depth blocks, about 70 KB); blocks freed after that go straight to the allocator.
// Under AddressSanitizer the cache is bypassed (blocks go straight to the allocator, size classes kept), so use after free and
// leaks of string blocks stay visible to it.
#if defined(__SANITIZE_ADDRESS__)
#define ZODIAC_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ZODIAC_UNDER_ASAN 1
#endif
#endif
#if defined(ZODIAC_UNDER_ASAN)
constexpr bool block_cache_on=false;
#else
constexpr bool block_cache_on=true;
#endif
constexpr std::size_t block_class_step=16;
constexpr std::size_t block_cache_limit=256;
constexpr std::size_t block_classes=block_cache_limit/block_class_step;
constexpr std::uint32_t block_cache_depth=32;
struct FreeBlock {
FreeBlock* next;
};
struct BlockCache {
FreeBlock* heads[block_classes];
std::uint32_t counts[block_classes];
~BlockCache();
};
// Trivially destructible, so it stays usable after the cache is destroyed (strings freed by later destructors).
thread_local bool block_cache_closed=false;
thread_local BlockCache block_cache{};
BlockCache::~BlockCache()
{
block_cache_closed=true;
for(FreeBlock* head:heads){
while(head){
FreeBlock* next=head->next;
heap::delete_block(static_cast<void*>(head));
head=next;
}
}
}
// The size class of a block of bytes bytes (bytes <= block_cache_limit): class k holds blocks of (k+1)*block_class_step.
inline std::size_t block_class(std::size_t bytes) noexcept
{
return (bytes-1)/block_class_step;
}
// A block for capacity bytes and the NUL; capacity is raised to all the block holds.
char* allocate_block(std::uint32_t& capacity)
{
std::size_t bytes=sizeof(HeapHeader)+std::size_t{capacity}+1;
void* block=nullptr;
if(bytes<=block_cache_limit){
const std::size_t k=block_class(bytes);
bytes=(k+1)*block_class_step;
capacity=static_cast<std::uint32_t>(bytes-sizeof(HeapHeader)-1);
if(block_cache_on&&!block_cache_closed){
BlockCache& cache=block_cache;
if(FreeBlock* head=cache.heads[k]){
cache.heads[k]=head->next;
--cache.counts[k];
block=head;
}
}
}
if(!block)block=heap::new_block(bytes);
auto* header=::new(block) HeapHeader{nullptr};
return reinterpret_cast<char*>(header+1);
}
inline HeapHeader* header_of(char* ptr) noexcept
{
return reinterpret_cast<HeapHeader*>(ptr)-1;
}
// Frees the block of ptr, allocated for capacity bytes (its index, if any, already freed or moved to another block).
void free_block(char* ptr,std::uint32_t capacity) noexcept
{
void* block=header_of(ptr);
const std::size_t bytes=sizeof(HeapHeader)+std::size_t{capacity}+1;
if(block_cache_on&&bytes<=block_cache_limit&&!block_cache_closed){
BlockCache& cache=block_cache;
const std::size_t k=block_class(bytes);
if(cache.counts[k]<block_cache_depth){
cache.heads[k]=::new(block) FreeBlock{cache.heads[k]};
++cache.counts[k];
return;
}
}
heap::delete_block(block);
}

// Number parsing. Clinger's fast path is exact when the significand fits in 53 bits and the power of ten is exact.
constexpr double exact_powers[]={1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22};
inline bool is_digit(char c) noexcept
{
return static_cast<unsigned char>(c-'0')<10;
}
// Correctly rounded conversion of text that already matched the grammar; text is NUL-terminated.
// from_chars where the library has it for double (libc++ 20 and later); strtod in the "C" locale otherwise and for
// results out of range (overflow to infinity, underflow and subnormals as strtod rounds them).
double convert_slow(std::string_view text) noexcept
{
#if defined(_LIBCPP_VERSION)&&_LIBCPP_VERSION>=200000
const char* first=text.data()+(text[0]=='+'?1:0);
double value=0;
const std::from_chars_result result=std::from_chars(first,text.data()+text.size(),value);
if(result.ec==std::errc{}&&result.ptr==text.data()+text.size())return value;
#endif
#if defined(_WIN32)
static const _locale_t c_locale=_create_locale(LC_ALL,"C");
return _strtod_l(text.data(),nullptr,c_locale);
#else
static const locale_t c_locale=newlocale(LC_ALL_MASK,"C",static_cast<locale_t>(0));
return strtod_l(text.data(),nullptr,c_locale);
#endif
}
// Parses the whole of text (to_number's grammar); text is NUL-terminated.
bool parse_number(std::string_view text,double& value) noexcept
{
const char* p=text.data();
const char* end=p+text.size();
bool negative=false;
if(p<end&&(*p=='+'||*p=='-')){
negative=*p=='-';
++p;
}
if(p==end)return false;
std::uint64_t significand=0;
int digits=0;
long long exponent=0;
bool any_digit=false;
bool inexact=false;
for(;p<end&&is_digit(*p);++p){
any_digit=true;
const auto digit=static_cast<unsigned>(*p-'0');
if(significand==0&&digit==0)continue;
if(digits<19){
significand=significand*10+digit;
++digits;
}else{
++exponent;
inexact=inexact||digit!=0;
}
}
if(p<end&&*p=='.'){
for(++p;p<end&&is_digit(*p);++p){
any_digit=true;
const auto digit=static_cast<unsigned>(*p-'0');
if(significand==0&&digit==0){
--exponent;
continue;
}
if(digits<19){
significand=significand*10+digit;
++digits;
--exponent;
}else{
inexact=inexact||digit!=0;
}
}
}
if(!any_digit)return false;
if(p<end&&(*p=='e'||*p=='E')){
++p;
bool negative_exponent=false;
if(p<end&&(*p=='+'||*p=='-')){
negative_exponent=*p=='-';
++p;
}
if(p==end||!is_digit(*p))return false;
long long written=0;
for(;p<end&&is_digit(*p);++p){
if(written<1000000)written=written*10+(*p-'0');
}
exponent+=negative_exponent?-written:written;
}
if(p!=end)return false;
if(significand==0){
value=negative?-0.0:0.0;
return true;
}
if(!inexact&&significand<=(std::uint64_t{1}<<53)&&exponent>=-22&&exponent<=22){
double result=static_cast<double>(significand);
result=exponent<0?result/exact_powers[-exponent]:result*exact_powers[exponent];
value=negative?-result:result;
return true;
}
value=convert_slow(text);
return true;
}
}

// Writes a new string front to back with amortized growth; used by operations whose result size is not known up front.
class StringWriter {
public:
explicit StringWriter(std::size_t capacity)
{
base_=out_.raw_reserve(std::min<std::size_t>(capacity,max_size));
cursor_=base_;
limit_=base_+out_.capacity_bytes();
}
// Room for at least size more bytes at the returned cursor; advance() commits what was written.
char* reserve(std::size_t size)
{
if(static_cast<std::size_t>(limit_-cursor_)<size)grow(size);
return cursor_;
}
void advance(std::size_t size) noexcept
{
cursor_+=size;
}
void put(const char* bytes,std::size_t size)
{
if(size==0)return;
copy_bytes(reserve(size),bytes,size);
cursor_+=size;
}
void put_byte(char value)
{
*reserve(1)=value;
++cursor_;
}
String finish(std::uint32_t meta)
{
out_.raw_set_size(static_cast<std::uint32_t>(cursor_-base_),meta);
return std::move(out_);
}
private:
void grow(std::size_t size)
{
const auto used=static_cast<std::size_t>(cursor_-base_);
if(size>max_size-used)too_long();
const std::size_t doubled=std::min<std::size_t>(std::size_t{out_.capacity_bytes()}*2,max_size);
out_.raw_set_size(static_cast<std::uint32_t>(used),meta_unknown);
base_=out_.raw_reserve(std::max(used+size,doubled));
cursor_=base_+used;
limit_=base_+out_.capacity_bytes();
}
String out_;
char* base_;
char* cursor_;
char* limit_;
};

namespace {
template<bool Upper,bool Turkic>
String map_case(std::string_view text)
{
const byte* p=ubytes(text.data());
const byte* end=p+text.size();
// Most text maps to the same size; starting there keeps results that fit inline, and the writer grows for the rest.
StringWriter out(text.size());
std::uint32_t count=0;
bool ascii=true;
constexpr byte trigger=Upper?'i':'I';
while(p<end){
if(end-p>=8){
const std::uint64_t word=load_word(p);
if((word&high_bits)==0&&(!Turkic||!has_byte(word,trigger))){
store_word(out.reserve(8),Upper?ascii_upper(word):ascii_lower(word));
out.advance(8);
p+=8;
count+=8;
continue;
}
}
const byte b=*p;
++count;
if(b<0x80){
if(Turkic&&b==trigger){
if constexpr(Upper){
char* w=out.reserve(2);
w[0]=static_cast<char>(0xC4);
w[1]=static_cast<char>(0xB0);
out.advance(2);
ascii=false;
++p;
continue;
}
// SpecialCasing After_I: I, then marks whose combining class is neither 0 nor 230, then U+0307 COMBINING DOT ABOVE
// lowers to i followed by those marks; the U+0307 is dropped. Otherwise I is dotless ı.
const byte* marks=p+1;
const byte* q=marks;
std::uint32_t mark_count=0;
while(q<end&&*q>=0x80){
const std::uint32_t size=char_size(q,end);
if(size==1)break;
const char32_t mark=decode(q,size);
if(mark==0x307)break;
const unsigned ccc=unicode::combining_class(mark);
if(ccc==0||ccc==230)break;
q+=size;
++mark_count;
}
if(end-q>=2&&q[0]==0xCC&&q[1]==0x87){
out.put_byte('i');
out.put(reinterpret_cast<const char*>(marks),static_cast<std::size_t>(q-marks));
count+=mark_count;
ascii=ascii&&mark_count==0;
p=q+2;
}else{
char* w=out.reserve(2);
w[0]=static_cast<char>(0xC4);
w[1]=static_cast<char>(0xB1);
out.advance(2);
ascii=false;
++p;
}
continue;
}
char mapped=static_cast<char>(b);
if constexpr(Upper){
if(static_cast<byte>(b-'a')<26)mapped=static_cast<char>(b-32);
}else{
if(static_cast<byte>(b-'A')<26)mapped=static_cast<char>(b+32);
}
out.put_byte(mapped);
++p;
continue;
}
const std::uint32_t n=char_size(p,end);
if(n==1){
out.put_byte(static_cast<char>(b));
ascii=false;
++p;
continue;
}
const char32_t c=decode(p,n);
char32_t mapped;
if constexpr(Turkic){
mapped=Upper?unicode::to_upper_turkic(c):unicode::to_lower_turkic(c);
}else{
mapped=Upper?unicode::to_upper(c):unicode::to_lower(c);
}
if(mapped==c){
out.put(reinterpret_cast<const char*>(p),n);
ascii=false;
}else{
out.advance(encode(mapped,out.reserve(4)));
ascii=ascii&&mapped<0x80;
}
p+=n;
}
// Every character maps to exactly one complete character (After_I drops one), so the count is exact.
return out.finish(count|(ascii?meta_ascii:0));
}
// "tr" or "az" in any case, alone or followed by '-' or '_' and more.
bool is_turkic_locale(std::string_view locale) noexcept
{
if(locale.size()<2||(locale.size()>2&&locale[2]!='-'&&locale[2]!='_'))return false;
const char a=static_cast<char>(locale[0]|0x20);
const char b=static_cast<char>(locale[1]|0x20);
return (a=='t'&&b=='r')||(a=='a'&&b=='z');
}
}

namespace string_detail {
[[gnu::noinline]] bool same_long_bytes(const char* a,const char* b,std::size_t n) noexcept
{
#if defined(__SSE2__)
for(std::size_t k=0;k+16<n;k+=16){
const __m128i x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+k));
const __m128i y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+k));
if(_mm_movemask_epi8(_mm_cmpeq_epi8(x,y))!=0xFFFF)return false;
}
const __m128i x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+n-16));
const __m128i y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+n-16));
return _mm_movemask_epi8(_mm_cmpeq_epi8(x,y))==0xFFFF;
#else
return std::memcmp(a,b,n)==0;
#endif
}

// The hashes of the wyhash structure under keys, in parts so the stable hash folds its keys to constants and inputs of up
// to 16 bytes (most table keys) stay a leaf that touches no callee-saved register: longer inputs leave by a tail call to
// hash_over16, and inputs over 48 bytes from there to hash_long, whose three-lane loop keeps six words live.
// The last step over the bytes read into a and b: the size is mixed in, as wyhash does.
template<const HashKeys& keys>
[[gnu::always_inline]] inline std::uint64_t hash_finish(std::uint64_t a,std::uint64_t b,std::uint64_t seed,std::size_t size) noexcept
{
a^=keys.secret1;
b^=seed;
multiply(a,b);
return mix(a^keys.secret0^size,b^keys.secret1);
}
// The remaining i bytes at p (17 to 48, or 1 to 48 after the lanes) of an input of size bytes, with the state seed.
template<const HashKeys& keys>
[[gnu::always_inline]] inline std::uint64_t hash_rest(const byte* p,std::size_t i,std::uint64_t seed,std::size_t size) noexcept
{
const std::uint64_t k1=keys.secret1;
while(i>16){
seed=mix(read64(p)^k1,read64(p+8)^seed);
i-=16;
p+=16;
}
return hash_finish<keys>(read64(p+i-16),read64(p+i-8),seed,size);
}
// Inputs of more than 48 bytes: 48-byte blocks in three lanes while more than 48 bytes remain, then the rest.
template<const HashKeys& keys>
[[gnu::noinline]] std::uint64_t hash_long(const byte* p,std::size_t size) noexcept
{
const std::uint64_t k1=keys.secret1;
const std::uint64_t k2=keys.secret2;
const std::uint64_t k3=keys.secret3;
std::uint64_t seed=keys.seed;
std::uint64_t see1=seed;
std::uint64_t see2=seed;
std::size_t i=size;
do{
seed=mix(read64(p)^k1,read64(p+8)^seed);
see1=mix(read64(p+16)^k2,read64(p+24)^see1);
see2=mix(read64(p+32)^k3,read64(p+40)^see2);
p+=48;
i-=48;
}while(i>48);
return hash_rest<keys>(p,i,seed^see1^see2,size);
}
// Inputs of more than 16 bytes.
template<const HashKeys& keys>
[[gnu::noinline]] std::uint64_t hash_over16(const byte* p,std::size_t size) noexcept
{
if(size>48)return hash_long<keys>(p,size);
return hash_rest<keys>(p,size,keys.seed,size);
}
template<const HashKeys& keys>
[[gnu::always_inline]] inline std::uint64_t keyed_hash(const char* data,std::size_t size) noexcept
{
const byte* p=ubytes(data);
if(size>16)return hash_over16<keys>(p,size);
std::uint64_t a=0;
std::uint64_t b=0;
if(size>=4){
const std::size_t shift=(size>>3)<<2;
a=(read32(p)<<32)|read32(p+shift);
b=(read32(p+size-4)<<32)|read32(p+size-4-shift);
}else if(size>0){
a=(std::uint64_t{p[0]}<<16)|(std::uint64_t{p[size>>1]}<<8)|p[size-1];
}
return hash_finish<keys>(a,b,keys.seed,size);
}

std::uint64_t hash_bytes(const char* data,std::size_t size) noexcept
{
return keyed_hash<stable_keys>(data,size);
}

std::uint64_t table_hash_bytes(const char* data,std::size_t size) noexcept
{
assert(table_keys_drawn);
return keyed_hash<table_keys>(data,size);
}

namespace {
std::uint64_t key_word(const HashKeys& keys,int index) noexcept
{
const std::uint64_t words[]={keys.seed,keys.secret0,keys.secret1,keys.secret2,keys.secret3};
return static_cast<unsigned>(index)<std::size(words)?words[index]:0;
}
}

std::uint64_t table_key_word(int index) noexcept
{
return key_word(table_keys,index);
}

std::uint64_t stable_key_word(int index) noexcept
{
return key_word(stable_keys,index);
}
}

void String::copy_heap(const String& other)
{
const std::uint32_t meta=other.load_meta();
const std::uint32_t size=other.size_bytes();
if(size<=static_cast<std::uint32_t>(inline_capacity)){
copy_bytes(rep_.storage.buffer,other.rep_.storage.heap.ptr,std::size_t{size}+1);
rep_.size=size;
rep_.meta=meta;
return;
}
std::uint32_t capacity=size;
char* ptr=allocate_block(capacity);
copy_bytes(ptr,other.rep_.storage.heap.ptr,std::size_t{size}+1);
rep_.storage.heap={ptr,capacity,0,0};
rep_.size=size|heap_bit;
rep_.meta=meta;
copy_hash(other);
}

void String::copy_hash(const String& other) noexcept
{
if(other.is_heap()&&__atomic_load_n(&other.rep_.storage.heap.hash_known,__ATOMIC_ACQUIRE)!=0){
rep_.storage.heap.hash=__atomic_load_n(&other.rep_.storage.heap.hash,__ATOMIC_RELAXED);
rep_.storage.heap.hash_known=1;
}else{
rep_.storage.heap.hash_known=0;
}
}

String& String::operator=(const String& other)
{
if(this==&other)return *this;
const std::uint32_t size=other.size_bytes();
const std::uint32_t meta=other.load_meta();
if(!is_heap()){
if(!other.is_heap()){
rep_.storage=other.rep_.storage;
rep_.size=other.rep_.size;
rep_.meta=meta;
return *this;
}
if(size<=static_cast<std::uint32_t>(inline_capacity)){
copy_bytes(rep_.storage.buffer,other.rep_.storage.heap.ptr,std::size_t{size}+1);
rep_.size=size;
rep_.meta=meta;
return *this;
}
}else if(rep_.storage.heap.capacity>=size){
free_index();
copy_bytes(rep_.storage.heap.ptr,other.data(),std::size_t{size}+1);
rep_.size=size|heap_bit;
rep_.meta=meta;
copy_hash(other);
return *this;
}
String copy(other);
return *this=std::move(copy);
}

String& String::operator=(std::string_view bytes)
{
if(bytes.size()>max_size)too_long();
const auto size=static_cast<std::uint32_t>(bytes.size());
if(size>capacity_bytes()){
// Copy first: bytes may view the storage about to be released.
String fresh(bytes);
return *this=std::move(fresh);
}
char* d=mutable_data();
if(size!=0)std::memmove(d,bytes.data(),size);
raw_set_size(size,meta_unknown);
return *this;
}

void String::init(const char* bytes,std::size_t size,std::uint32_t meta)
{
if(size>max_size)too_long();
const auto n=static_cast<std::uint32_t>(size);
if(n<=static_cast<std::uint32_t>(inline_capacity)){
if(n!=0)copy_bytes(rep_.storage.buffer,bytes,n);
rep_.storage.buffer[n]='\0';
rep_.size=n;
}else{
std::uint32_t capacity=n;
char* ptr=allocate_block(capacity);
copy_bytes(ptr,bytes,n);
ptr[n]='\0';
rep_.storage.heap={ptr,capacity,0,0};
rep_.size=n|heap_bit;
}
rep_.meta=n==0?meta_ascii:meta;
}

void String::release() noexcept
{
HeapHeader* header=header_of(rep_.storage.heap.ptr);
if(header->index!=nullptr)heap::delete_block(header->index);
free_block(rep_.storage.heap.ptr,rep_.storage.heap.capacity);
}

void String::free_index() noexcept
{
HeapHeader* header=header_of(rep_.storage.heap.ptr);
if(header->index!=nullptr){
heap::delete_block(header->index);
header->index=nullptr;
}
}

int String::scan() const noexcept
{
const byte* p=ubytes(data());
bool ascii=true;
const std::uint32_t count=count_chars(p,p+size_bytes(),ascii);
// Concurrent const callers all store the same value, so a relaxed store is enough.
string_detail::store_relaxed(rep_.meta,count|(ascii?meta_ascii:0));
return static_cast<int>(count);
}

bool String::settle_ascii() const noexcept
{
// The count is known and equals the byte length: the text is ASCII unless a byte is 0x80 or above (an invalid byte).
const byte* p=ubytes(data());
const std::uint32_t size=size_bytes();
std::uint64_t high=0;
std::uint32_t i=0;
for(;i+8<=size;i+=8)high|=load_word(p+i);
for(;i<size;++i)high|=p[i];
if((high&high_bits)!=0)return false;
string_detail::store_relaxed(rep_.meta,size|meta_ascii);
return true;
}

std::uint64_t String::cache_hash() const noexcept
{
const std::uint64_t value=string_detail::table_hash_bytes(rep_.storage.heap.ptr,size_bytes());
__atomic_store_n(&rep_.storage.heap.hash,value,__ATOMIC_RELAXED);
__atomic_store_n(&rep_.storage.heap.hash_known,std::uint32_t{1},__ATOMIC_RELEASE);
return value;
}

void String::reallocate(std::uint32_t capacity)
{
const std::uint32_t size=size_bytes();
char* ptr=allocate_block(capacity);
copy_bytes(ptr,data(),size);
ptr[size]='\0';
if(is_heap()){
HeapHeader* old_header=header_of(rep_.storage.heap.ptr);
header_of(ptr)->index=old_header->index;
free_block(rep_.storage.heap.ptr,rep_.storage.heap.capacity);
rep_.storage.heap.ptr=ptr;
rep_.storage.heap.capacity=capacity;
}else{
rep_.storage.heap={ptr,capacity,0,0};
rep_.size|=heap_bit;
}
}

char* String::raw_reserve(std::size_t capacity)
{
if(capacity>max_size)too_long();
if(capacity>capacity_bytes())reallocate(static_cast<std::uint32_t>(capacity));
return mutable_data();
}

void String::raw_set_size(std::uint32_t size,std::uint32_t meta) noexcept
{
mutable_data()[size]='\0';
rep_.size=size|(rep_.size&heap_bit);
rep_.meta=size==0?meta_ascii:meta;
if(is_heap()){
rep_.storage.heap.hash_known=0;
free_index();
}
}

void String::reserve(int bytes)
{
if(bytes>0)raw_reserve(static_cast<std::size_t>(bytes));
}

void String::clear() noexcept
{
raw_set_size(0,meta_ascii);
}

// The common append: the bytes fit the current storage. No call is made unless the text has a sparse index to update.
String& String::append_raw(const char* bytes,std::size_t size,std::uint32_t meta)
{
if(size==0)return *this;
const std::uint32_t old_size=size_bytes();
// capacity_bytes() never exceeds max_size, so a fit cannot overflow.
if(size>capacity_bytes()-old_size)return append_grown(bytes,size,meta);
const bool joins=is_continuation(static_cast<byte>(bytes[0]));
const std::uint32_t new_meta=joined_meta(rep_.meta,meta,std::string_view(bytes,size));
const auto total=old_size+static_cast<std::uint32_t>(size);
char* d=mutable_data();
// bytes may view this string's own text, which lies before old_size and so never overlaps the copy.
copy_bytes(d+old_size,bytes,size);
d[total]='\0';
rep_.size=total|(rep_.size&heap_bit);
rep_.meta=new_meta;
if(is_heap()){
rep_.storage.heap.hash_known=0;
// Only non-ASCII text has an index, so a result known to be ASCII (which needs ASCII on both sides) had none.
const bool known_ascii=new_meta!=meta_unknown&&(new_meta&meta_ascii)!=0;
if(!known_ascii&&header_of(rep_.storage.heap.ptr)->index!=nullptr)update_index(joins);
}
return *this;
}

void String::update_index(bool joins) noexcept
{
// Earlier characters (and so the index entries) change only when the new bytes start with a continuation byte that
// completes a sequence the old end left open; otherwise the index is extended over the new text.
if(joins){
free_index();
return;
}
// The new bytes, size and meta are already committed when the index is extended, so a std::bad_alloc from growing its
// block must not leave the old index (which covers only the old text) published: it is dropped instead, and the next
// indexed read rebuilds it over the whole text. extend_index allocates before it writes anything, so the old block is
// intact for free_index. Table-based unwinding makes the handler free on the path that does not throw, which is cheaper
// than allocating the grown block before the commit (that needs the character count of the new text first).
try{
extend_index();
}
catch(...){
free_index();
}
}

// An append that needs a larger block.
String& String::append_grown(const char* bytes,std::size_t size,std::uint32_t meta)
{
const std::uint32_t old_size=size_bytes();
if(size>max_size-old_size)too_long();
const auto total=old_size+static_cast<std::uint32_t>(size);
const bool joins=is_continuation(static_cast<byte>(bytes[0]));
const std::uint32_t new_meta=joined_meta(rep_.meta,meta,std::string_view(bytes,size));
if(total>capacity_bytes()){
const auto base=reinterpret_cast<std::uintptr_t>(data());
const auto source=reinterpret_cast<std::uintptr_t>(bytes);
const bool own=source>=base&&source<base+old_size;
reallocate(std::max(total,std::min(capacity_bytes()*2,max_size)));
if(own)bytes=data()+(source-base);
}
char* d=mutable_data();
copy_bytes(d+old_size,bytes,size);
d[total]='\0';
rep_.size=total|(rep_.size&heap_bit);
rep_.meta=new_meta;
if(is_heap()){
rep_.storage.heap.hash_known=0;
if(header_of(rep_.storage.heap.ptr)->index!=nullptr)update_index(joins);
}
return *this;
}

void String::extend_index()
{
// Non-const (exclusive access), so the block may be grown in place. An index exists only for text with a multi-byte
// character, so the result is non-ASCII; the count of the new text falls out of the extension.
HeapHeader* header=header_of(rep_.storage.heap.ptr);
std::uint32_t* block=header->index;
const byte* begin=ubytes(rep_.storage.heap.ptr);
const byte* end=begin+size_bytes();
std::uint32_t entries=block[0];
const byte* last=begin+block[1+entries];
bool ascii=true;
const std::uint32_t length=(entries-1)*step+count_chars(last,end,ascii);
const std::uint32_t needed=(length-1)/step+1;
if(needed>block[1]){
const std::uint32_t capacity=std::max(needed,block[1]*2);
auto* grown=static_cast<std::uint32_t*>(heap::new_block(sizeof(std::uint32_t)*(std::size_t{capacity}+2)));
std::memcpy(grown,block,sizeof(std::uint32_t)*(std::size_t{entries}+2));
heap::delete_block(block);
grown[1]=capacity;
header->index=block=grown;
}
for(const byte* p=last;entries<needed;++entries){
p=skip_chars(p,end,step);
block[2+entries]=static_cast<std::uint32_t>(p-begin);
}
block[0]=entries;
rep_.meta=length;
}

std::uint32_t String::view_meta(std::string_view bytes) noexcept
{
// Short texts are counted on the spot (cheaper than a later rescan of the whole result); long ones stay unknown.
if(bytes.empty()||bytes.size()>64)return bytes.empty()?meta_ascii:meta_unknown;
const byte* p=ubytes(bytes.data());
bool ascii=true;
const std::uint32_t count=count_chars(p,p+bytes.size(),ascii);
return count|(ascii?meta_ascii:0);
}

String& String::append(const String& other)
{
return append_raw(other.data(),other.size_bytes(),other.load_meta());
}

String& String::append(std::string_view bytes)
{
return append_raw(bytes.data(),bytes.size(),rep_.meta==meta_unknown?meta_unknown:view_meta(bytes));
}

String& String::append(char32_t code_point)
{
char buffer[4];
const std::uint32_t size=encode(code_point,buffer);
return append_raw(buffer,size,size==1?1|meta_ascii:1);
}

String& String::append(bool value)
{
return value?append_raw("true",4,4|meta_ascii):append_raw("false",5,5|meta_ascii);
}

String& String::append(double value)
{
char buffer[number_buffer];
const std::uint32_t size=format_number(value,buffer);
return append_raw(buffer,size,size|meta_ascii);
}

String& String::append(float value)
{
char buffer[number_buffer];
const std::uint32_t size=format_number(value,buffer);
return append_raw(buffer,size,size|meta_ascii);
}

String& String::append_signed(long long value)
{
char buffer[24];
const auto size=static_cast<std::uint32_t>(std::to_chars(buffer,buffer+sizeof(buffer),value).ptr-buffer);
return append_raw(buffer,size,size|meta_ascii);
}

String& String::append_unsigned(unsigned long long value)
{
char buffer[24];
const auto size=static_cast<std::uint32_t>(std::to_chars(buffer,buffer+sizeof(buffer),value).ptr-buffer);
return append_raw(buffer,size,size|meta_ascii);
}

String String::concat(std::string_view a,std::uint32_t a_meta,std::string_view b,std::uint32_t b_meta)
{
if(a.size()+b.size()>max_size)too_long();
String out=with_size(a.size()+b.size());
char* d=out.mutable_data();
if(!a.empty())copy_bytes(d,a.data(),a.size());
if(!b.empty())copy_bytes(d+a.size(),b.data(),b.size());
if(!out.empty())out.rep_.meta=a.empty()?b_meta:joined_meta(a_meta,b_meta,b);
return out;
}

String String::with_size(std::size_t size)
{
String out;
out.raw_reserve(size);
out.raw_set_size(static_cast<std::uint32_t>(size),meta_unknown);
return out;
}

String String::from_code_point(char32_t code_point)
{
String text;
text.append(code_point);
return text;
}

String String::from_bool(bool value)
{
return value?String("true",4,4|meta_ascii):String("false",5,5|meta_ascii);
}

String String::from_number(double value)
{
char buffer[number_buffer];
const std::uint32_t size=format_number(value,buffer);
return String(buffer,size,size|meta_ascii);
}

String String::from_number(float value)
{
char buffer[number_buffer];
const std::uint32_t size=format_number(value,buffer);
return String(buffer,size,size|meta_ascii);
}

const std::uint32_t* String::index() const
{
// A published index always covers the whole text (appends extend it, other mutations free it), so readers never change
// it. The first reader builds it in one go and publishes it with a release compare-and-swap; a reader that loses the race
// frees its copy and uses the winner's.
char* ptr=rep_.storage.heap.ptr;
HeapHeader* header=header_of(ptr);
std::uint32_t* block=__atomic_load_n(&header->index,__ATOMIC_ACQUIRE);
if(block!=nullptr)return block+2;
const std::uint32_t needed=((load_meta()&meta_length)-1)/step+1;
auto* built=static_cast<std::uint32_t*>(heap::new_block(sizeof(std::uint32_t)*(std::size_t{needed}+2)));
built[0]=needed;
built[1]=needed;
built[2]=0;
const byte* begin=ubytes(ptr);
const byte* end=begin+size_bytes();
const byte* p=begin;
for(std::uint32_t k=1;k<needed;++k){
p=skip_chars(p,end,step);
built[2+k]=static_cast<std::uint32_t>(p-begin);
}
std::uint32_t* expected=nullptr;
if(__atomic_compare_exchange_n(&header->index,&expected,built,false,__ATOMIC_RELEASE,__ATOMIC_ACQUIRE))return built+2;
heap::delete_block(built);
return expected+2;
}

std::uint32_t String::char_to_byte(std::uint32_t position) const
{
const std::uint32_t size=size_bytes();
const std::uint32_t length=load_meta()&meta_length;
if(length==size)return position;
if(position==length)return size;
const byte* begin=ubytes(data());
const byte* end=begin+size;
if(position<step||!is_heap())return static_cast<std::uint32_t>(skip_chars(begin,end,position)-begin);
const std::uint32_t k=position/step;
const std::uint32_t offset=index()[k];
return static_cast<std::uint32_t>(skip_chars(begin+offset,end,position-k*step)-begin);
}

std::uint32_t String::byte_to_char(std::uint32_t offset) const
{
const std::uint32_t size=size_bytes();
const std::uint32_t length=load_meta()&meta_length;
if(length==size)return offset;
if(offset==size)return length;
const byte* begin=ubytes(data());
bool ascii=true;
if(offset<=4*step||!is_heap())return count_chars(begin,begin+offset,ascii);
const std::uint32_t* offsets=index();
const std::uint32_t entries=(length-1)/step+1;
const auto k=static_cast<std::uint32_t>(std::upper_bound(offsets,offsets+entries,offset)-offsets)-1;
return k*step+count_chars(begin+offsets[k],begin+offset,ascii);
}

String String::bytes_between(std::uint32_t begin,std::uint32_t end,std::uint32_t meta) const
{
if(begin==0&&end==size_bytes())return *this;
return String(data()+begin,end-begin,meta);
}

std::expected<String,StringError> String::char_at(int index) const
{
const int length=this->length();
if(index<0||index>=length)return std::unexpected(StringError::index_out_of_range);
const std::uint32_t offset=char_to_byte(static_cast<std::uint32_t>(index));
const byte* begin=ubytes(data());
const byte* p=begin+offset;
const std::uint32_t size=static_cast<std::uint32_t>(length)==size_bytes()?1:char_size(p,begin+size_bytes());
return String(data()+offset,size,*p<0x80?1|meta_ascii:1);
}

std::expected<char32_t,StringError> String::multibyte_code_at(int index) const
{
const int length=this->length();
if(index<0||index>=length)return std::unexpected(StringError::index_out_of_range);
const byte* begin=ubytes(data());
const byte* p=begin+char_to_byte(static_cast<std::uint32_t>(index));
return decode(p,char_size(p,begin+size_bytes()));
}

std::expected<std::uint8_t,StringError> String::byte_at(int index) const
{
if(index<0||index>=byte_length())return std::unexpected(StringError::index_out_of_range);
return static_cast<std::uint8_t>(data()[index]);
}

int String::find_simple(const char* needle,std::size_t size,int start) const noexcept
{
assert(start>=0&&static_cast<std::uint32_t>(start)<=size_bytes());
assert(static_cast<std::uint32_t>(length())==size_bytes());
const auto from=static_cast<std::size_t>(start);
const std::size_t pos=search(ubytes(data())+from,std::size_t{size_bytes()}-from,ubytes(needle),size);
return pos==npos?-1:static_cast<int>(pos+from);
}

std::expected<int,StringError> String::find_multibyte(std::string_view needle,int start) const
{
if(needle.empty())return start;
const std::size_t pos=find_aligned(bytes(),needle,char_to_byte(static_cast<std::uint32_t>(start)));
if(pos==npos)return -1;
return static_cast<int>(byte_to_char(static_cast<std::uint32_t>(pos)));
}

std::expected<int,StringError> String::rfind(std::string_view needle,int start) const
{
const int length=this->length();
if(start==-1)start=length;
else if(start<0||start>length)return std::unexpected(StringError::index_out_of_range);
if(needle.empty())return start;
if(static_cast<std::uint32_t>(length)==size_bytes()){
// Every character is one byte: indices are byte offsets and every offset is a character boundary.
const std::size_t pos=bytes().rfind(needle,static_cast<std::size_t>(start));
return pos==npos?-1:static_cast<int>(pos);
}
const std::size_t pos=rfind_aligned(bytes(),needle,char_to_byte(static_cast<std::uint32_t>(start)));
if(pos==npos)return -1;
return static_cast<int>(byte_to_char(static_cast<std::uint32_t>(pos)));
}

bool String::contains(std::string_view needle) const noexcept
{
return needle.empty()||find_aligned(bytes(),needle,0)!=npos;
}

bool String::starts_with(std::string_view prefix) const noexcept
{
const std::string_view text=bytes();
if(!text.starts_with(prefix))return false;
const byte* begin=ubytes(text.data());
return is_boundary(begin,begin+text.size(),begin+prefix.size());
}

bool String::ends_with(std::string_view suffix) const noexcept
{
const std::string_view text=bytes();
if(!text.ends_with(suffix))return false;
const byte* begin=ubytes(text.data());
return is_boundary(begin,begin+text.size(),begin+text.size()-suffix.size());
}

int String::count(std::string_view needle) const noexcept
{
if(needle.empty())return 0;
const std::string_view text=bytes();
int matches=0;
for(std::size_t pos=find_aligned(text,needle,0);pos!=npos;pos=find_aligned(text,needle,pos+needle.size()))++matches;
return matches;
}

std::expected<String,StringError> String::substr(int start,int count) const
{
const int length=this->length();
if(start<0||start>length)return std::unexpected(StringError::index_out_of_range);
if(count<-1)return std::unexpected(StringError::negative_count);
const auto available=static_cast<std::uint32_t>(length-start);
const std::uint32_t n=count==-1||static_cast<std::uint32_t>(count)>available?available:static_cast<std::uint32_t>(count);
const std::uint32_t begin=char_to_byte(static_cast<std::uint32_t>(start));
std::uint32_t end;
if(n==available){
end=size_bytes();
}else if(static_cast<std::uint32_t>(length)==size_bytes()){
end=begin+n;
}else if(n<step){
const byte* base=ubytes(data());
end=static_cast<std::uint32_t>(skip_chars(base+begin,base+size_bytes(),n)-base);
}else{
end=char_to_byte(static_cast<std::uint32_t>(start)+n);
}
// A part between two character boundaries decodes as it did in the whole string: its count is n.
return bytes_between(begin,end,part_meta(n));
}

std::expected<String,StringError> String::left(int count) const
{
if(count<0)return std::unexpected(StringError::negative_count);
return substr(0,count);
}

std::expected<String,StringError> String::right(int count) const
{
if(count<0)return std::unexpected(StringError::negative_count);
const int length=this->length();
const int n=std::min(count,length);
return substr(length-n,n);
}

std::vector<String> String::split(std::string_view delimiter,bool keep_empty) const
{
std::vector<String> parts;
const std::string_view text=bytes();
if(delimiter.empty()){
parts.reserve(static_cast<std::size_t>(length()));
const byte* begin=ubytes(text.data());
const byte* end=begin+text.size();
for(const byte* p=begin;p<end;){
const std::uint32_t size=*p<0x80?1:char_size(p,end);
parts.push_back(String(reinterpret_cast<const char*>(p),size,*p<0x80?1|meta_ascii:1));
p+=size;
}
return parts;
}
// When every character is one byte, each part's count is its byte length; otherwise parts are counted lazily (counting
// them here would cost a scan of the whole text that split itself does not need).
const std::uint32_t meta=load_meta();
const bool simple=meta!=meta_unknown&&(meta&meta_length)==size_bytes();
std::size_t from=0;
for(;;){
const std::size_t pos=find_aligned(text,delimiter,from);
const std::size_t end=pos==npos?text.size():pos;
if(keep_empty||end>from){
const auto size=static_cast<std::uint32_t>(end-from);
parts.push_back(String(text.data()+from,size,simple?size|(meta&meta_ascii):meta_unknown));
}
if(pos==npos)break;
from=pos+delimiter.size();
}
return parts;
}

String String::join(std::span<const String> parts,std::string_view separator)
{
if(parts.empty())return String();
std::size_t total=separator.size()*(parts.size()-1);
for(const String& part:parts)total+=part.size_bytes();
if(total>max_size)too_long();
String out=with_size(total);
char* d=out.mutable_data();
bool first=true;
for(const String& part:parts){
if(!first&&!separator.empty()){
copy_bytes(d,separator.data(),separator.size());
d+=separator.size();
}
first=false;
copy_bytes(d,part.data(),part.size_bytes());
d+=part.size_bytes();
}
return out;
}

String String::replace(std::string_view old_text,std::string_view new_text,bool all) const
{
if(old_text.empty())return *this;
const std::string_view text=bytes();
std::size_t pos=find_aligned(text,old_text,0);
if(pos==npos)return *this;
// Start at the input size (results that fit stay inline); the writer grows when replacements are longer.
StringWriter out(text.size()+(all||new_text.size()<=old_text.size()?0:new_text.size()-old_text.size()));
std::size_t from=0;
do{
out.put(text.data()+from,pos-from);
out.put(new_text.data(),new_text.size());
from=pos+old_text.size();
if(!all)break;
pos=find_aligned(text,old_text,from);
}while(pos!=npos);
out.put(text.data()+from,text.size()-from);
return out.finish(meta_unknown);
}

String String::trimmed(bool left,bool right) const
{
const byte* begin=ubytes(data());
const byte* end=begin+size_bytes();
const byte* p=begin;
std::uint32_t removed=0;
while(left&&p<end){
if(*p<0x80){
if(!unicode::is_whitespace(*p))break;
++p;
++removed;
continue;
}
const std::uint32_t size=char_size(p,end);
if(size==1||!unicode::is_whitespace(decode(p,size)))break;
p+=size;
++removed;
}
const byte* e=end;
while(right&&e>p){
if(e[-1]<0x80){
if(!unicode::is_whitespace(e[-1]))break;
--e;
++removed;
continue;
}
const byte* start=previous_char(p,e);
const auto size=static_cast<std::uint32_t>(e-start);
if(size==1||!unicode::is_whitespace(decode(start,size)))break;
e=start;
++removed;
}
// The result lies between character boundaries, so its count is the original minus the removed characters.
const std::uint32_t meta=load_meta();
return bytes_between(static_cast<std::uint32_t>(p-begin),static_cast<std::uint32_t>(e-begin),meta==meta_unknown?meta_unknown:part_meta((meta&meta_length)-removed));
}

String String::trim_left() const
{
return trimmed(true,false);
}

String String::trim_right() const
{
return trimmed(false,true);
}

String String::trim() const
{
return trimmed(true,true);
}

String String::lower() const
{
return map_case<false,false>(bytes());
}

String String::upper() const
{
return map_case<true,false>(bytes());
}

String String::lower(std::string_view locale) const
{
return is_turkic_locale(locale)?map_case<false,true>(bytes()):map_case<false,false>(bytes());
}

String String::upper(std::string_view locale) const
{
return is_turkic_locale(locale)?map_case<true,true>(bytes()):map_case<true,false>(bytes());
}

std::expected<String,StringError> String::repeat(int count) const
{
if(count<0)return std::unexpected(StringError::negative_count);
const std::uint32_t size=size_bytes();
if(count==0||size==0)return String();
if(static_cast<std::uint64_t>(size)*static_cast<std::uint64_t>(count)>max_size)too_long();
const std::uint32_t total=size*static_cast<std::uint32_t>(count);
String out=with_size(total);
char* d=out.mutable_data();
copy_bytes(d,data(),size);
for(std::uint32_t copied=size;copied<total;){
const std::uint32_t chunk=std::min(copied,total-copied);
copy_bytes(d+copied,d,chunk);
copied+=chunk;
}
const std::uint32_t meta=load_meta();
if(meta!=meta_unknown&&!is_continuation(static_cast<byte>(d[0])))out.rep_.meta=((meta&meta_length)*static_cast<std::uint32_t>(count))|(meta&meta_ascii);
return out;
}

String String::reverse() const
{
const std::uint32_t size=size_bytes();
if(size<=1)return *this;
String out=with_size(size);
char* d=out.mutable_data();
const byte* begin=ubytes(data());
const byte* end=begin+size;
std::uint32_t count=0;
bool ascii=true;
bool valid=true;
for(const byte* p=begin;p<end;){
const auto at=static_cast<std::uint32_t>(p-begin);
if(end-p>=8){
const std::uint64_t word=load_word(p);
if((word&high_bits)==0){
store_word(d+size-at-8,std::byteswap(word));
p+=8;
count+=8;
continue;
}
}
std::uint32_t n=1;
if(*p>=0x80){
ascii=false;
n=char_size(p,end);
valid=valid&&n!=1;
}
copy_bytes(d+size-at-n,reinterpret_cast<const char*>(p),n);
p+=n;
++count;
}
// Reversing can join invalid bytes into a valid sequence; when every character was valid the count is unchanged.
if(valid)out.rep_.meta=count|(ascii?meta_ascii:0);
return out;
}

std::expected<String,StringError> String::insert(int index,std::string_view text) const
{
const int length=this->length();
if(index<0||index>length)return std::unexpected(StringError::index_out_of_range);
const std::uint32_t at=char_to_byte(static_cast<std::uint32_t>(index));
const std::uint32_t size=size_bytes();
if(text.size()>max_size-size)too_long();
String out=with_size(size+text.size());
char* d=out.mutable_data();
const char* s=data();
copy_bytes(d,s,at);
if(!text.empty())copy_bytes(d+at,text.data(),text.size());
copy_bytes(d+at+text.size(),s+at,size-at);
return out;
}

std::expected<String,StringError> String::erase(int start,int count) const
{
const int length=this->length();
if(start<0||start>length)return std::unexpected(StringError::index_out_of_range);
if(count<-1)return std::unexpected(StringError::negative_count);
const auto available=static_cast<std::uint32_t>(length-start);
const std::uint32_t n=count==-1||static_cast<std::uint32_t>(count)>available?available:static_cast<std::uint32_t>(count);
if(n==0)return *this;
const std::uint32_t size=size_bytes();
const std::uint32_t begin=char_to_byte(static_cast<std::uint32_t>(start));
const std::uint32_t end=n==available?size:char_to_byte(static_cast<std::uint32_t>(start)+n);
String out=with_size(size-(end-begin));
char* d=out.mutable_data();
copy_bytes(d,data(),begin);
copy_bytes(d+begin,data()+end,size-end);
// The two parts keep their counts unless the second starts with a continuation byte that can join the first's end.
if(!out.empty()&&(end==size||!is_continuation(static_cast<byte>(data()[end]))))out.rep_.meta=part_meta(static_cast<std::uint32_t>(length)-n);
return out;
}

String String::normalize() const
{
const std::string_view text=bytes();
// Every code point below U+0300 has NFC_Quick_Check=Yes and combining class 0, and those are exactly the characters whose
// bytes are all below 0xCC, so such text (all ASCII and Turkish among it) is already NFC.
byte top=0;
for(const char c:text)top=std::max(top,static_cast<byte>(c));
if(top<0xCC)return *this;
const byte* p=ubytes(text.data());
const byte* end=p+text.size();
StringWriter out(text.size());
std::u32string run;
const byte* run_begin=p;
const auto flush=[&](const byte* run_end){
if(run_begin==run_end)return;
if(unicode::quick_check_nfc(run)){
out.put(reinterpret_cast<const char*>(run_begin),static_cast<std::size_t>(run_end-run_begin));
}else{
for(const char32_t c:unicode::nfc(run))out.advance(encode(c,out.reserve(4)));
}
run.clear();
};
while(p<end){
const std::uint32_t size=*p<0x80?1:char_size(p,end);
if(size==1&&*p>=0x80){
flush(p);
out.put_byte(static_cast<char>(*p));
++p;
run_begin=p;
continue;
}
run.push_back(decode(p,size));
p+=size;
}
flush(end);
return out.finish(meta_unknown);
}

std::expected<double,StringError> String::to_number() const
{
double value=0;
if(!parse_number(bytes(),value))return std::unexpected(StringError::not_a_number);
return value;
}

std::expected<std::int64_t,StringError> String::to_int() const
{
const std::string_view text=bytes();
std::size_t i=0;
const bool negative=!text.empty()&&text[0]=='-';
if(!text.empty()&&(text[0]=='-'||text[0]=='+'))i=1;
if(i<text.size()){
std::uint64_t magnitude=0;
bool overflow=false;
std::size_t k=i;
for(;k<text.size()&&is_digit(text[k]);++k){
const auto digit=static_cast<std::uint64_t>(text[k]-'0');
if(magnitude>(std::numeric_limits<std::uint64_t>::max()-digit)/10)overflow=true;
else magnitude=magnitude*10+digit;
}
if(k==text.size()){
constexpr std::uint64_t max_positive=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
if(negative){
if(overflow||magnitude>max_positive+1)return std::numeric_limits<std::int64_t>::min();
return magnitude==max_positive+1?std::numeric_limits<std::int64_t>::min():-static_cast<std::int64_t>(magnitude);
}
if(overflow||magnitude>max_positive)return std::numeric_limits<std::int64_t>::max();
return static_cast<std::int64_t>(magnitude);
}
}
double value=0;
if(!parse_number(text,value))return std::unexpected(StringError::not_a_number);
constexpr double limit=9223372036854775808.0;
if(value>=limit)return std::numeric_limits<std::int64_t>::max();
if(value<=-limit)return std::numeric_limits<std::int64_t>::min();
return static_cast<std::int64_t>(value);
}

bool String::is_number() const noexcept
{
double value=0;
return parse_number(bytes(),value);
}

bool String::is_digits() const noexcept
{
const std::string_view text=bytes();
if(text.empty())return false;
// No early exit, so the loop vectorizes.
unsigned other=0;
for(const char c:text)other|=static_cast<unsigned char>(c-'0')>=10?1u:0u;
return other==0;
}
}
