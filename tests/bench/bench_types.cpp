// bench_types: the speed gate of spec 8.4. Measures Zodiac's script string, array<T>, dictionary, maths and random numbers against
// AngelScript's sample add-ons (scriptstdstring, scriptarray, scriptdictionary, scriptmath) in one process.
// Contract:
// - Two kinds of engine: Zodiac's types and library (registered as ScriptRuntime does: array, random, string, dictionary, maths),
//   and the add-ons (RegisterStdString, RegisterScriptArray as the default array, the bench's random numbers,
//   RegisterStdStringUtils, RegisterScriptDictionary, RegisterScriptMath in its double configuration), both native. The maths
//   Zodiac has and scriptmath does not (integer abs, min, clamp, round, round with digits, hypot) are plain C++ natives on the
//   add-on engine (bench_natives), and so are the random numbers, on std::mt19937_64 with the standard distributions
//   (bench_random): what a game without Zodiac's library would register itself.
// - Every operation of spec 8.4 is a pair of AngelScript snippets with the same semantics, the add-on side written the way a
//   script would have to write it with the add-ons (where the add-on has no such operation, e.g. character indexing on Turkish
//   text, it is the script code doing the same work). Each snippet defines int64 run(int reps), which does ITEMS units of work
//   reps times and returns a checksum, and may define void setup(), run once before timing. Both sides must return the same
//   checksum; a difference is a bench bug and fails the run (exit code 3).
// - Every operation has its own engines on both sides (kEngines per side, see Side), so no operation's objects affect
//   another's and no single engine placement decides a side. Per operation, reps is calibrated so that one run of both
//   sides together takes at least twice the target time. Then come the rounds: each round runs every operation once, both
//   sides back to back (the side that runs first alternates with the round), each run timed alone, with a full garbage
//   collection after it (untimed). Taking the operations in turn spreads each one's rounds over the whole run: on a busy
//   machine the relative speed of two code paths drifts by up to 20 % over a second or so while other programs come and
//   go, and an operation measured in one stretch would read whatever that second's state was.
// - Output: first a line naming the machine, {"machine":{"cpu":brand,"logical_cores":n,"os":build}} (also printed to
//   stderr as the header), then, once every round is done, one JSON line per operation to stdout (and to --out when given):
//   {"op":name,"new_ns":n,"addon_ns":n,"ratio":r}, new_ns and addon_ns the median nanoseconds per unit of work of each
//   side, ratio the median of the per-round ratios new/addon. tools/check_bench.py gates the lines (ratio at most 1.05,
//   and exactly the operations --list prints, the one list of them) and prints the machine above its table.
//   Progress goes to stderr every 10 rounds.
// - Usage: bench_types [--rounds N] [--target-ms N] [--op NAME]... [--out PATH] [--detail] [--layout] [--diagnostic]
//   [--list]. Defaults: 101 rounds (at least 15 are required, spec 8.4) of 1 ms per side: many short rounds pair the two
//   sides closely in time, and the whole run takes about 45 s on the release preset. --op (repeatable) measures only the
//   named operations, --detail also prints each operation's median ratio per pair of engine and context to stderr, --list
//   prints the names. For diagnosis only (tools/bench_diagnose.py, never the gate): --diagnostic adds the operations
//   dict_keys_<n> and dict_get_ordered_<n> (to --list and to the names --op takes) and prints, per operation, the page faults
//   each side's timed runs took ("bench_types: faults <op> zodiac <n> addon <n> ..."), and --layout prints, per dictionary
//   operation and Zodiac engine, a line "bench_types: layout {json}" to stderr with the table's addresses (mod 4096), index
//   size, each key's home slot and probe length, and the cache lines the lookups of all its keys touch.
// - Exit codes: 0 measured, 1 bad arguments, 2 a snippet did not build or run, 3 the two sides disagree on a checksum.
// Not a ctest test (spec 8.4): CI runs it on release presets, and Task 7 reuses it to compare compiler flags.
#include <angelscript.h>
#include <core/library/library.hpp>
#include <core/types/array.hpp>
#include <core/types/dictionary.hpp>
#include <core/types/string_binding.hpp>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scriptmath/scriptmath.h>
#include <scriptstdstring/scriptstdstring.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#endif
#if defined(__x86_64__)
#include <cpuid.h>
#endif

namespace zodiac {
// --layout (declared a friend by Dictionary): where a dictionary's parts sit in memory and how its keys probe, as one JSON object.
struct DictionaryTestAccess {
static std::string layout(const Dictionary& d)
{
const auto low=[](const void* p){return static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(p)&4095u);};
std::string out="{\"table_key0\":\""+hex(string_detail::table_key_word(0))+"\",\"live\":"+std::to_string(d.live_);
out+=",\"dictionary_mod4096\":"+std::to_string(low(&d));
const std::uint32_t size=d.index_?d.mask_+1:0;
out+=",\"index_size\":"+std::to_string(size)+",\"index_mod4096\":"+std::to_string(low(d.index_));
out+=",\"segment0_mod4096\":"+std::to_string(low(d.segments_[0]));
std::vector<std::uintptr_t> lines;
std::string keys;
std::uint32_t probe_total=0,probe_max=0;
// In slot order (Entry is complete only in dictionary.cpp, so the insertion-order links cannot be followed here).
for(std::uint32_t position=0;position<size;++position){
if(!d.index_[position].entry)continue;
const std::uint32_t index=d.index_[position].entry-1;
const Dictionary::Entry& item=d.entry(index);
const std::uint32_t home=d.home(d.index_[position].hash);
const std::uint32_t probe=(position-home)&d.mask_;
probe_total+=probe;
probe_max=std::max(probe_max,probe);
for(std::uint32_t at=home;;at=(at+1)&d.mask_){
lines.push_back(reinterpret_cast<std::uintptr_t>(d.index_+at)/64);
if(at==position)break;
}
lines.push_back(reinterpret_cast<std::uintptr_t>(&item)/64);
if(!keys.empty())keys+=",";
keys+="{\"entry\":"+std::to_string(index)+",\"home\":"+std::to_string(home)+",\"slot\":"+std::to_string(position)+",\"probe\":"+std::to_string(probe)+",\"slot_line\":"+std::to_string((reinterpret_cast<std::uintptr_t>(d.index_+position)/64)%64)+",\"entry_mod4096\":"+std::to_string(low(&item))+"}";
}
std::sort(lines.begin(),lines.end());
const auto distinct=std::unique(lines.begin(),lines.end())-lines.begin();
out+=",\"probe_total\":"+std::to_string(probe_total)+",\"probe_max\":"+std::to_string(probe_max)+",\"lines_touched\":"+std::to_string(distinct);
// Where keys() puts its array in this process: the next script call of the same size is likely to land there too.
if(ScriptArray* sample=d.keys()){
if(sample->length()>0)out+=",\"keys_array_mod4096\":"+std::to_string(low(sample->at(0)));
sample->release();
}
// Each key's own record only for small tables (the large ones would write megabytes per run).
if(d.live_<=64)out+=",\"keys\":["+keys+"]";
out+="}";
return out;
}
static std::string hex(std::uint64_t value)
{
char digits[17];
std::snprintf(digits,sizeof digits,"%016llx",static_cast<unsigned long long>(value));
return digits;
}
};
}

