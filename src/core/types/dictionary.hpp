// zodiac::Dictionary: Zodiac's script `dictionary` (spec 8.3), a hash table from string keys to values of any script type
// that keeps its keys in insertion order. zodiac::DictionaryValue is one value, registered as the d[key] proxy
// `dictionary_value`. Written from scratch; values are stored like the sample add-on's (numbers inline, objects counted).
// Contract:
// - register_dictionary(engine) registers `dictionary_value` (asOBJ_VALUE|asOBJ_ASHANDLE|asOBJ_GC) and `dictionary`
//   (asOBJ_REF|asOBJ_GC) with native calling conventions. Zodiac's array and string must be registered first (keys()
//   returns array<string>). Script API (style C, none of the add-on's camelCase names): factories dictionary() and the list
//   literal {{"key", value}, ...} (a repeated key keeps its first position and the last value), opAssign (copies),
//   set(key, value) (also int64 and double overloads), bool get(key, out value) (also int64 and double), d[key] (inserts an
//   empty value when the key is missing; on a const dictionary a missing key throws "key \"<key>\" not found"),
//   bool exists(key), bool remove(key), array<string>@ keys() (insertion order), int length(), bool empty(), clear(),
//   string serialize(), bool deserialize(text). dictionary_value: opAssign and opHndlAssign from any value or another
//   dictionary_value, opConv/opCast to any type, int64 and double opConv.
// - Order (for inventories and menus): positions are 0-based in the order keys(), iteration and serialize() follow, which a
//   new key joins at the back. bool move_by(key, int delta) moves key delta places (negative: towards the front) and
//   returns false, changing nothing, when key is missing, delta is 0 or the move would pass either end; it costs O(|delta|),
//   so a move by one is O(1). bool move(key, int index) moves key so that it ends at index and returns false, changing
//   nothing, when key is missing or index is outside [0, length()-1]; a move to the index the key already has changes
//   nothing and returns true. int index_of(key)
//   (-1 when missing) walks from the key towards both ends at once and stops at the first: min(i, length()-1-i) steps for a
//   key at i, so the ends of the order are O(1). string key_at(int index) walks from the nearer end, the same
//   min(index, length()-1-index) steps, and throws "index out of range" outside [0, length()-1]. move costs index_of's walk
//   plus the shorter of the walks from the key and from the nearer end to index. None of them runs script code or
//   allocates, and a remove or re-insert keeps the order of the other keys.
// - Values: every integer type (and enums) is stored as int64 (uint64 bit for bit), float and double as double, bool as
//   bool; objects as their own copy (a counted reference for handles, null handles included). An empty value (a key made
//   by d[key]) has type id 0. Reading converts between all numeric types and bool (integers wrap, doubles truncate and
//   saturate, NaN reads as 0); an object reads into the same type (by its assignment) or, through RefCastObject, into a
//   compatible handle; nothing else converts: get() returns false and writes nothing (AngelScript itself resets an object
//   &out argument), a proxy conversion leaves the target's default. Conversions are explicit (int(d[key]),
//   string(d[key]), cast<T>(d[key])), as in the add-on. Storing a dictionary_value stores its content.
// - Table: entries live in segments that never move (the 16 entries of segment 0, then doubling), so a reference returned
//   by d[key] stays valid memory for the dictionary's lifetime; removed entries go to a free list for reuse. Keys are
//   chained in insertion order. An open-addressing index (linear probing, power-of-two size, at most half full,
//   backward-shift removal, no tombstones) stores each key's 64-bit String::table_hash() next to its entry number, so lookups
//   hash the key once (heap keys cache it), compare hashes before bytes and never allocate. A key's home slot is the low bits
//   of that hash. table_hash() is keyed by per-process secrets from the operating system's entropy source (string.hpp), so
//   keys that untrusted input (players, the network, save files) picks to collide in the script-visible String::hash(),
//   fully or in some bits, do not degrade the table; only slot positions vary between runs, never the iteration order or
//   serialize()'s text. Insert, lookup and remove are amortized O(1); iteration is in insertion order. clear() keeps the
//   memory. At most 2^30 entries ("out of memory" beyond).
// - Script code can run inside set, remove, clear, assignment, deserialize and the proxy's assignments (a value's copy,
//   opAssign or destructor). Every such operation makes new values before it changes the table and releases old values only
//   after the table is consistent again; get() holds a reference to the value it copies from; assignment copies from a
//   snapshot holding references. So callbacks may use the dictionary freely: they see a consistent table, and what they do
//   is kept, except that assignment and deserialize replace the whole content when they commit. A value written through a
//   stale d[key] reference whose entry was removed meanwhile is released when the entry is reused, cleared or destroyed.
// - serialize(): UTF-8 text, first line "zodiac-dictionary 1", then one line per entry in insertion order:
//   <tag> "<key>" [<payload>]. Tags and payloads: none (empty value, no payload), bool true|false, int64 decimal,
//   double shortest round-trip text or inf, -inf, nan, string "<text>", dictionary <count> followed by count entry lines
//   (or null), array<T> <count> followed by count element lines holding only a payload (or null), for T bool, int8, int16,
//   int, int64, uint8, uint16, uint, uint64, float, double, string, dictionary@ (null or a count with entry lines) and
//   dictionary. Quoted text escapes \" \\ \n \r \t and other bytes below 0x20 and 0x7F as \xHH; bytes from 0x80 are kept.
//   Every line ends with "\n". Nesting is limited to max_depth levels. Any other value throws
//   "cannot serialize \"<key>\": unsupported type <type>" (the innermost key; <type> as scripts write it, e.g. array<Item@>), a
//   dictionary reached again inside itself "cannot serialize \"<key>\": the dictionary contains itself", deeper nesting
//   "cannot serialize \"<key>\": nested too deeply". Keys in messages go through one_line and are cut to 64 characters
//   followed by "...".
// - deserialize(text) accepts exactly that format (lines may also end with "\r\n", the last line needs no line end) and
//   replaces the content with it, nested dictionaries and arrays as handles; any malformed input (unknown tag or escape,
//   bad number or range, duplicate key at one level, wrong count, trailing text, deeper nesting) returns false and leaves
//   the dictionary unchanged.
// - Errors raise script exceptions on the active context (std::bad_alloc as "out of memory"); no C++ exception reaches
//   AngelScript. A failed copy of an object value raises "cannot copy a value of type <type>" unless the copy already
//   raised its own exception, and changes nothing.
// - Reference counting is atomic; any number of threads may read one dictionary that nobody changes.
#pragma once
#include <angelscript.h>
#include <core/types/string.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zodiac {
struct DictionaryCache;
class ScriptArray;

