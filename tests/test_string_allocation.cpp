// Tests zodiac::String when the heap runs out in the middle of an append (final review item 1): this executable replaces the
// global operator new so a test can make one chosen allocation of the current thread throw std::bad_alloc. An append whose
// bytes are already committed when growing the sparse index fails must still leave an index that covers the whole text
// (both append paths: the bytes fit the block, and the block is reallocated first), and an append whose new block cannot be
// allocated changes nothing. Every character is checked against a std::u32string model afterwards.
#include <core/types/string.hpp>
#include <doctest/doctest.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {
// The allocation (counted from 1) of this thread that throws, 0 when none is armed; failed counts the throws.
thread_local int fail_at=0;
thread_local int failed=0;
}

void* operator new(std::size_t size)
{
if(fail_at>0&&--fail_at==0){
++failed;
throw std::bad_alloc();
}
if(void* block=std::malloc(size==0?1:size))return block;
throw std::bad_alloc();
}

namespace {
using zodiac::String;

// Characters of 1, 2, 3 and 4 bytes, so byte offsets are not a multiple of the character index.
constexpr char32_t pattern[]={U'a',U'ş',U'€',U'\U0001F600',U'İ',U'z'};

void append_utf8(std::string& out,char32_t code)
{
if(code<0x80){
out.push_back(static_cast<char>(code));
}else if(code<0x800){
out.push_back(static_cast<char>(0xC0|(code>>6)));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}else if(code<0x10000){
out.push_back(static_cast<char>(0xE0|(code>>12)));
out.push_back(static_cast<char>(0x80|((code>>6)&0x3F)));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}else{
out.push_back(static_cast<char>(0xF0|(code>>18)));
out.push_back(static_cast<char>(0x80|((code>>12)&0x3F)));
out.push_back(static_cast<char>(0x80|((code>>6)&0x3F)));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}
}
// count characters of the pattern starting at its first'th character, as UTF-8 and as code points.
void make_text(std::size_t first,std::size_t count,std::string& bytes,std::u32string& codes)
{
for(std::size_t at=0;at<count;++at){
const char32_t code=pattern[(first+at)%std::size(pattern)];
append_utf8(bytes,code);
codes.push_back(code);
}
}
// Every character of text read through the index (code_at, char_at, substr and find of the last character) against model.
void check_every_character(const String& text,const std::u32string& model)
{
REQUIRE(text.length()==static_cast<int>(model.size()));
for(std::size_t at=0;at<model.size();++at){
INFO("character ",at);
const auto index=static_cast<int>(at);
const auto code=text.code_at(index);
REQUIRE(code.has_value());
CHECK(*code==model[at]);
std::string expected;
append_utf8(expected,model[at]);
const auto one=text.char_at(index);
REQUIRE(one.has_value());
CHECK(one->bytes()==expected);
const auto rest=text.substr(index);
REQUIRE(rest.has_value());
CHECK(rest->length()==static_cast<int>(model.size()-at));
}
std::string last;
append_utf8(last,model.back());
const auto hit=text.rfind(last);
REQUIRE(hit.has_value());
CHECK(*hit==static_cast<int>(model.size()-1));
}
// Appends bytes to text with the allocation-th allocation of the append failing; true when the append threw std::bad_alloc.
// No test macro runs while the failure is armed, so only the append's own allocations are counted.
bool append_failing(String& text,std::string_view bytes,int allocation)
{
failed=0;
fail_at=allocation;
bool threw=false;
try{
text.append(bytes);
}
catch(const std::bad_alloc&){
threw=true;
}
fail_at=0;
return threw;
}
// One append under an armed failure, and what it left. The failure is injected at the heap (the replaced operator new above is what
// zodiac::heap::new_block calls) and the whole scenario runs on a thread of its own: String's per-thread block cache starts empty
// there, so every block the scenario asks for comes from the heap and is counted, whatever sizes the cache holds (Task 5c fix round 2:
// before, the tests relied on blocks above the cache's 256-byte limit). The test macros run afterwards, on the test's thread.
struct Appended {
std::string bytes;
std::string more;
std::u32string model;
std::optional<String> text;
// The text was built as asked (its characters counted, its sparse index built).
bool built=false;
std::size_t capacity_before=0;
bool threw=false;
int failures=0;
};
// A heap string of 100 characters with its sparse index built (a read past index_step characters builds it), with reserve bytes
// reserved first, then an append of 70 more whose allocation-th allocation fails; model gets the 70 only when with_more.
Appended append_on_fresh_thread(int reserve,int allocation,bool with_more)
{
Appended out;
std::thread worker([&out,reserve,allocation,with_more]{
make_text(0,100,out.bytes,out.model);
String text;
text.reserve(reserve);
text.append(std::string_view(out.bytes));
out.built=text.length()==100&&text.code_at(99).has_value();
std::u32string more_model;
make_text(100,70,out.more,more_model);
if(with_more)out.model+=more_model;
out.capacity_before=static_cast<std::size_t>(text.capacity());
out.threw=append_failing(text,out.more,allocation);
out.failures=failed;
out.text.emplace(std::move(text));
});
worker.join();
return out;
}
}

TEST_CASE("an append whose sparse index cannot grow keeps an index covering the whole text (bytes fit the block)")
{
// 100 characters have an index of 4 entries; 70 more need 6, so the extension has to grow the index block, and that
// allocation is the only one the append makes (the reserved block holds all the bytes).
Appended run=append_on_fresh_thread(4096,1,true);
REQUIRE(run.built);
REQUIRE(run.capacity_before>=run.bytes.size()+run.more.size());
CHECK_FALSE(run.threw);
CHECK(run.failures==1);
String& text=*run.text;
CHECK(text.bytes()==run.bytes+run.more);
check_every_character(text,run.model);
// The index dropped by the failure is rebuilt, and later appends extend it as usual.
std::string again;
make_text(170,90,again,run.model);
text.append(std::string_view(again));
CHECK(text.bytes()==run.bytes+run.more+again);
check_every_character(text,run.model);
}

TEST_CASE("an append whose sparse index cannot grow keeps an index covering the whole text (block reallocated)")
{
// No spare room: the append reallocates the block (the first allocation) and then grows the index (the second), which fails.
Appended run=append_on_fresh_thread(0,2,true);
REQUIRE(run.built);
REQUIRE(run.capacity_before<run.bytes.size()+run.more.size());
CHECK_FALSE(run.threw);
CHECK(run.failures==1);
CHECK(run.text->bytes()==run.bytes+run.more);
check_every_character(*run.text,run.model);
}

TEST_CASE("an append whose new block cannot be allocated throws and changes nothing")
{
Appended run=append_on_fresh_thread(0,1,false);
REQUIRE(run.built);
CHECK(run.threw);
CHECK(run.failures==1);
CHECK(run.text->bytes()==run.bytes);
check_every_character(*run.text,run.model);
}