namespace {
// Shared by both sides: the texts every string operation reads and a deterministic pseudo-random sequence.
constexpr std::string_view kCommonPrelude=R"(
string repeat_text(const string &in word,int times)
{
string text;
for(int i=0;i<times;++i)text+=word;
return text;
}
const string ascii_text=repeat_text("The quick brown fox jumps over the lazy dog. ",24);
const string turkish_text=repeat_text("Çağrı ışığı söndü, güzel öğün şimdi yeşil. ",24);
uint seed=12345;
int next_random()
{
seed=seed*1103515245+12345;
return int((seed>>8)&0x7FFFFF);
}
)";
// Zodiac's string counts characters itself.
constexpr std::string_view kZodiacPrelude=R"(
const int ascii_count=ascii_text.length();
const int turkish_count=turkish_text.length();
)";
// What a script needs to handle characters (not bytes) with the add-on string: the texts are valid UTF-8.
constexpr std::string_view kAddonPrelude=R"(
int utf8_size(uint8 lead)
{
if(lead<0x80)return 1;
if(lead<0xE0)return 2;
if(lead<0xF0)return 3;
return 4;
}
int utf8_length(const string &in text)
{
int count=0;
for(uint i=0,n=text.length();i<n;++i)if((text[i]&0xC0)!=0x80)++count;
return count;
}
uint utf8_offset(const string &in text,int index)
{
uint offset=0;
for(int i=0;i<index;++i)offset+=utf8_size(text[offset]);
return offset;
}
int utf8_count(const string &in text,uint bytes)
{
int count=0;
for(uint i=0;i<bytes;++i)if((text[i]&0xC0)!=0x80)++count;
return count;
}
string utf8_char_at(const string &in text,int index)
{
uint offset=utf8_offset(text,index);
return text.substr(offset,utf8_size(text[offset]));
}
int utf8_find(const string &in text,const string &in needle,int start)
{
int at=text.findFirst(needle,utf8_offset(text,start));
return at<0?-1:utf8_count(text,uint(at));
}
string replace_all(const string &in text,const string &in from,const string &in to)
{
string result;
uint start=0;
int at;
while((at=text.findFirst(from,start))>=0){
result+=text.substr(start,uint(at)-start);
result+=to;
start=uint(at)+from.length();
}
result+=text.substr(start);
return result;
}
const int ascii_count=int(ascii_text.length());
const int turkish_count=utf8_length(turkish_text);
)";

struct Operation {
const char* name;
int items;
const char* zodiac;
const char* addon;
};