class DictionaryValue {
public:
DictionaryValue() noexcept {payload_.integer=0;}
DictionaryValue(DictionaryValue&& other) noexcept:payload_(other.payload_),type_(other.type_),type_id_(other.type_id_)
{
other.forget();
}
DictionaryValue& operator=(DictionaryValue&& other) noexcept;
DictionaryValue(const DictionaryValue&)=delete;
DictionaryValue& operator=(const DictionaryValue&)=delete;
~DictionaryValue()
{
if(type_)release();
}
void swap(DictionaryValue& other) noexcept;

// 0 for an empty value, asTYPEID_INT64, asTYPEID_DOUBLE, asTYPEID_BOOL or an object type id (with its handle flags).
int type_id() const noexcept {return type_id_;}
bool empty() const noexcept {return type_id_==0;}
// The object, or nullptr for null handles and non-objects; its type, or nullptr for non-objects.
void* object() const noexcept {return type_?payload_.object:nullptr;}
asITypeInfo* object_type() const noexcept {return type_;}
std::int64_t integer() const noexcept {return payload_.integer;}
double number() const noexcept {return payload_.number;}
bool boolean() const noexcept {return payload_.integer!=0;}

void set_integer(std::int64_t value) noexcept;
void set_number(double value) noexcept;
void set_boolean(bool value) noexcept;
// Takes over one reference to object (counted by the caller) as a value of type_id.
void adopt(void* object,asITypeInfo* type,int type_id) noexcept;
// Releases the content; the value is empty afterwards.
void reset() noexcept;
// Stores a copy of the script value at ref (type_id as AngelScript passes ?&in). Returns false after raising a script
// exception (or when the copy raised one), leaving the value unchanged. Throws std::bad_alloc.
bool assign(const void* ref,int type_id,const DictionaryCache& cache);
bool assign(const DictionaryValue& other,const DictionaryCache& cache);
// Writes the value to out as type_id (?&out) with the conversions of the contract; false when it does not convert.
// Throws std::bad_alloc.
bool get(void* out,int type_id,const DictionaryCache& cache) const;

void enum_references(asIScriptEngine* engine) noexcept;
void release_references(asIScriptEngine* engine) noexcept;
private:
friend class Dictionary;
union Payload {
std::int64_t integer;
double number;
void* object;
};
void forget() noexcept
{
payload_.integer=0;
type_=nullptr;
type_id_=0;
}
void release() noexcept;
// Build into this empty value; false after a raised failure. make: the script value at ref; make_copy: another value;
// make_object_copy: a copy of an object (kept alive meanwhile); snapshot: like make_copy without running script code,
// holding a counted script object stored by value by reference for a later make_object_copy.
bool make(const void* ref,int type_id,const DictionaryCache& cache);
bool make_copy(const DictionaryValue& other,const DictionaryCache& cache);
bool make_object_copy(void* object,asITypeInfo* type,int type_id,const DictionaryCache& cache);
bool snapshot(const DictionaryValue& other,const DictionaryCache& cache);
// True for an object stored by value whose type is reference counted (copied after a snapshot).
bool counted_object() const noexcept;

Payload payload_;
// The type of an object value (also of a null handle); nullptr for primitives and empty values.
asITypeInfo* type_=nullptr;
int type_id_=0;
};

