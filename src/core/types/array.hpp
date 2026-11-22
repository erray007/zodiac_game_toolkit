/*
   AngelCode Scripting Library
   Copyright (c) 2003-2026 Andreas Jonsson

   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any
   damages arising from the use of this software.

   Permission is granted to anyone to use this software for any
   purpose, including commercial applications, and to alter it and
   redistribute it freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you
      must not claim that you wrote the original software. If you use
      this software in a product, an acknowledgment in the product
      documentation would be appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and
      must not be misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
      distribution.

   The original version of this library can be located at:
   http://www.angelcode.com/angelscript/

   Andreas Jonsson
   andreas@angelcode.com
*/
// ALTERED SOURCE: zodiac::ScriptArray is Zodiac's script array<T> (spec 8.2), forked from AngelScript 2.39.0's scriptarray
// add-on (CScriptArray). Changed for Zodiac: style-C script names, int indices and counts with Zodiac's range rules and error
// texts, elements stored in one block next to the length (no separate header allocation), geometric growth, native sort/find/
// equality for primitive and string elements, callback sorts that work on a copy, and no generic calling convention.
// Contract:
// - register_array(engine) registers the template array<T> (asOBJ_REF|asOBJ_GC|asOBJ_TEMPLATE, the default array type, so
//   T[] works) with native calling conventions, keeping the add-on's template callback, type-info cache (opCmp/opEquals
//   found once per instance), garbage-collector behaviours (reference count, flag, enumerate and release references; an
//   instance is only garbage collected when its elements can form cycles) and copy semantics (opAssign copies elements:
//   values by their opAssign, handles by reference). Script API: factories array<T>(), array<T>(int length) explicit,
//   array<T>(int length, const T &in value), initializer lists {a, b, c}; T &opIndex(int) (and const), foreach with value and
//   int index, opAssign, opEquals, length, empty, resize, reserve, clear, insert_at(index, value), insert_at(index, array),
//   insert_last, remove_at, remove_last, remove_range(start, count=-1), find(value, start=0), find_by_ref(handle, start=0),
//   contains, sort(), sort(less), sort_descending(), reverse(), slice(start, count=-1), and the funcdef
//   bool array<T>::less(const T &in a, const T &in b). None of the add-on's camelCase names is registered. The string
//   binding adds join (string join(const T &in separator), usable on array<string> only).
// - Range rules (spec 8.1): an index or start outside [0, length()] (outside [0, length()) for element access and
//   remove_at) throws "index out of range"; counts past the end are clamped; a negative count other than the -1 "to the end"
//   default throws "negative count"; a failed allocation (or more than INT_MAX elements) throws "out of memory".
// - Native fast paths: for bool, int8..int64, uint8..uint64, float, double, enums (as their underlying type, signed or
//   unsigned) and Zodiac's string, sort(), sort_descending(), find, contains and opEquals compare in C++ without calling
//   back into the engine. Native sorts are not stable, which is only visible for values that compare equal but differ in
//   bits (-0.0 and 0.0, NaN payloads); NaN sorts last in both orders and equals nothing. Other element types use their
//   opCmp/opEquals (opCmp also serves equality) through a nested call on the active context; find, contains, opEquals on
//   non-empty arrays and sort of a type without them throw "type \"<name>\" has no opCmp method" or
//   "type \"<name>\" has no opEquals or opCmp method" ("... more than one ..." when overloads are ambiguous).
// - Callback sorts (sort(less), and sort()/sort_descending() through opCmp) are stable and all-or-nothing: they sort a copy
//   (holding a reference to every handle) and write it back only when every call finished. A call that throws or aborts
//   leaves the array unchanged and passes the exception (same text) or the abort on to the caller; a compare that inserts
//   elements stops the sort with "the array changed while it was sorted".
// - While a callback sort, find, contains or opEquals runs, the arrays involved are busy: remove_at, remove_last,
//   remove_range, clear, a shrinking resize and opAssign raise "the array changed while it was sorted" (during a sort) or
//   "the array changed while it was searched" and change nothing, so script code can never free an element still being
//   compared. Handle elements are also counted around each opEquals/opCmp call.
// - Script code run by element destructors may use the array: removed elements are released after the array is consistent.
// - The C++ API mirrors the script one (snake case, int indices) for Zodiac's own bindings and the plugin ABI; errors raise
//   script exceptions on the active context (nothing happens without one) and never throw C++ exceptions. at() returns
//   nullptr for an index out of range; for object elements it returns the object, for handles and primitives the slot.
#pragma once
#include <angelscript.h>
#include <cstddef>
#include <cstdint>