// Each snippet pair measures the same work; ITEMS (prepended as a constant) is the number of units new_ns and addon_ns divide by.
const Operation kOperations[]={
// Construction of a short string (inline in Zodiac, within libc++'s small-string buffer) from a literal, then a copy of it.
{"string_construct_short",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s="Merhaba";string t=s;n+=t.byte_length();}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s="Merhaba";string t=s;n+=t.length();}return n;})"},
// Construction of a long string (on the heap on both sides) from a literal, then a copy of it.
{"string_construct_long",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s="A longer line of dialogue that does not fit inline.";string t=s;n+=t.byte_length();}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s="A longer line of dialogue that does not fit inline.";string t=s;n+=t.length();}return n;})"},
// Concatenation with +: a new string from two others.
{"string_concat_add",1000,
R"(const string first="Merhaba ";const string second="dünya";
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s=first+second;n+=s.byte_length();}return n;})",
R"(const string first="Merhaba ";const string second="dünya";
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s=first+second;n+=s.length();}return n;})"},
// Concatenation with +=: building a 1280-byte string word by word.
{"string_concat_append",256,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){string s;for(int i=0;i<ITEMS;++i)s+="word ";n+=s.byte_length();}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){string s;for(int i=0;i<ITEMS;++i)s+="word ";n+=s.length();}return n;})"},
// length() of an ASCII text, as a loop condition calls it: characters in Zodiac, bytes in the add-on (the same here).
{"string_length_ascii",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.length();return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.length();return n;})"},
// The character count of a Turkish text: Zodiac's length(), the add-on side counts UTF-8 lead bytes in script.
{"string_length_turkish",100,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=turkish_text.length();return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=utf8_length(turkish_text);return n;})"},
// The character at an index of an ASCII text, as a one-character string: s[i] in Zodiac, substr(i, 1) in the add-on.
{"string_index_ascii",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string c=ascii_text[(i*7919)%ascii_count];if(c=="o")++n;}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string c=ascii_text.substr((i*7919)%ascii_count,1);if(c=="o")++n;}return n;})"},
// The character code at an index of an ASCII text: code_at(i) in Zodiac, the byte s[i] in the add-on.
{"string_code_ascii",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.code_at((i*7919)%ascii_count);return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text[(i*7919)%ascii_count];return n;})"},
// The byte at a byte index of an ASCII text: byte_at(i) in Zodiac, s[i] in the add-on.
{"string_byte_ascii",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.byte_at((i*7919)%ascii_count);return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text[(i*7919)%ascii_count];return n;})"},
// The character at an index of a Turkish text: s[i] in Zodiac; the add-on side walks the UTF-8 sequences up to the index in script.
{"string_index_turkish",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string c=turkish_text[(i*7919)%turkish_count];if(c=="ı")++n;}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string c=utf8_char_at(turkish_text,(i*7919)%turkish_count);if(c=="ı")++n;}return n;})"},
// find from a start position in an ASCII text: find in Zodiac, findFirst in the add-on (byte and character indices agree).
{"string_find_ascii",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.find("lazy dog",(i*37)%1000);return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=ascii_text.findFirst("lazy dog",(i*37)%1000);return n;})"},
// find in a Turkish text with character indices: find in Zodiac; the add-on side converts the start to bytes and the hit back in script.
{"string_find_turkish",100,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=turkish_text.find("yeşil",(i*37)%(turkish_count-50));return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=utf8_find(turkish_text,"yeşil",(i*37)%(turkish_count-50));return n;})"},
// split of a 1080-byte ASCII text at spaces into array<string> (217 parts, empty ones kept on both sides).
{"string_split",10,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){array<string>@ parts=ascii_text.split(" ");n+=parts.length()+parts[1].byte_length();}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){array<string>@ parts=ascii_text.split(" ");n+=parts.length()+parts[1].length();}return n;})"},
// Replacing every match in a 1080-byte ASCII text: Zodiac's replace; the add-on has none, so the script loops over findFirst and substr.
{"string_replace",10,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s=ascii_text.replace("fox","wolf");n+=s.byte_length()+s.find("wolf");}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){string s=replace_all(ascii_text,"fox","wolf");n+=s.length()+s.findFirst("wolf");}return n;})"},
// insert_last of 1000 ints into a new array: insert_last in Zodiac, insertLast in the add-on.
{"array_insert_last_int",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<int> a;for(int i=0;i<ITEMS;++i)a.insert_last(i);n+=a.length()+a[ITEMS-1];}return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<int> a;for(int i=0;i<ITEMS;++i)a.insertLast(i);n+=a.length()+a[ITEMS-1];}return n;})"},
// insert_last of 1000 strings into a new array.
{"array_insert_last_string",1000,
R"(array<string> words;
void setup(){for(int i=0;i<64;++i)words.insert_last("item"+i);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string> a;for(int i=0;i<ITEMS;++i)a.insert_last(words[i&63]);n+=a.length()+a[ITEMS-1].byte_length();}return n;})",
R"(array<string> words;
void setup(){for(int i=0;i<64;++i)words.insertLast("item"+i);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string> a;for(int i=0;i<ITEMS;++i)a.insertLast(words[i&63]);n+=a.length()+a[ITEMS-1].length();}return n;})"},
// find of an int in a 1000-element array, at spread positions.
{"array_find_int",100,
R"(array<int> numbers;
void setup(){for(int i=0;i<1000;++i)numbers.insert_last(i*3);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=numbers.find(((i*397)%1000)*3);return n;})",
R"(array<int> numbers;
void setup(){for(int i=0;i<1000;++i)numbers.insertLast(i*3);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=numbers.find(((i*397)%1000)*3);return n;})"},
// find of a string in a 1000-element array, at spread positions.
{"array_find_string",100,
R"(array<string> names;array<string> probes;
void setup(){for(int i=0;i<1000;++i)names.insert_last("name_"+i);for(int i=0;i<ITEMS;++i)probes.insert_last("name_"+((i*397)%1000));}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=names.find(probes[i]);return n;})",
R"(array<string> names;array<string> probes;
void setup(){for(int i=0;i<1000;++i)names.insertLast("name_"+i);for(int i=0;i<ITEMS;++i)probes.insertLast("name_"+((i*397)%1000));}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=names.find(probes[i]);return n;})"},
// Ascending sort of a copy of 1000 pseudo-random ints: sort() in Zodiac, sortAsc() in the add-on (the copy is the same on both).
{"array_sort_int",1,
R"(array<int> source;
void setup(){for(int i=0;i<1000;++i)source.insert_last(next_random());}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<int> a=source;a.sort();n+=a[0]+a[500]+a[999];}return n;})",
R"(array<int> source;
void setup(){for(int i=0;i<1000;++i)source.insertLast(next_random());}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<int> a=source;a.sortAsc();n+=a[0]+a[500]+a[999];}return n;})"},
// Ascending sort of a copy of 1000 pseudo-random strings.
{"array_sort_string",1,
R"(array<string> source;
void setup(){for(int i=0;i<1000;++i)source.insert_last("name_"+next_random());}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string> a=source;a.sort();n+=a[0].byte_length()+a[500].byte_length()+(a[0]<a[999]?1:0)+a.find(source[0]);}return n;})",
R"(array<string> source;
void setup(){for(int i=0;i<1000;++i)source.insertLast("name_"+next_random());}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string> a=source;a.sortAsc();n+=a[0].length()+a[500].length()+(a[0]<a[999]?1:0)+a.find(source[0]);}return n;})"},
// Maths: the same snippet on both sides, a sum over 1000 arguments of one call each (checksum: the integer part of the sum).
// sqrt, sin, floor and pow against scriptmath.
{"math_sqrt",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=sqrt(i+0.5);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=sqrt(i+0.5);return int64(s);})"},
{"math_sin",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=sin(i*0.0137);return int64(s*1000);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=sin(i*0.0137);return int64(s*1000);})"},
{"math_floor",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=floor(i*0.37-100.0);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=floor(i*0.37-100.0);return int64(s);})"},
{"math_pow",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=pow(1.0001,i*0.5);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=pow(1.0001,i*0.5);return int64(s);})"},
// Integer abs, min, clamp, round, hypot and round with digits against the plain C++ natives (bench_natives).
{"math_abs_int",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=abs(i-500);return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=abs(i-500);return n;})"},
{"math_min_double",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=min(i*0.5,250.25);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=min(i*0.5,250.25);return int64(s);})"},
{"math_clamp_double",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=clamp(i*0.5-100.0,0.0,250.0);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=clamp(i*0.5-100.0,0.0,250.0);return int64(s);})"},
// Quarters from -100 up, so a quarter of the arguments are exact halves.
{"math_round",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=round(i*0.25-100.0);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=round(i*0.25-100.0);return int64(s);})"},
{"math_hypot",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=hypot(i*0.5,300.0-i);return int64(s);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=hypot(i*0.5,300.0-i);return int64(s);})"},
// Computed values rounded to 2 places, none of them near a half (where the two sides would differ, Zodiac's being the
// decimal answer).
{"math_round_digits",1000,
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=round(i*0.0137+0.00031,2);return int64(s*100);})",
R"(int64 run(int reps){double s=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)s+=round(i*0.0137+0.00031,2);return int64(s*100);})"},
// Random numbers against std::mt19937_64 with the standard distributions (bench_random). The two generators give different
// values, so each checksum counts results inside their range, a test both sides pay alike (random_bool's ?1:1 still evaluates
// the call and branches on it).
{"random_int",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random(1,100)<=100)++n;return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random(1,100)<=100)++n;return n;})"},
{"random_float",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random_float()<1.0)++n;return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random_float()<1.0)++n;return n;})"},
{"random_float_range",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random_float(2.0,5.0)<=5.0)++n;return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(random_float(2.0,5.0)<=5.0)++n;return n;})"},
{"random_bool",1000,
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=random_bool()?1:1;return n;})",
R"(int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=random_bool()?1:1;return n;})"},
// A uniform element of a 16-element array<int>.
{"random_choice",1000,
R"(array<int> a;
void setup(){for(int i=0;i<16;++i)a.insert_last(i);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(a.random_choice()<16)++n;return n;})",
R"(array<int> a;
void setup(){for(int i=0;i<16;++i)a.insertLast(i);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(a.random_choice()<16)++n;return n;})"},
// random(1, 100) of a seeded random_generator global (Xoshiro256 in place against std::mt19937_64 in place).
{"generator_random_int",1000,
R"(random_generator g;
void setup(){g.seed(42);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(g.random(1,100)<=100)++n;return n;})",
R"(random_generator g;
void setup(){g.seed(42);}
int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)if(g.random(1,100)<=100)++n;return n;})"},
};

// The add-on engine's counterparts of Zodiac's maths that scriptmath lacks: plain C++, as a game would bind them itself.
namespace bench_natives {
int abs_int(int x)
{
return std::abs(x);
}
double min_double(double a,double b)
{
return std::min(a,b);
}
double clamp_double(double x,double low,double high)
{
return std::clamp(x,low,high);
}
double round_double(double x)
{
return std::round(x);
}
double hypot_double(double x,double y)
{
return std::hypot(x,y);
}
// Multiply, round, divide, with the power of ten from a table (digits from 0 to 16, as the bench calls it).
double round_digits(double x,int digits)
{
static constexpr double powers[]={1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12,1e13,1e14,1e15,1e16};
const double p=powers[digits];
return std::round(x*p)/p;
}
void register_all(asIScriptEngine* engine)
{
engine->RegisterGlobalFunction("int abs(int)",asFUNCTION(abs_int),asCALL_CDECL);
engine->RegisterGlobalFunction("double min(double,double)",asFUNCTION(min_double),asCALL_CDECL);
engine->RegisterGlobalFunction("double clamp(double,double,double)",asFUNCTION(clamp_double),asCALL_CDECL);
engine->RegisterGlobalFunction("double round(double)",asFUNCTION(round_double),asCALL_CDECL);
engine->RegisterGlobalFunction("double hypot(double,double)",asFUNCTION(hypot_double),asCALL_CDECL);
engine->RegisterGlobalFunction("double round(double,int)",asFUNCTION(round_digits),asCALL_CDECL);
}
}

