// zodiac::String: Zodiac's UTF-8 string value type, the pure C++ core behind the script `string` (spec 8.1).
// Contract:
// - Bytes are kept exactly as given (embedded NULs included), always followed by a NUL so data() can be handed to C code,
//   at most max_byte_length bytes (longer results throw std::bad_alloc, like running out of memory).
// - A character is a Unicode code point. A byte that does not start a well-formed UTF-8 sequence (Unicode Table 3-7: no
//   overlongs, surrogates, values above U+10FFFF, truncated or 5/6-byte forms) is one character of its own, so length(),
//   indices and counts are consistent for any bytes, invalid bytes survive every operation, and no operation splits a sequence
//   (searches only match whole characters). code_at() of such a byte is U+FFFD.
// - Layout (32 bytes): up to inline_capacity bytes live inside the object without allocation; longer text lives in one heap
//   block (an 8-byte header for the sparse index, then the bytes). Copies of heap strings that fit are stored inline.
//   Blocks of up to 256 bytes come in 16-byte size classes (the capacity is all the class holds) and are recycled through
//   a per-thread cache of at most 32 blocks per class, freed when the thread ends; any thread may free any string.
// - Metadata is computed lazily and invalidated on every mutation (or carried over exactly when an operation provably
//   cannot change the character count): the character count with an all-ASCII flag (one word-at-a-time scan), the 64-bit
//   table hash (cached for heap strings; inline strings hash their at most 23 bytes on each call), and for non-ASCII heap text
//   longer than index_step characters a sparse index with the byte offset of every index_step-th character, so indexed
//   access, substr and the character position of a search hit scan at most index_step characters. The index always covers
//   the whole text: const members build it in one go, appends extend it. When every character is one byte, indices are
//   byte offsets and no index exists.
// - hash() is a fixed-seed wyhash-style 64-bit hash of the bytes read little-endian: equal on every platform and run, the
//   script's `hash()`, computed on each call (never cached). table_hash() is the hash every hash table keyed by String uses
//   (the dictionary, std::hash<String>): the same wyhash structure with its seed and all four mixing secrets drawn per
//   process from the operating system's entropy source, so it is equal for equal bytes within one process only, and key
//   sets built offline to collide in hash() (fully or in some bits) spread in tables like any other keys.
// - Range rules: lengths, indices and counts are int and "not found" is -1. char_at/code_at need 0 <= i < length(),
//   byte_at 0 <= i < byte_length(); find/substr/insert/erase starts must lie in [0, length()] (rfind also takes -1 = end);
//   counts that run past the end are clamped; a negative count other than the -1 "to the end" default is negative_count.
//   Violations are returned as StringError (no C++ exceptions for script errors); the binding maps them to exception texts.
// - Comparisons are by bytes (unsigned), which for valid UTF-8 is code point order.
// - Thread safety like a standard container: any number of threads may call const members (and copy) one String at the
//   same time; the caches they fill are written atomically (relaxed counts, release/acquire for the table hash and the index
//   pointer, which is published once by compare-and-swap and only replaced or freed by non-const members). Non-const
//   members need exclusive access.
// - Alignment: alignof(String) is 8, but a String may live at an address that is only 4 mod 8: AngelScript places value
//   types on its script stack at 4-byte granularity, and the dictionary list literal's buffer holds keys at 4-byte boundaries
//   (DictionaryCache::make_from_list in dictionary.cpp reads them in place). That is safe only because the in-object 64-bit
//   field, Heap::hash, is read and written only with relaxed loads and stores, which compile to plain moves that x86-64 and
//   ARM64 perform at any address. Such a move may not be single-copy atomic when it crosses a cache line, which is harmless
//   here: every thread that stores the table hash stores the same value (the secrets are fixed before main and before any
//   other static initializer runs), and readers load it only after an acquire load of
//   hash_known has seen the release store that follows a complete store. Acquire, release, compare-and-swap and
//   read-modify-write operations are used only on 4-aligned 32-bit fields (Heap::hash_known, Rep::meta) and on the heap
//   block's header (the index pointer), which the allocator aligns; those would fault or tear on a misaligned address.
#pragma once
#include <cassert>
#include <climits>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zodiac {
// Script-visible failures; the binding turns them into "index out of range", "\"<text>\" is not a number", "negative count".
enum class StringError:std::uint8_t {index_out_of_range,not_a_number,negative_count};