namespace zodiac {
struct ArrayCache;
class String;

class ScriptArray {
public:
// The element categories the native paths and the copy code switch on (enums use the signed kind of their size).
enum class Kind:std::uint8_t {boolean,int8,int16,int32,int64,uint8,uint16,uint32,uint64,float32,float64,string,object,handle};

static ScriptArray* create(asITypeInfo* type) noexcept;
static ScriptArray* create(asITypeInfo* type,int length) noexcept;
static ScriptArray* create(asITypeInfo* type,int length,const void* value) noexcept;
static ScriptArray* create_from_list(asITypeInfo* type,void* list) noexcept;
// An empty array<string> (type must hold Zodiac's string) with room for exactly capacity elements, for a native producer that
// then appends each element with append_string: no default construction and assignment per element, one block for the slots.
// cache, when not null, is cache_for(type), which a caller that makes many such arrays keeps: finding it again costs a lookup of
// the type's user data under the engine's lock (on Windows a semaphore wait and release, two system calls) per array.
// nullptr after a raised failure.
static ScriptArray* create_strings(asITypeInfo* type,int capacity,ArrayCache* cache=nullptr) noexcept;
// The per-type data every array of type uses, made once per type; nullptr after a raised failure. It lives as long as the type.
static ArrayCache* cache_for(asITypeInfo* type) noexcept;
// Appends a copy of text, copy-constructed in memory from asAllocMem (as the engine allocates value types), into the room
// create_strings made; false after a raised failure ("out of memory", or past that room).
bool append_string(const String& text) noexcept;

void add_ref() const noexcept;
void release() const noexcept;

asITypeInfo* array_type() const noexcept {return type_;}
int array_type_id() const noexcept;
int element_type_id() const noexcept {return sub_type_id_;}
Kind kind() const noexcept {return kind_;}
// True when the elements are Zodiac's script string (zodiac::String).
bool holds_strings() const noexcept {return kind_==Kind::string;}

int length() const noexcept {return static_cast<int>(count_);}
bool empty() const noexcept {return count_==0;}
void reserve(int capacity) noexcept;
// A capacity hint: false (and nothing raised) when the memory cannot be had.
bool try_reserve(int capacity) noexcept;
void resize(int length) noexcept;
void clear() noexcept;

void* at(int index) noexcept;
const void* at(int index) const noexcept;
ScriptArray& assign(const ScriptArray& other) noexcept;
bool equals(const ScriptArray& other) const noexcept;

void insert_at(int index,const void* value) noexcept;
void insert_at(int index,const ScriptArray& values) noexcept;
void insert_last(const void* value) noexcept;
void remove_at(int index) noexcept;
void remove_last() noexcept;
void remove_range(int start,int count) noexcept;

int find(const void* value,int start) const noexcept;
int find_by_ref(const void* value,int start) const noexcept;
bool contains(const void* value) const noexcept;
void sort_ascending() noexcept;
void sort_descending() noexcept;
void sort(asIScriptFunction* less) noexcept;
void reverse() noexcept;
ScriptArray* slice(int start,int count) const noexcept;

// Garbage-collector behaviours.
int ref_count() const noexcept;
void set_gc_flag() noexcept;
bool gc_flag() const noexcept;
void enum_references(asIScriptEngine* engine) noexcept;
void release_all_handles(asIScriptEngine* engine) noexcept;

ScriptArray(const ScriptArray&)=delete;
ScriptArray& operator=(const ScriptArray&)=delete;
private:
explicit ScriptArray(asITypeInfo* type) noexcept;
// An array<string> whose cache is already known (create_strings).
ScriptArray(asITypeInfo* type,ArrayCache* strings) noexcept;
~ScriptArray();
static ScriptArray* allocate(asITypeInfo* type) noexcept;
void notify_gc() noexcept;
asIScriptEngine* engine() const noexcept {return type_->GetEngine();}
asITypeInfo* sub_type() const noexcept {return type_->GetSubType();}
std::byte* slot(std::uint32_t index) const noexcept {return data_+static_cast<std::size_t>(index)*element_size_;}
const void* element_address(std::uint32_t index) const noexcept;
bool has_objects() const noexcept {return kind_>=Kind::string;}
bool native() const noexcept {return kind_<=Kind::string;}
// Exact capacity (at least the length), and geometric growth to at least needed; both raise "out of memory" on failure.
bool set_capacity(std::uint32_t capacity) noexcept;
bool grow(std::uint32_t needed) noexcept;
// Opens count uninitialised slots at at and counts them; the caller fills them before anything else can run.
bool make_room(std::uint32_t at,std::uint32_t count) noexcept;
// Makes a new element from value (a copy, or a counted handle) in an unused slot; false after a raised failure.
bool store_copy(std::byte* slot,const void* value) noexcept;
void destroy_slot(std::byte* slot) noexcept;
void erase_slots(std::uint32_t first,std::uint32_t count) noexcept;
bool assign_object(void* target,const void* value) noexcept;
bool check_compare(bool equality) const noexcept;
bool sort_with_calls(asIScriptFunction* less,bool descending) noexcept;
// Native fast paths (array_fast_paths.cpp); valid only when native().
void native_sort(bool descending) noexcept;
int native_find(const void* value,std::uint32_t start) const noexcept;
bool native_equals(const ScriptArray& other) const noexcept;

mutable int ref_count_=1;
mutable bool gc_flag_=false;
Kind kind_=Kind::int32;
// Object elements (not handles) are stored as pointers to objects the array owns.
bool indirect_=false;
std::uint32_t element_size_=0;
std::uint32_t count_=0;
std::uint32_t capacity_=0;
// While a callback sort, find or equality runs: the error text every element-destroying operation raises instead.
mutable const char* busy_=nullptr;
int sub_type_id_=0;
std::byte* data_=nullptr;
asITypeInfo* type_=nullptr;
ArrayCache* cache_=nullptr;
};

// Registers array<T> as the default array type; throws std::runtime_error naming the declaration the engine refuses.
void register_array(asIScriptEngine* engine);
}