// The add-on engine's random numbers: std::mt19937_64 with std::uniform_int_distribution and std::uniform_real_distribution,
// the plain C++ a game would bind itself (one global generator; random_generator a std::mt19937_64 in place, seeded from
// std::random_device; random_choice a method of the add-on's array<T>).
namespace bench_random {
std::mt19937_64 generator{0x5EEDu};
int random_int(int min,int max)
{
return std::uniform_int_distribution<int>(min,max)(generator);
}
double random_float()
{
return std::uniform_real_distribution<double>(0.0,1.0)(generator);
}
double random_float_range(double min,double max)
{
return std::uniform_real_distribution<double>(min,max)(generator);
}
bool random_bool()
{
return std::uniform_int_distribution<int>(0,1)(generator)!=0;
}
const void* random_choice(const CScriptArray& array)
{
const asUINT size=array.GetSize();
if(size==0){
if(asIScriptContext* context=asGetActiveContext())context->SetException("random_choice on an empty array");
return nullptr;
}
return array.At(std::uniform_int_distribution<asUINT>(0,size-1)(generator));
}
void construct_generator(std::mt19937_64* memory)
{
new(memory) std::mt19937_64(std::random_device{}());
}
void generator_seed(std::mt19937_64& random,std::uint64_t value)
{
random.seed(value);
}
int generator_int(std::mt19937_64& random,int min,int max)
{
return std::uniform_int_distribution<int>(min,max)(random);
}
// After RegisterScriptArray and before anything makes an array instance (RegisterStdStringUtils makes array<string>).
void register_all(asIScriptEngine* engine)
{
static_assert(std::is_trivially_copyable_v<std::mt19937_64>);
engine->RegisterGlobalFunction("int random(int,int)",asFUNCTION(random_int),asCALL_CDECL);
engine->RegisterGlobalFunction("double random_float()",asFUNCTION(random_float),asCALL_CDECL);
engine->RegisterGlobalFunction("double random_float(double,double)",asFUNCTION(random_float_range),asCALL_CDECL);
engine->RegisterGlobalFunction("bool random_bool()",asFUNCTION(random_bool),asCALL_CDECL);
engine->RegisterObjectMethod("array<T>","const T &random_choice() const",asFUNCTION(random_choice),asCALL_CDECL_OBJFIRST);
engine->RegisterObjectType("random_generator",sizeof(std::mt19937_64),asOBJ_VALUE|asOBJ_POD|asOBJ_APP_CLASS|asOBJ_APP_CLASS_ALLINTS);
engine->RegisterObjectBehaviour("random_generator",asBEHAVE_CONSTRUCT,"void f()",asFUNCTION(construct_generator),asCALL_CDECL_OBJFIRST);
engine->RegisterObjectMethod("random_generator","void seed(uint64)",asFUNCTION(generator_seed),asCALL_CDECL_OBJFIRST);
engine->RegisterObjectMethod("random_generator","int random(int,int)",asFUNCTION(generator_int),asCALL_CDECL_OBJFIRST);
}
}

// Dictionary snippets for one size: insert into a new dictionary, look up every key in shuffled order, iterate over the keys.
// The diagnostic pair (--diagnostic, never gated) splits iterate into its two parts: keys() alone, and get of every key in
// insertion order from the setup's array.
struct DictionarySnippets {
std::string insert_name,lookup_name,iterate_name,move_by_name,index_of_name,keys_name,ordered_name;
std::string zodiac_insert,addon_insert,zodiac_lookup,addon_lookup,zodiac_iterate,addon_iterate,zodiac_move_by,addon_move_by,zodiac_index_of,addon_index_of;
std::string zodiac_keys,addon_keys,zodiac_ordered,addon_ordered;
};

DictionarySnippets dictionary_snippets(int size)
{
const std::string count=std::to_string(size);
DictionarySnippets s;
s.insert_name="dict_insert_"+count;
s.lookup_name="dict_lookup_"+count;
s.iterate_name="dict_iterate_"+count;
// The keys, the table they fill and a shuffled copy of them (Fisher-Yates with next_random), the same on both sides
// (untimed). The keys array is reserved first: the add-on's insertLast grows its buffer by one element each time, which
// makes filling 100 000 keys quadratic (two seconds per engine) for no part of what is measured.
const std::string zodiac_setup="array<string> keys;array<string> probes;dictionary table;\n"
"void setup(){keys.reserve(ITEMS);for(int i=0;i<ITEMS;++i){keys.insert_last(\"key\"+i);table.set(keys[i],int64(i));}probes=keys;"
"for(int i=ITEMS-1;i>0;--i){int j=next_random()%(i+1);string t=probes[i];probes[i]=probes[j];probes[j]=t;}}\n";
const std::string addon_setup="array<string> keys;array<string> probes;dictionary table;\n"
"void setup(){keys.reserve(ITEMS);for(int i=0;i<ITEMS;++i){keys.insertLast(\"key\"+i);table.set(keys[i],int64(i));}probes=keys;"
"for(int i=ITEMS-1;i>0;--i){int j=next_random()%(i+1);string t=probes[i];probes[i]=probes[j];probes[j]=t;}}\n";
// set(key, int64) of ITEMS keys into a new dictionary.
s.zodiac_insert=zodiac_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){dictionary d;for(int i=0;i<ITEMS;++i)d.set(keys[i],int64(i));n+=d.length();}return n;}";
s.addon_insert=addon_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){dictionary d;for(int i=0;i<ITEMS;++i)d.set(keys[i],int64(i));n+=d.getSize();}return n;}";
// get(key, int64 &out) of every key, in shuffled order.
const std::string lookup="int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){int64 v;if(table.get(probes[i],v))n+=v;}return n;}";
s.zodiac_lookup=zodiac_setup+lookup;
s.addon_lookup=addon_setup+lookup;
// Every key and its value: keys() in Zodiac, getKeys() in the add-on, then get of each key.
s.zodiac_iterate=zodiac_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string>@ k=table.keys();for(int i=0,c=k.length();i<c;++i){int64 v;table.get(k[i],v);n+=v;}}return n;}";
s.addon_iterate=addon_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string>@ k=table.getKeys();for(uint i=0,c=k.length();i<c;++i){int64 v;table.get(k[i],v);n+=v;}}return n;}";
// The order operations (gated at size 10 only, see all_jobs): the add-on dictionary has no order, so its side keeps the keys' order in an
// array<string> as a script would, finding a key with find() and swapping it with its neighbour for a move by one.
s.move_by_name="dict_move_by_"+count;
s.index_of_name="dict_index_of_"+count;
const std::string addon_order_setup="array<string> keys;array<string> probes;array<string> order;dictionary table;\n"
"void setup(){keys.reserve(ITEMS);for(int i=0;i<ITEMS;++i){keys.insertLast(\"key\"+i);table.set(keys[i],int64(i));}probes=keys;order=keys;"
"for(int i=ITEMS-1;i>0;--i){int j=next_random()%(i+1);string t=probes[i];probes[i]=probes[j];probes[j]=t;}}\n"
"bool move_by(const string &in key,int delta){int at=order.find(key);if(at<0||delta==0)return false;int to=at+delta;if(to<0||to>=int(order.length()))return false;"
"if(delta==1||delta==-1){string t=order[at];order[at]=order[to];order[to]=t;}else{order.removeAt(uint(at));order.insertAt(uint(to),key);}return true;}\n";
// Every key moved by one, alternately towards the back and the front, in shuffled order (the order drifts the same way on both sides).
s.zodiac_move_by=zodiac_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){if(table.move_by(probes[i],(i&1)==0?1:-1))++n;}return n;}";
s.addon_move_by=addon_order_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){if(move_by(probes[i],(i&1)==0?1:-1))++n;}return n;}";
// The position of every key, in shuffled order.
s.zodiac_index_of=zodiac_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=table.index_of(probes[i]);return n;}";
s.addon_index_of=addon_order_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=order.find(probes[i]);return n;}";
s.keys_name="dict_keys_"+count;
s.ordered_name="dict_get_ordered_"+count;
s.zodiac_keys=zodiac_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string>@ k=table.keys();n+=k.length();}return n*ITEMS;}";
s.addon_keys=addon_setup+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r){array<string>@ k=table.getKeys();n+=k.length();}return n*ITEMS;}";
const std::string ordered="int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){int64 v;if(table.get(keys[i],v))n+=v;}return n;}";
s.zodiac_ordered=zodiac_setup+ordered;
s.addon_ordered=addon_setup+ordered;
return s;
}