// 64-byte aligned (C++17 aligned new) so the fields a lookup reads, which come first, share one cache line.
class alignas(64) Dictionary {
public:
static constexpr int max_depth=100;

// A new dictionary known to the garbage collector. Throws std::bad_alloc.
static Dictionary* create(DictionaryCache& cache);

void add_ref() const noexcept;
void release() const noexcept;

int length() const noexcept {return static_cast<int>(live_);}
bool empty() const noexcept {return live_==0;}
bool exists(const String& key) const noexcept;
const DictionaryValue* find(const String& key) const noexcept;
DictionaryValue* find(const String& key) noexcept;
// The value of key, inserting an empty one when missing. Throws std::bad_alloc.
DictionaryValue& at(const String& key);
// set and get as in the contract; set returns false after a raised failure. Throw std::bad_alloc.
bool set(const String& key,const void* ref,int type_id);
void set(const String& key,std::int64_t value);
void set(const String& key,double value);
// Stores value under key; value receives what the entry held before (released by the caller after the table is
// consistent).
void put(const String& key,DictionaryValue& value);
bool get(const String& key,void* out,int type_id) const;
bool remove(const String& key) noexcept;
void clear() noexcept;
// The order operations of the contract (positions 0-based in the insertion-order chain).
bool move_by(const String& key,int delta) noexcept;
bool move(const String& key,int index) noexcept;
int index_of(const String& key) const noexcept;
// The key at index, or nullptr when index is outside [0, length()-1].
const String* key_at(int index) const noexcept;
// The keys in insertion order; nullptr after a raised failure.
ScriptArray* keys() const noexcept;
// Replaces the content with copies of other's; false after a raised failure (the content is then unchanged).
bool assign(const Dictionary& other);
// Throws std::runtime_error with the contract's message for values it cannot write, and std::bad_alloc.
String serialize() const;
bool deserialize(std::string_view text);

// Garbage-collector behaviours.
int ref_count() const noexcept;
void set_gc_flag() noexcept;
bool gc_flag() const noexcept;
void enum_references(asIScriptEngine* engine) noexcept;
void release_all_references(asIScriptEngine* engine) noexcept;

Dictionary(const Dictionary&)=delete;
Dictionary& operator=(const Dictionary&)=delete;
// On Zodiac's heap (core/types/heap.hpp), at the class's 64-byte alignment.
static void* operator new(std::size_t size);
static void operator delete(void* block) noexcept;
private:
friend class DictionaryWriter;
friend class DictionaryReader;
// Tests read the index through it (probe lengths); defined only by them.
friend struct DictionaryTestAccess;
struct Entry;
struct Slot {
// The key's String::table_hash().
std::uint64_t hash;
// Entry number plus one; 0 marks an empty slot.
std::uint32_t entry;
std::uint32_t unused;
};
static constexpr int segment_count=27;

// The home slot of a key: the low bits of its table hash. That hash is keyed per process (String::table_hash()), so no
// further mapping is needed for keys chosen to collide in String::hash(), which scripts can see, to spread. Iteration and
// serialize() follow the insertion-order links, so both stay deterministic.
std::uint32_t home(std::uint64_t hash) const noexcept
{
return static_cast<std::uint32_t>(hash)&mask_;
}

explicit Dictionary(DictionaryCache& cache) noexcept;
~Dictionary();
Entry& entry(std::uint32_t index) const noexcept;
// The index position of key, or no_entry.
std::uint32_t find_position(const String& key,std::uint64_t hash) const noexcept;
// Makes room for count more entries (index and storage); throws std::bad_alloc and then changes nothing.
void reserve(std::uint32_t count);
void rehash(std::uint32_t size);
void add_segment();
// Inserts a key known to be missing after reserve(1); value receives a stray value found in the reused entry.
std::uint32_t insert_new(String&& key,std::uint64_t hash,DictionaryValue& value) noexcept;
void erase_position(std::uint32_t position) noexcept;
void link_last(std::uint32_t index) noexcept;
// Link an unlinked entry just before or just after the live entry at.
void link_before(std::uint32_t index,std::uint32_t at) noexcept;
void link_after(std::uint32_t index,std::uint32_t at) noexcept;
void unlink(std::uint32_t index) noexcept;
// The position of a live entry in the order chain: min(position, live_-1-position) steps, walking towards both ends at once.
std::uint32_t position_of(std::uint32_t index) const noexcept;
// The live entry at position (< live_): min(position, live_-1-position) steps from the nearer end.
std::uint32_t entry_at(std::uint32_t position) const noexcept;
// The live entry steps places from index towards the front (backward) or the back, or no entry when the chain ends first.
std::uint32_t walk(std::uint32_t index,std::uint32_t steps,bool backward) const noexcept;
void free_entry(std::uint32_t index) noexcept;
// Moves every entry of source (a private temporary) into this dictionary, replacing its content.
void commit(Dictionary& source);

// What a lookup reads comes first: the index, its mask, the cache get() converts with and the segments (with the object
// on a 64-byte boundary, the first line holds these and segments 0-4, the first 496 entries).
Slot* index_=nullptr;
DictionaryCache* cache_;
std::uint32_t mask_=0;
std::uint32_t live_=0;
Entry* segments_[segment_count]={};
mutable int ref_count_=1;
mutable bool gc_flag_=false;
// Entries [0, used_) have been handed out; the free list holds the removed ones among them.
std::uint32_t used_=0;
std::uint32_t capacity_=0;
std::uint32_t free_count_=0;
std::uint32_t free_;
std::uint32_t first_;
std::uint32_t last_;
std::uint32_t clearing_=0;
};

// Registers dictionary_value and dictionary; throws std::runtime_error naming the declaration the engine refuses.
void register_dictionary(asIScriptEngine* engine);
}