namespace string_detail {
// Integer types formatted as numbers: every integral type except bool and the character types.
template<typename T>
concept integer_value=std::integral<T>&&!std::is_same_v<T,bool>&&!std::is_same_v<T,char>&&!std::is_same_v<T,wchar_t>&&!std::is_same_v<T,char8_t>&&!std::is_same_v<T,char16_t>&&!std::is_same_v<T,char32_t>;
// The 64-bit hash of String::hash() over any bytes.
std::uint64_t hash_bytes(const char* data,std::size_t size) noexcept;
// The 64-bit hash of String::table_hash() over any bytes, keyed per process. Valid from the first dynamic initializer on
// (the secrets are drawn by a priority-101 constructor; debug builds assert it).
std::uint64_t table_hash_bytes(const char* data,std::size_t size) noexcept;
// For tests only (nothing else calls them, so release links drop them): word index of table_hash()'s keys and of hash()'s
// keys, 0 the seed and 1 to 4 the secrets.
std::uint64_t table_key_word(int index) noexcept;
std::uint64_t stable_key_word(int index) noexcept;
// Byte equality without a C runtime call, for the hot comparisons of short strings (array find and equality, dictionary
// lookups, substring search): the Windows C runtime's memcmp, called once per comparison, dominated them. Up to 16 bytes
// the n bytes are compared with two overlapping loads of 2, 4 or 8 bytes; longer runs go to an out-of-line loop (16-byte
// SSE2 blocks, the last one overlapping) so callers' loops keep few live registers. Every load stays inside both ranges.
// same_long_bytes needs n > 16.
bool same_long_bytes(const char* a,const char* b,std::size_t n) noexcept;
template<class T>
inline T load_bytes(const char* p) noexcept
{
T value;
__builtin_memcpy(&value,p,sizeof value);
return value;
}
inline bool same_bytes(const char* a,const char* b,std::size_t n) noexcept
{
if(n>16)return same_long_bytes(a,b,n);
if(n>=8)return ((load_bytes<std::uint64_t>(a)^load_bytes<std::uint64_t>(b))|(load_bytes<std::uint64_t>(a+n-8)^load_bytes<std::uint64_t>(b+n-8)))==0;
if(n>=4)return ((load_bytes<std::uint32_t>(a)^load_bytes<std::uint32_t>(b))|(load_bytes<std::uint32_t>(a+n-4)^load_bytes<std::uint32_t>(b+n-4)))==0;
if(n>=2)return ((load_bytes<std::uint16_t>(a)^load_bytes<std::uint16_t>(b))|(load_bytes<std::uint16_t>(a+n-2)^load_bytes<std::uint16_t>(b+n-2)))==0;
return n==0||*a==*b;
}
// a and b hold the same bytes (String's ==).
inline bool same_text(std::string_view a,std::string_view b) noexcept
{
return a.size()==b.size()&&same_bytes(a.data(),b.data(),a.size());
}
// String's packed fields. size: bit 31 is set while the bytes live on the heap. meta: meta_unknown, or the character count
// plus meta_ascii when the text is known to be ASCII. Without meta_ascii the text is non-ASCII when the count differs from
// the byte length, and not yet known otherwise (every character one byte: ASCII or invalid bytes; is_ascii() settles it).
// String::max_byte_length keeps a known meta from ever equalling meta_unknown.
inline constexpr std::uint32_t heap_bit=0x80000000u;
inline constexpr std::uint32_t meta_unknown=0xFFFFFFFFu;
inline constexpr std::uint32_t meta_ascii=0x80000000u;
inline constexpr std::uint32_t meta_length=0x7FFFFFFFu;
// Atomic access to the caches const members fill (clang builtins: libc++ 18 has no std::atomic_ref).
inline std::uint32_t load_relaxed(const std::uint32_t& value) noexcept
{
return __atomic_load_n(&value,__ATOMIC_RELAXED);
}
inline void store_relaxed(std::uint32_t& value,std::uint32_t written) noexcept
{
__atomic_store_n(&value,written,__ATOMIC_RELAXED);
}
}