struct Job {
std::string name;
int items;
std::string zodiac;
std::string addon;
};

// The gated operations, then with diagnostic the diagnostic-only ones (dict_keys_<n>, dict_get_ordered_<n>).
std::vector<Job> all_jobs(bool diagnostic)
{
std::vector<Job> jobs;
for(const Operation& operation:kOperations)jobs.push_back({operation.name,operation.items,operation.zodiac,operation.addon});
std::vector<Job> extra;
for(const int size:{10,1000,100000}){
DictionarySnippets s=dictionary_snippets(size);
jobs.push_back({s.insert_name,size,s.zodiac_insert,s.addon_insert});
jobs.push_back({s.lookup_name,size,s.zodiac_lookup,s.addon_lookup});
jobs.push_back({s.iterate_name,size,s.zodiac_iterate,s.addon_iterate});
// Size 10 only (an inventory): at 1000 the add-on side's array find takes about 0.8 ms per key, which would add minutes to every
// gate run for a comparison Zodiac wins by three orders of magnitude.
if(size==10){
jobs.push_back({s.move_by_name,size,s.zodiac_move_by,s.addon_move_by});
jobs.push_back({s.index_of_name,size,s.zodiac_index_of,s.addon_index_of});
}
extra.push_back({s.keys_name,size,s.zodiac_keys,s.addon_keys});
extra.push_back({s.ordered_name,size,s.zodiac_ordered,s.addon_ordered});
}
// Report-only order operations on 1000 keys (ITEMS=10 calls per rep, so the add-on side's array find, about 0.8 ms per call
// here, stays affordable): moves by one and index_of of keys spread over the order, and index_of of the middle key (n/2, the
// walk's worst case).
const std::string big_zodiac="array<string> probes;dictionary table;\n"
"void setup(){for(int i=0;i<1000;++i)table.set(\"key\"+i,int64(i));for(int i=0;i<ITEMS;++i)probes.insert_last(\"key\"+((i*397)%1000));}\n";
const std::string big_addon="array<string> probes;array<string> order;dictionary table;\n"
"void setup(){for(int i=0;i<1000;++i){table.set(\"key\"+i,int64(i));order.insertLast(\"key\"+i);}for(int i=0;i<ITEMS;++i)probes.insertLast(\"key\"+((i*397)%1000));}\n"
"bool move_by(const string &in key,int delta){int at=order.find(key);if(at<0||delta==0)return false;int to=at+delta;if(to<0||to>=int(order.length()))return false;"
"string t=order[at];order[at]=order[to];order[to]=t;return true;}\n";
const std::string middle_zodiac="dictionary table;\nvoid setup(){for(int i=0;i<1000;++i)table.set(\"key\"+i,int64(i));}\n";
const std::string middle_addon="array<string> order;dictionary table;\nvoid setup(){for(int i=0;i<1000;++i){table.set(\"key\"+i,int64(i));order.insertLast(\"key\"+i);}}\n";
extra.push_back({"dict_move_by_1000",10,
big_zodiac+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){if(table.move_by(probes[i],(i&1)==0?1:-1))++n;}return n;}",
big_addon+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i){if(move_by(probes[i],(i&1)==0?1:-1))++n;}return n;}"});
extra.push_back({"dict_index_of_1000",10,
big_zodiac+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=table.index_of(probes[i]);return n;}",
big_addon+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=order.find(probes[i]);return n;}"});
extra.push_back({"dict_index_of_middle_1000",10,
middle_zodiac+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=table.index_of(\"key500\");return n;}",
middle_addon+"int64 run(int reps){int64 n=0;for(int r=0;r<reps;++r)for(int i=0;i<ITEMS;++i)n+=order.find(\"key500\");return n;}"});
if(diagnostic)jobs.insert(jobs.end(),extra.begin(),extra.end());
return jobs;
}

[[noreturn]] void fail(int code,const std::string& message)
{
std::fprintf(stderr,"bench_types: %s\n",message.c_str());
std::exit(code);
}

void print_message(const asSMessageInfo* info,void*)
{
const char* kind=info->type==asMSGTYPE_ERROR?"error":info->type==asMSGTYPE_WARNING?"warning":"info";
std::fprintf(stderr,"bench_types: %s (%d, %d): %s: %s\n",info->section,info->row,info->col,kind,info->message);
}

asIScriptEngine* make_engine(bool zodiac_types)
{
asIScriptEngine* engine=asCreateScriptEngine();
if(!engine)fail(2,"cannot create an AngelScript engine");
engine->SetMessageCallback(asFUNCTION(print_message),nullptr,asCALL_CDECL);
if(zodiac_types){
zodiac::register_array(engine);
zodiac::register_random(engine);
zodiac::register_string(engine);
zodiac::register_dictionary(engine);
zodiac::register_math(engine);
zodiac::register_time(engine);
}
else{
RegisterStdString(engine);
RegisterScriptArray(engine,true);
bench_random::register_all(engine);
RegisterStdStringUtils(engine);
RegisterScriptDictionary(engine);
RegisterScriptMath(engine);
bench_natives::register_all(engine);
}
return engine;
}

// One side of one operation: kEngines engines of its kind (Zodiac's types or the add-ons), each with kContexts contexts
// and the operation's snippet built in a module. Two effects decide this layout (measured on the owner's machine):
// - Isolation. The engine's state is part of what a run costs: the add-on's array<string>.find compares through nested
//   calls on the active context, and with a 100 000-key dictionary of another operation alive in the same engine one of
//   its runs took 90 s instead of 6 ms. So every operation gets engines of its own, as if it ran alone.
// - Placement. Where a fresh engine's own objects land in memory (the descriptors and tables a native call reads, 4K
//   aliasing, cache sets) moves an operation's speed by several percent with identical code: length() on ASCII text,
//   a one-load leaf on both sides, read 0.93 to 1.05 across processes with one engine per side. So each side runs on
//   kEngines engines, run k on engine k % kEngines and on the next of its contexts (each with its own script stack at its
//   own address), and the medians span all those placements, the same way on both sides.
constexpr int kEngines=4;
constexpr int kContexts=4;
// --diagnostic: count the page faults of every timed run (read outside the timed span).
bool g_count_faults=false;
// The process's page faults so far: PageFaultCount on Windows (soft and hard), minor plus major faults elsewhere.
std::uint64_t page_faults() noexcept
{
#if defined(_WIN32)
PROCESS_MEMORY_COUNTERS counters{};
counters.cb=sizeof counters;
return K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof counters)?counters.PageFaultCount:0;
#else
rusage usage{};
if(getrusage(RUSAGE_SELF,&usage)!=0)return 0;
return static_cast<std::uint64_t>(usage.ru_minflt)+static_cast<std::uint64_t>(usage.ru_majflt);
#endif
}
class Side {
public:
// Builds job's snippet on kEngines new engines and runs its setup() on each (untimed).
Side(bool zodiac_types,const Job& job,std::string_view prelude,const std::string& snippet):name_(job.name+(zodiac_types?" (zodiac)":" (add-on)"))
{
std::string source(kCommonPrelude);
source.append(prelude).append("const int ITEMS=").append(std::to_string(job.items)).append(";\n").append(snippet);
for(Instance& instance:instances_){
instance.engine=make_engine(zodiac_types);
for(asIScriptContext*& context:instance.contexts){
context=instance.engine->CreateContext();
if(!context)fail(2,"cannot create a script context");
}
asIScriptModule* module=instance.engine->GetModule(name_.c_str(),asGM_ALWAYS_CREATE);
instance.module=module;
if(module->AddScriptSection(name_.c_str(),source.data(),source.size())<0||module->Build()<0)fail(2,name_+" does not build");
instance.run=module->GetFunctionByDecl("int64 run(int)");
if(!instance.run)fail(2,name_+" has no int64 run(int)");
if(asIScriptFunction* setup=module->GetFunctionByDecl("void setup()")){
context_=instance.contexts[0];
if(context_->Prepare(setup)<0||context_->Execute()!=asEXECUTION_FINISHED)fail(2,name_+": setup() failed"+exception());
context_->Unprepare();
}
}
}
// The contexts are released before their engine shuts down (which discards the module).
~Side()
{
for(Instance& instance:instances_){
for(asIScriptContext* context:instance.contexts)context->Release();
instance.engine->ShutDownAndRelease();
}
}
Side(const Side&)=delete;
Side& operator=(const Side&)=delete;
// Runs run(reps) once on the next engine and context; returns its checksum and stores the time taken in nanoseconds.
// The garbage collection after it is untimed.
std::int64_t run(int reps,double& ns)
{
Instance& instance=instances_[next_engine_];
last_placement_=next_engine_*kContexts+instance.next_context;
next_engine_=(next_engine_+1)%kEngines;
context_=instance.contexts[instance.next_context];
instance.next_context=(instance.next_context+1)%kContexts;
if(context_->Prepare(instance.run)<0)fail(2,name_+": cannot prepare run()");
context_->SetArgDWord(0,static_cast<asDWORD>(reps));
const std::uint64_t faults_before=g_count_faults?page_faults():0;
const auto start=std::chrono::steady_clock::now();
const int result=context_->Execute();
const auto stop=std::chrono::steady_clock::now();
if(g_count_faults)faults+=page_faults()-faults_before;
if(result!=asEXECUTION_FINISHED)fail(2,name_+": run() failed"+exception());
ns=std::chrono::duration<double,std::nano>(stop-start).count();
const std::int64_t checksum=static_cast<std::int64_t>(context_->GetReturnQWord());
context_->Unprepare();
instance.engine->GarbageCollect(asGC_FULL_CYCLE);
return checksum;
}
// The engine and context of the last run, as engine*kContexts+context (for --detail).
int last_placement() const noexcept
{
return last_placement_;
}
// With --diagnostic: the page faults the process took during this side's timed runs (calibration included until reset).
std::uint64_t faults=0;
// The address of the script global name on engine (the object itself for an object variable), or nullptr.
void* global(int engine,const char* name) const
{
asIScriptModule* module=instances_[engine].module;
const int index=module->GetGlobalVarIndexByName(name);
return index<0?nullptr:module->GetAddressOfGlobalVar(static_cast<asUINT>(index));
}
private:
struct Instance {
asIScriptEngine* engine=nullptr;
asIScriptContext* contexts[kContexts]={};
int next_context=0;
asIScriptModule* module=nullptr;
asIScriptFunction* run=nullptr;
};
std::string exception() const
{
if(context_->GetState()!=asEXECUTION_EXCEPTION)return "";
const char* text=context_->GetExceptionString();
return std::string(": ")+(text?text:"");
}
std::string name_;
Instance instances_[kEngines];
asIScriptContext* context_=nullptr;
int next_engine_=0;
int last_placement_=0;
};

double median(std::vector<double> values)
{
const std::size_t middle=values.size()/2;
std::nth_element(values.begin(),values.begin()+static_cast<std::ptrdiff_t>(middle),values.end());
const double upper=values[middle];
if(values.size()%2)return upper;
return (upper+*std::max_element(values.begin(),values.begin()+static_cast<std::ptrdiff_t>(middle)))/2;
}

void append_number(std::string& out,double value)
{
char digits[64];
const auto end=std::to_chars(digits,digits+sizeof digits,value,std::chars_format::fixed,value<10?4:2).ptr;
out.append(digits,end);
}

// One operation under measurement: its two sides, its calibrated reps and the times of its rounds.
struct Measurement {
Measurement(const Job& job_,int rounds):job(&job_),zodiac(true,job_,kZodiacPrelude,job_.zodiac),addon(false,job_,kAddonPrelude,job_.addon)
{
zodiac_times.reserve(static_cast<std::size_t>(rounds));
addon_times.reserve(static_cast<std::size_t>(rounds));
ratios.reserve(static_cast<std::size_t>(rounds));
placements.reserve(static_cast<std::size_t>(rounds));
}
const Job* job;
Side zodiac;
Side addon;
int reps=1;
std::vector<double> zodiac_times,addon_times,ratios;
// The placements of each round's two runs, as zodiac*kEngines*kContexts+addon (for --detail).
std::vector<int> placements;
};