class String {
public:
static constexpr int inline_capacity=23;
static constexpr int index_step=32;
static constexpr int max_byte_length=INT_MAX-1;

String() noexcept
{
init_empty();
}
String(std::string_view bytes)
{
init(bytes.data(),bytes.size(),meta_unknown);
}
String(const char* text):String(std::string_view(text)) {}
// Copies read other's caches atomically: other may be read (and its caches filled) by other threads at the same time.
// The inline buffer and the sizes are never written by const members. Inline text is copied here, heap text out of line.
String(const String& other)
{
// The size is read once: it decides the path and is the copy's size (reading it again after the test costs the script copy a load).
const std::uint32_t size=other.rep_.size;
if((size&heap_bit)!=0){
copy_heap(other);
return;
}
rep_.storage=other.rep_.storage;
rep_.size=size;
rep_.meta=other.load_meta();
}
String(String&& other) noexcept:rep_(other.rep_)
{
other.init_empty();
}
~String()
{
if(is_heap())release();
}
String& operator=(const String& other);
String& operator=(String&& other) noexcept
{
if(this!=&other){
if(is_heap())release();
rep_=other.rep_;
other.init_empty();
}
return *this;
}
// Replaces the bytes; bytes may view this string's own text.
String& operator=(std::string_view bytes);
String& operator=(const char* text)
{
return *this=std::string_view(text);
}

static String from_code_point(char32_t code_point);
static String from_bool(bool value);
// Shortest text that reads back as the same double (std::to_chars), so integers have no ".0"; "inf", "-inf", "nan",
// and negative zero is "0". JavaScript's notation thresholds: fixed notation for 1e-7 <= |value| < 1e21 ("100000",
// "0.0001"), otherwise scientific with the shortest exponent ("1e+21", "1e-7"). Unlike JavaScript, which pads the shortest
// digits with zeros, integers of 2^53 and above print their exact digits in fixed notation (123456789012345680000.0 is
// "123456789012345683968").
static String from_number(double value);
// The same for float, with the shortest text that reads back as the same float (0.1f is "0.1").
static String from_number(float value);
template<string_detail::integer_value T>
static String from_number(T value)
{
String text;
text.append(value);
return text;
}
// A string written in place, for I/O that reads straight into a string's storage without a buffer and a copy: fill(data) writes
// at most capacity bytes at data and returns how many it wrote (more is a bug, asserted in debug builds). Text of up to
// inline_capacity bytes needs no allocation. A
// capacity above max_byte_length throws std::bad_alloc; when fill throws, the string is freed.
template<class Fill>
static String filled(std::size_t capacity,Fill&& fill)
{
String text;
char* data=text.raw_reserve(capacity);
const std::size_t size=fill(data);
assert(size<=capacity&&"String::filled: fill wrote more than the capacity");
text.raw_set_size(static_cast<std::uint32_t>(size),meta_unknown);
return text;
}

const char* data() const noexcept
{
return is_heap()?rep_.storage.heap.ptr:rep_.storage.buffer;
}
const char* c_str() const noexcept
{
return data();
}
std::string_view bytes() const noexcept
{
return {data(),size_bytes()};
}
// True when the text lives inside the object (no heap block): copying such a string never allocates, so it cannot throw.
bool is_inline() const noexcept
{
return !is_heap();
}
int byte_length() const noexcept
{
return static_cast<int>(size_bytes());
}
bool empty() const noexcept
{
return size_bytes()==0;
}
// Number of characters (cached).
int length() const noexcept
{
const std::uint32_t meta=load_meta();
if(meta!=meta_unknown)return static_cast<int>(meta&meta_length);
return scan();
}
// True when every byte is below 0x80 (cached with length()).
bool is_ascii() const noexcept
{
std::uint32_t meta=load_meta();
if(meta==meta_unknown){
scan();
meta=load_meta();
}
if((meta&meta_ascii)!=0)return true;
if((meta&meta_length)!=size_bytes())return false;
return settle_ascii();
}
// The stable script hash (see the contract), computed on each call.
std::uint64_t hash() const noexcept
{
return string_detail::hash_bytes(data(),size_bytes());
}
// The per-process keyed hash for hash tables (see the contract), cached for heap text.
std::uint64_t table_hash() const noexcept
{
if(!is_heap())return string_detail::table_hash_bytes(rep_.storage.buffer,rep_.size);
if(__atomic_load_n(&rep_.storage.heap.hash_known,__ATOMIC_ACQUIRE)!=0)return __atomic_load_n(&rep_.storage.heap.hash,__ATOMIC_RELAXED);
return cache_hash();
}
// Bytes the string can hold without reallocating.
int capacity() const noexcept
{
return static_cast<int>(capacity_bytes());
}
void reserve(int bytes);
// Empties the string, keeping its storage.
void clear() noexcept;

std::expected<String,StringError> char_at(int index) const;
std::expected<char32_t,StringError> code_at(int index) const
{
const int length=this->length();
if(static_cast<std::uint32_t>(length)!=size_bytes())return multibyte_code_at(index);
// Every character is one byte: an ASCII byte is its own code point, any other is an invalid byte (U+FFFD).
if(static_cast<unsigned>(index)>=static_cast<unsigned>(length))return std::unexpected(StringError::index_out_of_range);
const auto byte=static_cast<unsigned char>(data()[index]);
return byte<0x80?char32_t{byte}:char32_t{0xFFFD};
}
// The code at index when the cached metadata already shows the text is ASCII (so indices are byte offsets); -1 otherwise
// (code_at then has the answer). Never scans, so callers can use it as a leaf fast path.
int known_ascii_code_at(int index) const noexcept
{
const std::uint32_t meta=load_meta();
if(meta==meta_unknown||(meta&meta_ascii)==0||static_cast<std::uint32_t>(index)>=size_bytes())return -1;
return static_cast<unsigned char>(data()[index]);
}
std::expected<std::uint8_t,StringError> byte_at(int index) const;

// Character index of the first match at or after start; an empty needle matches at start.
std::expected<int,StringError> find(std::string_view needle,int start=0) const
{
const int length=this->length();
if(start<0||start>length)return std::unexpected(StringError::index_out_of_range);
if(static_cast<std::uint32_t>(length)==size_bytes())return find_simple(needle.data(),needle.size(),start);
return find_multibyte(needle,start);
}
// Character index of the last match beginning at or before start (-1: the end).
std::expected<int,StringError> rfind(std::string_view needle,int start=-1) const;
bool contains(std::string_view needle) const noexcept;
bool starts_with(std::string_view prefix) const noexcept;
bool ends_with(std::string_view suffix) const noexcept;
// Non-overlapping matches; 0 for an empty needle.
int count(std::string_view needle) const noexcept;

std::expected<String,StringError> substr(int start,int count=-1) const;
std::expected<String,StringError> left(int count) const;
std::expected<String,StringError> right(int count) const;
// Parts between matches of delimiter; an empty delimiter splits into characters. keep_empty=false drops empty parts.
std::vector<String> split(std::string_view delimiter,bool keep_empty=true) const;
static String join(std::span<const String> parts,std::string_view separator);

// Replaces every match (only the first when all is false); an empty old_text changes nothing.
String replace(std::string_view old_text,std::string_view new_text,bool all=true) const;
// Removes leading and trailing White_Space characters.
String trim() const;
String trim_left() const;
String trim_right() const;
// Simple (one-to-one) case mapping, locale-independent.
String lower() const;
String upper() const;
// locale "tr" or "az" (any case, optionally followed by '-' or '_' and a region) applies the Turkish dotted/dotless I rules,
// including SpecialCasing's After_I rule (I, then marks of combining class other than 0 and 230, then U+0307 lowers to i
// with those marks, dropping the U+0307); others are invariant.
String lower(std::string_view locale) const;
String upper(std::string_view locale) const;
std::expected<String,StringError> repeat(int count) const;
// Characters in reverse order (bytes of each character kept in order).
String reverse() const;
std::expected<String,StringError> insert(int index,std::string_view text) const;
std::expected<String,StringError> erase(int start,int count=-1) const;
// Normalization Form C of each run of well-formed text; invalid bytes are kept and nothing composes across them.
String normalize() const;

// The whole string as a decimal number: [+-] digits [. digits] [e [+-] digits] (digits on at least one side of the point),
// with no surrounding white space. "inf" and "nan" are not numbers (input validation). Correctly rounded; a decimal too
// large for a double gives +-inf.
std::expected<double,StringError> to_number() const;
// An integer text exactly (saturating at the int64 limits); any other number truncated toward zero and saturated.
std::expected<std::int64_t,StringError> to_int() const;
bool is_number() const noexcept;
// True when non-empty and every byte is an ASCII digit 0-9.
bool is_digits() const noexcept;

String& append(const String& other);
// bytes may view this string's own text.
String& append(std::string_view bytes);
String& append(const char* text)
{
return append(std::string_view(text));
}
// Appends the UTF-8 encoding; surrogates and values above U+10FFFF append U+FFFD.
String& append(char32_t code_point);
String& append(bool value);
String& append(double value);
String& append(float value);
template<string_detail::integer_value T>
String& append(T value)
{
if constexpr(std::is_signed_v<T>){
return append_signed(static_cast<long long>(value));
}else{
return append_unsigned(static_cast<unsigned long long>(value));
}
}
template<typename T>
String& operator+=(const T& value)
{
return append(value);
}

friend bool operator==(const String& a,const String& b) noexcept
{
return a.bytes()==b.bytes();
}
friend bool operator==(const String& a,std::string_view b) noexcept
{
return a.bytes()==b;
}
friend bool operator==(const String& a,const char* b) noexcept
{
return a.bytes()==std::string_view(b);
}
friend std::strong_ordering operator<=>(const String& a,const String& b) noexcept
{
return a.bytes()<=>b.bytes();
}
friend std::strong_ordering operator<=>(const String& a,std::string_view b) noexcept
{
return a.bytes()<=>b;
}
friend std::strong_ordering operator<=>(const String& a,const char* b) noexcept
{
return a.bytes()<=>std::string_view(b);
}
friend String operator+(const String& a,const String& b)
{
return concat(a.bytes(),a.load_meta(),b.bytes(),b.load_meta());
}
friend String operator+(String&& a,const String& b)
{
a.append(b);
return std::move(a);
}
friend String operator+(const String& a,std::string_view b)
{
const std::uint32_t a_meta=a.load_meta();
return concat(a.bytes(),a_meta,b,a_meta==meta_unknown?meta_unknown:view_meta(b));
}
friend String operator+(String&& a,std::string_view b)
{
a.append(b);
return std::move(a);
}
friend String operator+(const String& a,const char* b)
{
return a+std::string_view(b);
}
friend String operator+(String&& a,const char* b)
{
a.append(std::string_view(b));
return std::move(a);
}
friend String operator+(std::string_view a,const String& b)
{
const std::uint32_t b_meta=b.load_meta();
return concat(a,b_meta==meta_unknown?meta_unknown:view_meta(a),b.bytes(),b_meta);
}
friend String operator+(const char* a,const String& b)
{
return std::string_view(a)+b;
}

private:
friend class StringWriter;
static constexpr std::uint32_t heap_bit=string_detail::heap_bit;
static constexpr std::uint32_t meta_unknown=string_detail::meta_unknown;
static constexpr std::uint32_t meta_ascii=string_detail::meta_ascii;
static constexpr std::uint32_t meta_length=string_detail::meta_length;
struct Heap {
char* ptr;
std::uint32_t capacity;
std::uint32_t hash_known;
// The cached table_hash(). May sit at a 4-mod-8 address (see the alignment contract): only relaxed loads and stores, never acquire, release,
// compare-and-swap or any read-modify-write operation.
std::uint64_t hash;
};
union Storage {
char buffer[inline_capacity+1];
Heap heap;
};
struct Rep {
Storage storage;
std::uint32_t size;
std::uint32_t meta;
};
static_assert(sizeof(Rep)==32);
// The layout (and the 4-mod-8 placement argument) assumes 64-bit pointers.
static_assert(sizeof(void*)==8);

String(const char* bytes,std::size_t size,std::uint32_t meta)
{
init(bytes,size,meta);
}
// One ASCII byte (below 0x80), inline, its count and ASCII flag known: the script s[i] on ASCII text (StringBindingAccess), with no
// copy loop and no allocation.
struct OneAsciiByte {
char value;
};
explicit String(OneAsciiByte byte) noexcept
{
rep_.storage.buffer[0]=byte.value;
rep_.storage.buffer[1]='\0';
rep_.size=1;
rep_.meta=1|meta_ascii;
}
// A string whose fields the caller writes next, every one of them (script_add): nothing is stored twice.
struct Unset {};
explicit String(Unset) noexcept {}
bool is_heap() const noexcept
{
return (rep_.size&heap_bit)!=0;
}
std::uint32_t size_bytes() const noexcept
{
return rep_.size&~heap_bit;
}
std::uint32_t capacity_bytes() const noexcept
{
return is_heap()?rep_.storage.heap.capacity:static_cast<std::uint32_t>(inline_capacity);
}
char* mutable_data() noexcept
{
return is_heap()?rep_.storage.heap.ptr:rep_.storage.buffer;
}
std::uint32_t load_meta() const noexcept
{
return string_detail::load_relaxed(rep_.meta);
}
void init_empty() noexcept
{
rep_.storage.buffer[0]='\0';
rep_.size=0;
rep_.meta=meta_ascii;
}
void init(const char* bytes,std::size_t size,std::uint32_t meta);
// The copy constructor for heap text (other.is_heap()). It and release() start on a 64-byte boundary, as do the binding's
// copy and destroy entry points (string_binding.cpp): with full LTO their default 16-byte placement shifts with the size of
// unrelated code, and on the measured machines that alone moved copying a heap string from a literal by up to 8 %.
[[gnu::aligned(64)]] void copy_heap(const String& other);
// release() and scan() are the slow paths of ~String() and length(), and stay out of line (Task 5c fix round 3): Win64 code gets no
// shrink-wrapping, so a slow path inlined into a leaf gives the leaf's fast path its stack frame (pushes, sub/add rsp) on every call.
// The script destructor and length() measured 3-5 % slower than the add-on's on the EPYC 9V45 that way.
[[gnu::noinline,gnu::aligned(64)]] void release() noexcept;
void free_index() noexcept;
[[gnu::noinline]] int scan() const noexcept;
bool settle_ascii() const noexcept;
// Computes table_hash() of heap text and caches it.
std::uint64_t cache_hash() const noexcept;
void extend_index();
// Carries other's cached table hash (heap text of equal bytes) over to this heap text, or marks it unknown.
void copy_hash(const String& other) noexcept;
static std::uint32_t view_meta(std::string_view bytes) noexcept;
void reallocate(std::uint32_t capacity);
char* raw_reserve(std::size_t capacity);
void raw_set_size(std::uint32_t size,std::uint32_t meta) noexcept;
String& append_raw(const char* bytes,std::size_t size,std::uint32_t meta);
// append_raw's paths for a full block and for text with a sparse index, kept out of line so its common path stays small.
[[gnu::noinline]] String& append_grown(const char* bytes,std::size_t size,std::uint32_t meta);
// Never throws: an index it cannot extend is dropped (rebuilt lazily), so a published index always covers the whole text.
[[gnu::noinline]] void update_index(bool joins) noexcept;
String& append_signed(long long value);
String& append_unsigned(unsigned long long value);
static String concat(std::string_view a,std::uint32_t a_meta,std::string_view b,std::uint32_t b_meta);
static String with_size(std::size_t size);
bool is_simple() const noexcept
{
return static_cast<std::uint32_t>(length())==size_bytes();
}
const std::uint32_t* index() const;
// code_at for text with multibyte characters (length() differs from the byte length).
std::expected<char32_t,StringError> multibyte_code_at(int index) const;
// find for text with multibyte characters (start already checked).
std::expected<int,StringError> find_multibyte(std::string_view needle,int start) const;
// find() for text with one byte per character (length() == byte_length(), so indices are byte offsets and every
// offset is a character boundary): the first match of needle[0, size) at or after start, which must be in
// [0, byte_length()] (both asserted in debug builds). Never allocates. Private: find() and the script binding (through
// StringBindingAccess, which checks both preconditions first) are its only callers.
int find_simple(const char* needle,std::size_t size,int start) const noexcept;
friend struct StringBindingAccess;
// The script byte_at's out-of-range path (defined in string_binding.cpp, whose StringBindingAccess holds the leaf): raises the
// exception and returns a byte. Not noexcept, so the leaf can leave by a musttail call (nothing in it can throw).
const std::uint8_t& script_byte_out_of_range(int index) const;
// The script opAdd (StringBindingAccess registers it). The engine returns a string in memory: it passes the address of the result's
// uninitialized storage first, as the ABI passes a hidden return pointer, so the binding takes it as out and returns it. Two inline
// operands with an inline result take a frameless leaf that builds the result in out; everything else takes a forced tail call to
// script_add_any (defined in string_binding.cpp: the general concatenation, which may allocate, inside the script exception
// boundary). A String return would not do: clang makes no musttail call that returns a non-trivial type, and without it Win64 code
// saves registers for the slow path on every call. Neither is noexcept, so the leaf can leave by a musttail call; it cannot throw.
static String* script_add(String* out,const String& a,const String& b);
static String* script_add_any(String* out,const String& a,const String& b);
std::uint32_t char_to_byte(std::uint32_t index) const;
std::uint32_t byte_to_char(std::uint32_t offset) const;
// The meta of a part of this string with length characters (known whenever this string's count is known).
std::uint32_t part_meta(std::uint32_t length) const noexcept
{
const std::uint32_t meta=load_meta();
return meta==meta_unknown?meta_unknown:length|(meta&meta_ascii);
}
String bytes_between(std::uint32_t begin,std::uint32_t end,std::uint32_t meta) const;
String trimmed(bool left,bool right) const;

mutable Rep rep_;
};
}

template<>
struct std::hash<zodiac::String> {
std::size_t operator()(const zodiac::String& text) const noexcept
{
return static_cast<std::size_t>(text.table_hash());
}
};