void check(const Measurement& m,std::int64_t zodiac_sum,std::int64_t addon_sum)
{
if(zodiac_sum!=addon_sum)fail(3,m.job->name+": Zodiac's checksum "+std::to_string(zodiac_sum)+" differs from the add-on's "+std::to_string(addon_sum)+" at reps "+std::to_string(m.reps));
}

// Grows reps until one run of both sides takes at least twice the target (each run doubles as a warm-up).
void calibrate(Measurement& m,double target_ns)
{
for(;;){
double zodiac_ns=0,addon_ns=0;
const std::int64_t zodiac_sum=m.zodiac.run(m.reps,zodiac_ns);
const std::int64_t addon_sum=m.addon.run(m.reps,addon_ns);
check(m,zodiac_sum,addon_sum);
const double total=zodiac_ns+addon_ns;
if(total>=2*target_ns||m.reps>=(1<<24))return;
const double scale=total>0?2*target_ns/total*1.1:16;
m.reps=static_cast<int>(std::min<double>(1<<24,std::max<double>(m.reps*2.0,m.reps*scale)));
}
}

// One round of m: both sides back to back, the side that runs first alternating with the round.
void measure_round(Measurement& m,int round)
{
double zodiac_ns=0,addon_ns=0;
std::int64_t zodiac_sum,addon_sum;
if(round%2==0){
zodiac_sum=m.zodiac.run(m.reps,zodiac_ns);
addon_sum=m.addon.run(m.reps,addon_ns);
}
else{
addon_sum=m.addon.run(m.reps,addon_ns);
zodiac_sum=m.zodiac.run(m.reps,zodiac_ns);
}
check(m,zodiac_sum,addon_sum);
m.zodiac_times.push_back(zodiac_ns);
m.addon_times.push_back(addon_ns);
m.ratios.push_back(zodiac_ns/addon_ns);
m.placements.push_back(m.zodiac.last_placement()*kEngines*kContexts+m.addon.last_placement());
}

// --detail: the median ratio of m's rounds per pair of placements (Zodiac engine.context/add-on engine.context), to tell
// an operation that differs on every placement from one that a few placements decide.
std::string detail_line(const Measurement& m)
{
constexpr int placements=kEngines*kContexts;
std::vector<std::vector<double>> groups(static_cast<std::size_t>(placements*placements));
for(std::size_t i=0;i<m.ratios.size();++i)groups[static_cast<std::size_t>(m.placements[i])].push_back(m.ratios[i]);
std::string line="bench_types: detail "+m.job->name;
for(int key=0;key<placements*placements;++key){
const std::vector<double>& group=groups[static_cast<std::size_t>(key)];
if(group.empty())continue;
const int z=key/placements,a=key%placements;
line.append(" ").append(std::to_string(z/kContexts)).append(".").append(std::to_string(z%kContexts)).append("/").append(std::to_string(a/kContexts)).append(".").append(std::to_string(a%kContexts)).append("=");
char digits[32];
line.append(digits,std::to_chars(digits,digits+sizeof digits,median(group),std::chars_format::fixed,3).ptr);
line.append("x").append(std::to_string(group.size()));
}
line.append("\n");
return line;
}

std::string result_line(const Measurement& m)
{
const double units=static_cast<double>(m.reps)*m.job->items;
std::string line="{\"op\":\"";
line.append(m.job->name).append("\",\"new_ns\":");
append_number(line,median(m.zodiac_times)/units);
line.append(",\"addon_ns\":");
append_number(line,median(m.addon_times)/units);
line.append(",\"ratio\":");
char digits[32];
line.append(digits,std::to_chars(digits,digits+sizeof digits,median(m.ratios),std::chars_format::fixed,4).ptr);
line.append("}\n");
return line;
}

bool parse_int(std::string_view text,int& value)
{
const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
return error==std::errc()&&end==text.data()+text.size();
}

// The machine a run measured, so every log and result file names it (shared CI runners differ in CPU from job to job).
struct Machine {
// The CPU's brand string (CPUID leaves 0x80000002-0x80000004 on x86-64), spaces trimmed and runs of them folded; "unknown"
// where there is none.
std::string cpu;
unsigned logical_cores=0;
// "Windows <major>.<minor>.<build>.<revision>" (RtlGetVersion, which reports the real version, and the UBR registry value),
// or uname's "<system> <release> <version>".
std::string os;
};

std::string folded_spaces(std::string_view text)
{
std::string out;
for(const char c:text){
if(c==' '&&(out.empty()||out.back()==' '))continue;
out.push_back(c);
}
while(!out.empty()&&out.back()==' ')out.pop_back();
return out;
}

std::string cpu_brand()
{
#if defined(__x86_64__)
unsigned words[12];
unsigned top=0,b=0,c=0,d=0;
if(!__get_cpuid(0x80000000u,&top,&b,&c,&d)||top<0x80000004u)return "unknown";
for(unsigned leaf=0;leaf<3;++leaf)__get_cpuid(0x80000002u+leaf,&words[leaf*4],&words[leaf*4+1],&words[leaf*4+2],&words[leaf*4+3]);
char brand[sizeof words+1]={};
std::memcpy(brand,words,sizeof words);
const std::string text=folded_spaces(brand);
return text.empty()?"unknown":text;
#elif defined(__linux__)
// Elsewhere on Linux (aarch64): /proc/cpuinfo's model name when the kernel gives one, otherwise the MIDR fields it lists
// ("CPU implementer 0x41 part 0xd0c", which names the core).
std::FILE* file=std::fopen("/proc/cpuinfo","r");
if(!file)return "unknown";
std::string model,implementer,part;
char line[512];
while(std::fgets(line,sizeof line,file)){
const std::string_view text(line);
const std::size_t colon=text.find(':');
if(colon==std::string_view::npos)continue;
const auto trimmed=[](std::string_view part){
const std::size_t first=part.find_first_not_of(" \t\r\n");
return first==std::string_view::npos?std::string():folded_spaces(part.substr(first,part.find_last_not_of(" \t\r\n")-first+1));
};
const std::string key=trimmed(text.substr(0,colon));
const std::string value=trimmed(text.substr(colon+1));
if(model.empty()&&(key=="model name"||key=="Model"))model=value;
else if(implementer.empty()&&key=="CPU implementer")implementer=value;
else if(part.empty()&&key=="CPU part")part=value;
}
std::fclose(file);
if(!model.empty())return model;
if(!implementer.empty())return "CPU implementer "+implementer+" part "+part;
return "unknown";
#else
return "unknown";
#endif
}

std::string os_build()
{
#if defined(_WIN32)
using GetVersion=LONG(WINAPI*)(OSVERSIONINFOW*);
OSVERSIONINFOW info{};
info.dwOSVersionInfoSize=sizeof info;
const HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
const auto get_version=ntdll?reinterpret_cast<GetVersion>(reinterpret_cast<void*>(GetProcAddress(ntdll,"RtlGetVersion"))):nullptr;
if(!get_version||get_version(&info)!=0)return "Windows (unknown version)";
std::string text="Windows "+std::to_string(info.dwMajorVersion)+"."+std::to_string(info.dwMinorVersion)+"."+std::to_string(info.dwBuildNumber);
DWORD revision=0;
DWORD size=sizeof revision;
if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",L"UBR",RRF_RT_REG_DWORD,nullptr,&revision,&size)==ERROR_SUCCESS)text+="."+std::to_string(revision);
return text;
#else
utsname name{};
if(uname(&name)!=0)return "unknown";
return folded_spaces(std::string(name.sysname)+" "+name.release+" "+name.version);
#endif
}

Machine this_machine()
{
return Machine{cpu_brand(),std::thread::hardware_concurrency(),os_build()};
}

// A JSON string literal of text (the brand and uname texts are ASCII, but nothing is assumed).
std::string json_text(std::string_view text)
{
std::string out="\"";
for(const char c:text){
const auto byte=static_cast<unsigned char>(c);
if(c=='"'||c=='\\'){
out.push_back('\\');
out.push_back(c);
}
else if(byte<0x20||byte>=0x7F){
char escape[8];
std::snprintf(escape,sizeof escape,"\\u%04x",byte);
out.append(escape);
}
else out.push_back(c);
}
out.push_back('"');
return out;
}

std::string machine_line(const Machine& machine)
{
return "{\"machine\":{\"cpu\":"+json_text(machine.cpu)+",\"logical_cores\":"+std::to_string(machine.logical_cores)+",\"os\":"+json_text(machine.os)+"}}\n";
}

// Measurement hygiene, the same for both sides: on Windows a high priority class, so other programs interrupt the
// interleaved runs less (Linux needs privileges for that), then a short spin so the CPU leaves its idle clock before the
// first operation. Pinning to one CPU was tried and made runs noisier on a loaded machine, so the scheduler keeps its choice.
void quiet_process()
{
#if defined(_WIN32)
SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS);
#endif
const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(300);
volatile std::uint64_t spin=0;
while(std::chrono::steady_clock::now()<until)spin=spin+1;
}
}

int main(int argc,char** argv)
{
int rounds=101;
int target_ms=1;
std::vector<std::string> only;
const char* out_path=nullptr;
bool list=false;
bool detail=false;
bool layout=false;
// --diagnostic may follow the --op that names a diagnostic operation, so it is found first.
bool diagnostic=false;
for(int i=1;i<argc;++i)diagnostic=diagnostic||std::string_view(argv[i])=="--diagnostic";
const std::vector<Job> jobs=all_jobs(diagnostic);
for(int i=1;i<argc;++i){
const std::string_view arg=argv[i];
const bool has_value=i+1<argc;
if(arg=="--rounds"&&has_value){
if(!parse_int(argv[++i],rounds)||rounds<15)fail(1,"--rounds needs a whole number of at least 15 (spec 8.4)");
}
else if(arg=="--target-ms"&&has_value){
if(!parse_int(argv[++i],target_ms)||target_ms<1)fail(1,"--target-ms needs a whole number of at least 1");
}
else if(arg=="--op"&&has_value){
const std::string name=argv[++i];
if(std::none_of(jobs.begin(),jobs.end(),[&name](const Job& job){return job.name==name;}))fail(1,"unknown operation "+name+" (--list prints them)");
only.push_back(name);
}
else if(arg=="--out"&&has_value)out_path=argv[++i];
else if(arg=="--list")list=true;
else if(arg=="--detail")detail=true;
else if(arg=="--layout")layout=true;
else if(arg=="--diagnostic"){}
else fail(1,"usage: bench_types [--rounds N] [--target-ms N] [--op NAME]... [--out PATH] [--detail] [--layout] [--diagnostic] [--list]");
}
if(list){
for(const Job& job:jobs)std::printf("%s\n",job.name.c_str());
return 0;
}
std::FILE* out=nullptr;
if(out_path){
out=std::fopen(out_path,"wb");
if(!out)fail(1,std::string("cannot write ")+out_path+": "+std::strerror(errno));
}
const Machine machine=this_machine();
std::fprintf(stderr,"bench_types: machine: %s, %u logical cores, %s\n",machine.cpu.c_str(),machine.logical_cores,machine.os.c_str());
{
const std::string line=machine_line(machine);
std::fwrite(line.data(),1,line.size(),stdout);
if(out&&std::fwrite(line.data(),1,line.size(),out)!=line.size())fail(1,std::string("cannot write ")+out_path);
}
quiet_process();
asPrepareMultithread();
const double target_ns=target_ms*1e6;
{
// Each measurement's sides shut their engines down when the deque goes out of scope (a deque never moves its elements).
std::deque<Measurement> measurements;
for(const Job& job:jobs){
if(!only.empty()&&std::find(only.begin(),only.end(),job.name)==only.end())continue;
measurements.emplace_back(job,rounds);
}
g_count_faults=diagnostic;
for(Measurement& m:measurements)calibrate(m,target_ns);
for(Measurement& m:measurements)m.zodiac.faults=m.addon.faults=0;
// The rounds go over every operation in turn, so each operation's rounds spread over the whole run (see the contract).
for(int round=0;round<rounds;++round){
for(Measurement& m:measurements)measure_round(m,round);
if((round+1)%10==0||round+1==rounds)std::fprintf(stderr,"bench_types: round %d of %d\n",round+1,rounds);
}
if(detail){
for(const Measurement& m:measurements){
const std::string line=detail_line(m);
std::fwrite(line.data(),1,line.size(),stderr);
}
}
if(diagnostic){
for(const Measurement& m:measurements){
const double runs=static_cast<double>(m.ratios.size());
char line[256];
std::snprintf(line,sizeof line,"bench_types: faults %s zodiac %llu addon %llu (per timed run %.2f and %.2f, %d reps each)\n",m.job->name.c_str(),static_cast<unsigned long long>(m.zodiac.faults),static_cast<unsigned long long>(m.addon.faults),static_cast<double>(m.zodiac.faults)/runs,static_cast<double>(m.addon.faults)/runs,m.reps);
std::fputs(line,stderr);
}
}
if(layout){
for(const Measurement& m:measurements){
for(int engine=0;engine<kEngines;++engine){
const auto* table=static_cast<const zodiac::Dictionary*>(m.zodiac.global(engine,"table"));
if(!table)continue;
const std::string line="bench_types: layout {\"op\":\""+m.job->name+"\",\"engine\":"+std::to_string(engine)+",\"table\":"+zodiac::DictionaryTestAccess::layout(*table)+"}\n";
std::fwrite(line.data(),1,line.size(),stderr);
}
}
}
for(const Measurement& m:measurements){
const std::string line=result_line(m);
std::fwrite(line.data(),1,line.size(),stdout);
if(out&&std::fwrite(line.data(),1,line.size(),out)!=line.size())fail(1,std::string("cannot write ")+out_path);
}
std::fflush(stdout);
}
if(out&&std::fclose(out)!=0)fail(1,std::string("cannot write ")+out_path);
asUnprepareMultithread();
return 0;
}
