// Tests the script `string` (zodiac::String registered by register_string) and the exception helpers at script level:
// every method group of spec 8.1, the exact range and conversion error texts, concatenation with numbers and bools both
// ways, format(), string constants shared across threads, the plugin ABI string paths with NUL bytes and Turkish text
// (Review Focus 3), Turkish case mapping (Review Focus 5), and throw()/exception_info()/getExceptionInfo() (spec 8.8).
#include "script_fixture.hpp"
#include <core/types/string.hpp>
#include <core/types/string_binding.hpp>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using zodiac_test::ScriptFixture;

namespace {
// Runs "bool main(){return <expression>;}".
bool holds(ScriptFixture& fixture,std::string_view expression)
{
const std::string source="bool main(){return "+std::string(expression)+";}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"bool main()")==asEXECUTION_FINISHED);
return fixture.context->GetReturnByte()!=0;
}
// Runs "string main(){return <expression>;}" and returns the bytes of the result.
std::string value_of(ScriptFixture& fixture,std::string_view expression)
{
const std::string source="string main(){return "+std::string(expression)+";}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"string main()")==asEXECUTION_FINISHED);
return fixture.returned_string();
}
// Runs "void main(){<statements>}", which must raise a script exception, and returns its text.
// True when "void main(){<statements>}" compiles in the fixture's "test" module.
bool compiles(ScriptFixture& fixture,std::string_view statements)
{
const std::string source="void main(){"+std::string(statements)+"}";
asIScriptModule* module=fixture.engine->GetModule("test",asGM_ALWAYS_CREATE);
REQUIRE(module!=nullptr);
REQUIRE(module->AddScriptSection("test",source.c_str())>=0);
fixture.messages.clear();
return module->Build()>=0;
}

std::string error_of(ScriptFixture& fixture,std::string_view statements)
{
const std::string source="void main(){"+std::string(statements)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"void main()")==asEXECUTION_EXCEPTION);
return fixture.exception_string();
}
const zodiac_host* g_abi=nullptr;
std::string g_seen;
bool g_terminated=false;
constexpr std::string_view g_plugin_bytes{"a\0\xC5\x9F" "b",5};
void bytes_out(zodiac_call* call)
{
g_abi->ret_string(call,g_plugin_bytes.data(),g_plugin_bytes.size());
}
void bytes_echo(zodiac_call* call)
{
size_t length=0;
const char* data=g_abi->arg_string(call,0,&length);
g_terminated=data!=nullptr&&data[length]=='\0';
g_seen.assign(data,length);
g_abi->ret_string(call,data,length);
}
void bytes_list(zodiac_call* call)
{
zodiac_array* items=g_abi->ret_array(call,"string",0);
g_abi->array_push_string(items,"\0\xC4\xB0",3);
g_abi->array_push_string(items,g_plugin_bytes.data(),g_plugin_bytes.size());
}
void bytes_join(zodiac_call* call)
{
zodiac_array* items=g_abi->arg_array(call,0);
std::string joined;
for(uint32_t index=0;index<g_abi->array_length(items);++index){
size_t size=0;
const char* data=g_abi->array_get_string(items,index,&size);
joined.append(data,size);
}
g_abi->ret_string(call,joined.data(),joined.size());
}
}

TEST_CASE("string is a value type matching zodiac::String")
{
ScriptFixture fixture;
CHECK(asGetTypeTraits<zodiac::String>()==asOBJ_APP_CLASS_CDAK);
const asITypeInfo* type=fixture.engine->GetTypeInfoByName("string");
REQUIRE(type!=nullptr);
CHECK(type->GetSize()==sizeof(zodiac::String));
CHECK((type->GetFlags()&(asOBJ_VALUE|asOBJ_APP_CLASS_CDAK))==(asOBJ_VALUE|asOBJ_APP_CLASS_CDAK));
CHECK(fixture.engine->GetStringFactory()==type->GetTypeId());
}

TEST_CASE("size and access count characters")
{
ScriptFixture fixture;
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\".length()==5"));
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\".byte_length()==6"));
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\"[1]==\"e\""));
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\"[0]==\"\xC5\x9F\""));
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\".code_at(0)==0x15F"));
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\".byte_at(1)==0x9F"));
CHECK(holds(fixture,"\"\".empty()&&!\"a\".empty()"));
CHECK(holds(fixture,"\"a\\x00b\".byte_length()==3&&\"a\\x00b\".length()==3"));
CHECK(error_of(fixture,"string s=\"abc\"[3];")=="index out of range");
CHECK(error_of(fixture,"string s=\"abc\"[-1];")=="index out of range");
// s[i] on text already known to be ASCII (length() scanned it) takes the one-byte leaf: its edges, heap text, and the strings it makes.
CHECK(error_of(fixture,"string t=\"abc\";int n=t.length();string s=t[n];")=="index out of range");
CHECK(error_of(fixture,"string t=\"abc\";int n=t.length();string s=t[-1];")=="index out of range");
REQUIRE(fixture.run("bool main(){string t=\"a text long enough to live on the heap\";int n=t.length();string c=t[n-1];string d=t[0]+t[2];"
"return c==\"p\"&&c.length()==1&&c.byte_length()==1&&d==\"at\"&&!t[1].empty()&&t[1]==\" \"&&(t[0]+\"!\")==\"a!\";}","bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
// Text whose one byte is not ASCII ("\xFF", not valid UTF-8) takes the fallback, the same character the general path gives; ASCII text
// that was scanned, then grows by a multibyte character, is indexed by characters again (its meta no longer says ASCII).
REQUIRE(fixture.run("bool main(){string t=\"\\xFF\";int n=t.length();string c=t[0];"
"string u=\"abc\";int m=u.length();u+=\"\xC5\x9F\";"
"return n==1&&c==t.substr(0,1)&&c.byte_length()==1&&m==3&&u.length()==4&&u[3]==\"\xC5\x9F\"&&u[3].byte_length()==2&&u[0]==\"a\"&&u[2]==\"c\";}","bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
CHECK(error_of(fixture,"string u=\"abc\";int m=u.length();u+=\"\xC5\x9F\";string c=u[4];")=="index out of range");
CHECK(error_of(fixture,"uint c=\"abc\".code_at(3);")=="index out of range");
CHECK(error_of(fixture,"uint8 b=\"\xC5\x9F\".byte_at(2);")=="index out of range");
}

TEST_CASE("code_at and byte_at values stay correct however scripts combine them")
{
// Both return const references (the engine's fast call path): code_at into a table of code points, filled per block on
// first use, byte_at into the string. Scripts must only ever see the values.
ScriptFixture fixture;
const std::string text="a\xC5\x9F\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBD\xC4\xB1";
const std::string source="string t=\""+text+"\";\n"
"bool main(){\n"
"if(t.code_at(0)!=0x61||t.code_at(1)!=0x15F||t.code_at(2)!=0x20AC||t.code_at(3)!=0x1F600||t.code_at(4)!=0xFFFD||t.code_at(5)!=0x131)return false;\n"
"if(t.code_at(1)==t.code_at(2)||t.code_at(5)==t.code_at(4)||t.code_at(2)+t.code_at(3)!=0x20AC+0x1F600)return false;\n"
"auto c=t.code_at(3);c+=1;if(c!=0x1F601||t.code_at(3)!=0x1F600)return false;\n"
"uint8 b=t.byte_at(1);b+=1;if(b!=0xC6||t.byte_at(1)!=0xC5||t.byte_at(1)==t.byte_at(2))return false;\n"
"uint sum=0;for(int i=0;i<t.length();++i)sum+=t.code_at(i);\n"
"string s=\"x\";uint8 before=s.byte_at(0);s=\"a much longer text that moves to the heap\";\n"
"return sum==0x61+0x15F+0x20AC+0x1F600+0xFFFD+0x131&&before==0x78&&s.byte_at(0)==0x61;\n"
"}";
REQUIRE(fixture.run(source.c_str(),"bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
CHECK(error_of(fixture,"uint c=\"\xC5\x9F\".code_at(1);")=="index out of range");
CHECK(error_of(fixture,"uint c=\"\".code_at(0);")=="index out of range");
CHECK(error_of(fixture,"uint8 b=\"a\".byte_at(-1);")=="index out of range");
}

TEST_CASE("code_at and byte_at are read as values and cannot be assigned through")
{
// Declared const uint &code_at(int) const and const uint8 &byte_at(int) const (spec 8.1): locals, auto locals and
// expressions hold copies, so changing a copy, the string or another character never changes what was read.
ScriptFixture fixture;
const char* source="bool main(){\n"
"string s=\"a\xC5\x9F" "b\";\n"
"uint c=s.code_at(1);\n"
"auto ac=s.code_at(1);\n"
"uint8 b=s.byte_at(0);\n"
"auto ab=s.byte_at(0);\n"
"c+=1;ac+=2;b+=1;ab+=2;\n"
"if(s.code_at(1)!=0x15F||s.byte_at(0)!=0x61)return false;\n"
"uint kept=s.code_at(0);uint8 kept_byte=s.byte_at(3);\n"
"s=\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\";\n"
"if(kept!=0x61||kept_byte!=0x62)return false;\n"
"uint other=s.code_at(0);\n"
"return c==0x160&&ac==0x161&&b==0x62&&ab==0x63&&other==0x7A&&kept==0x61;\n"
"}";
REQUIRE(fixture.run(source,"bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
CHECK(compiles(fixture,"string s=\"ab\";uint c=s.code_at(0);uint8 b=s.byte_at(1);"));
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s.code_at(0)=5;"));
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s.byte_at(0)=5;"));
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s.byte_at(0)+=1;"));
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s.code_at(0)++;"));
// s[i] is read-only too (a get-only index accessor): assigning to it does not compile, reading it does, in expressions,
// as an argument, on a const string and on a temporary.
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s[0]=\"x\";"));
CHECK_FALSE(compiles(fixture,"string s=\"ab\";s[0]+=\"x\";"));
CHECK(compiles(fixture,"string s=\"ab\";string c=s[1];bool b=s[0]==\"a\";string d=s[0]+s[1];"));
// No implicit conversion from a number to a string, as an argument or in a comparison.
CHECK(compiles(fixture,"f(\"5\");}void f(string s){"));
CHECK_FALSE(compiles(fixture,"f(5);}void f(string s){"));
CHECK_FALSE(compiles(fixture,"bool b=\"a\"==5;"));
const char* reads="string first(const string &in s){return s;}\n"
"bool main(){\n"
"const string s=\"a\xC5\x9F" "b\";\n"
"string c=s[1];\n"
"int i=2;\n"
"return c==\"\xC5\x9F\"&&first(s[i])==\"b\"&&(s[0]+s[2])==\"ab\"&&(s+\"!\")[3]==\"!\"&&s[1].length()==1;\n"
"}";
REQUIRE(fixture.run(reads,"bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
}

TEST_CASE("byte_at and code_at of temporary strings read the right text")
{
// byte_at refers into the string, so the engine must keep a temporary alive until the script has read the byte. The two
// temporaries are heap strings of one block size class: had the first been freed before its byte was read, the second
// would reuse its block (through the per-thread cache) and the first read would see the second's text.
ScriptFixture fixture;
const char* source="const string head_a=\"a fairly long text that has to live on the heap\";\n"
"const string head_b=\"b fairly long text that has to live on the heap\";\n"
"bool main(){\n"
"uint8 x=(head_a+\"!\").byte_at(0);\n"
"if(x!=0x61)return false;\n"
"if((head_a+\"!\").byte_at(0)==(head_b+\"!\").byte_at(0))return false;\n"
"if(uint((head_a+\"!\").byte_at(0))+uint((head_b+\"!\").byte_at(0))!=0x61+0x62)return false;\n"
"if((head_a+\"\xC5\x9F\").code_at(47)!=0x15F||(head_b+\"!\").code_at(0)!=0x62)return false;\n"
"return (head_a+\"!\").byte_at(47)==0x21&&(head_b+\"?\").byte_at(47)==0x3F;\n"
"}";
REQUIRE(fixture.run(source,"bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
}

TEST_CASE("search returns character indices")
{
ScriptFixture fixture;
CHECK(holds(fixture,"\"\xC5\x9F" "ehir\".find(\"ir\")==3"));
CHECK(holds(fixture,"\"banana\".find(\"an\",2)==3"));
CHECK(holds(fixture,"\"banana\".find(\"x\")==-1"));
CHECK(holds(fixture,"\"banana\".rfind(\"an\")==3"));
CHECK(holds(fixture,"\"banana\".rfind(\"an\",2)==1"));
CHECK(holds(fixture,"\"banana\".contains(\"nan\")&&!\"banana\".contains(\"x\")"));
CHECK(holds(fixture,"\"banana\".starts_with(\"ba\")&&\"banana\".ends_with(\"na\")"));
CHECK(holds(fixture,"\"banana\".count(\"a\")==3"));
CHECK(error_of(fixture,"int i=\"abc\".find(\"a\",4);")=="index out of range");
}

TEST_CASE("parts clamp counts and reject bad starts")
{
ScriptFixture fixture;
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".substr(1,2)")=="eh");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".substr(3,100)")=="ir");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".substr(2)")=="hir");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".left(100)")=="\xC5\x9F" "ehir");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".left(1)")=="\xC5\x9F");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".right(2)")=="ir");
CHECK(error_of(fixture,"string s=\"abc\".substr(4);")=="index out of range");
CHECK(error_of(fixture,"string s=\"abc\".substr(-1);")=="index out of range");
CHECK(error_of(fixture,"string s=\"abc\".substr(0,-2);")=="negative count");
CHECK(error_of(fixture,"string s=\"abc\".left(-1);")=="negative count");
CHECK(holds(fixture,"\"a,b,,c\".split(\",\").length()==4"));
CHECK(holds(fixture,"\"a,b,,c\".split(\",\",false).length()==3"));
CHECK(value_of(fixture,"join(\"a,b,,c\".split(\",\"),\"-\")")=="a-b--c");
CHECK(value_of(fixture,"join(\"\xC5\x9F" "ey\".split(\"\"),\"|\")")=="\xC5\x9F|e|y");
CHECK(value_of(fixture,"join(array<string>(),\"-\")")=="");
}

TEST_CASE("changes return new strings")
{
ScriptFixture fixture;
CHECK(value_of(fixture,"\"a-b-c\".replace(\"-\",\"+\")")=="a+b+c");
CHECK(value_of(fixture,"\"a-b-c\".replace(\"-\",\"+\",false)")=="a+b-c");
CHECK(value_of(fixture,"\"  a b \\t\".trim()")=="a b");
CHECK(value_of(fixture,"\"  a \".trim_left()")=="a ");
CHECK(value_of(fixture,"\"  a \".trim_right()")=="  a");
CHECK(value_of(fixture,"\"\xC5\x9E" "EH\xC4\xB0R\".lower()")=="\xC5\x9F" "ehir");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".upper()")=="\xC5\x9E" "EHIR");
CHECK(value_of(fixture,"\"ab\".repeat(3)")=="ababab");
CHECK(error_of(fixture,"string s=\"ab\".repeat(-1);")=="negative count");
CHECK(value_of(fixture,"\"\xC5\x9F" "ey\".reverse()")=="ye\xC5\x9F");
CHECK(value_of(fixture,"\"\xC5\x9F" "ey\".insert(1,\"x\")")=="\xC5\x9F" "xey");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".erase(1,2)")=="\xC5\x9F" "ir");
CHECK(value_of(fixture,"\"\xC5\x9F" "ehir\".erase(2)")=="\xC5\x9F" "e");
CHECK(error_of(fixture,"string s=\"abc\".insert(4,\"x\");")=="index out of range");
CHECK(value_of(fixture,"\"s\xCC\xA7\".normalize()")=="\xC5\x9F");
}

TEST_CASE("Turkish case mapping at script level")
{
ScriptFixture fixture;
CHECK(value_of(fixture,"\"\xC4\xB0" "stanbul'da I\xC5\x9E" "IK\".lower(\"tr\")")=="istanbul'da \xC4\xB1\xC5\x9F\xC4\xB1" "k");
CHECK(value_of(fixture,"\"istanbul \xC4\xB1\xC5\x9F\xC4\xB1" "k\".upper(\"tr\")")=="\xC4\xB0" "STANBUL I\xC5\x9E" "IK");
CHECK(value_of(fixture,"\"I\".lower()")=="i");
CHECK(value_of(fixture,"\"\xC4\xB0" "STANBUL\".lower(\"tr\")")=="istanbul");
}

TEST_CASE("numbers convert both ways")
{
ScriptFixture fixture;
CHECK(holds(fixture,"\"3.5\".to_number()==3.5"));
CHECK(holds(fixture,"\"-42\".to_int()==-42"));
CHECK(holds(fixture,"\"12\".is_number()&&!\"1x\".is_number()"));
CHECK(holds(fixture,"\"0123\".is_digits()&&!\"-1\".is_digits()"));
CHECK(error_of(fixture,"double d=\"abc\".to_number();")=="\"abc\" is not a number");
CHECK(error_of(fixture,"int64 i=\"1.2.3\".to_int();")=="\"1.2.3\" is not a number");
CHECK(error_of(fixture,"double d=\"a\\nb\".to_number();")=="\"a\\nb\" is not a number");
CHECK(error_of(fixture,"double d=\"x\".repeat(70).to_number();")=="\""+std::string(64,'x')+"...\" is not a number");
CHECK(error_of(fixture,"double d=\"x\".repeat(64).to_number();")=="\""+std::string(64,'x')+"\" is not a number");
CHECK(value_of(fixture,"string(5)")=="5");
CHECK(value_of(fixture,"string(int8(-3))+string(int16(-300))+string(int64(5000000000))")=="-3-3005000000000");
CHECK(value_of(fixture,"string(uint8(255))+string(uint16(65535))+string(uint(4000000000))+string(uint64(18446744073709551615))")=="255655354000000000" "18446744073709551615");
CHECK(value_of(fixture,"string(1.5)+\" \"+string(0.1)+\" \"+string(100.0)")=="1.5 0.1 100");
CHECK(value_of(fixture,"string(0.1f)")=="0.1");
CHECK(value_of(fixture,"string(true)+string(false)")=="truefalse");
CHECK(holds(fixture,"\"abc\".hash()==\"abc\".hash()&&\"abc\".hash()!=\"abd\".hash()"));
REQUIRE(fixture.run("uint64 main(){return \"abc\".hash();}","uint64 main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnQWord()==zodiac::String("abc").hash());
}

TEST_CASE("concatenation with numbers and bools both ways")
{
ScriptFixture fixture;
CHECK(value_of(fixture,"\"a\"+5+\"b\"")=="a5b");
CHECK(value_of(fixture,"5+\"a\"")=="5a");
CHECK(value_of(fixture,"\"x\"+1.5+\"|\"+0.1f+\"|\"+true")=="x1.5|0.1|true");
CHECK(value_of(fixture,"false+\"x\"+uint8(7)+int64(-8)+uint64(9)")=="falsex7-89");
CHECK(value_of(fixture,"2.5+\"x\"+0.25f")=="2.5x0.25");
CHECK(value_of(fixture,"\"n\"+uint(3)+int16(-2)")=="n3-2");
REQUIRE(fixture.run("enum Color{red=2} string main(){return \"c\"+red;}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="c2");
CHECK(value_of(fixture,"\"\"+\"\xC5\x9F\"+\"e\"")=="\xC5\x9F" "e");
REQUIRE(fixture.run("string main(){string s=\"a\";s+=5;s+=\" \";s+=0.5f;s+=true;s+=s;return s;}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="a5 0.5truea5 0.5true");
REQUIRE(fixture.run("string main(){string s;s=7;string t;t=2.5;string u;u=false;return s+t+u;}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="72.5false");
}

TEST_CASE("string + string gives the right bytes and count at every size around the inline limit")
{
ScriptFixture fixture;
// Every pair of sizes from 0 to 26 (inline and heap operands, inline and heap results): the bytes, the byte length and the character
// count (the count of a result is carried over from the operands, so a wrong one shows here), for ASCII and for Turkish text.
REQUIRE(fixture.run("bool main(){string low=\"abcdefghijklmnopqrstuvwxyz\";string high=\"ABCDEFGHIJKLMNOPQRSTUVWXYZ\";"
"for(int i=0;i<=26;++i)for(int j=0;j<=26;++j){string a=low.substr(0,i);string b=high.substr(0,j);string c=a+b;"
"if(c.byte_length()!=i+j||c.length()!=i+j||c.substr(0,i)!=a||c.substr(i)!=b)return false;"
"for(int k=0;k<i+j;++k)if(c.byte_at(k)!=(k<i?a.byte_at(k):b.byte_at(k-i)))return false;}"
"for(int i=0;i<=11;++i)for(int j=0;j<=11;++j){string a=\"\xC5\x9F\".repeat(i);string b=\"\xC4\xB1\".repeat(j);string c=a+b;"
"if(c.byte_length()!=2*(i+j)||c.length()!=i+j||c.substr(0,i)!=a||c.substr(i)!=b)return false;}"
"return true;}","bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
// A lead byte at a's end completed by b's first byte: one character across the join, so the count is not the sum (3, not 4); an empty
// a before such a b; a heap operand holding short text (assigned over long text); a result that just fits inline and one that does not.
CHECK(holds(fixture,"(\"x\\xC5\"+\"\\x9Fy\").length()==3&&(\"x\\xC5\"+\"\\x9Fy\")==\"x\xC5\x9Fy\""));
CHECK(holds(fixture,"(\"\"+\"\\x9Fy\").length()==2&&(\"\\x9Fy\"+\"\").length()==2&&(\"\"+\"\").empty()"));
REQUIRE(fixture.run("bool main(){string h=\"a text long enough to live on the heap\";h=\"ab\";string c=h+\"cd\";string d=\"cd\"+h;"
"string e=\"0123456789012\"+\"3456789012\";string f=\"0123456789012\"+\"34567890123\";"
"return c==\"abcd\"&&c.length()==4&&d==\"cdab\"&&e.length()==23&&e==\"01234567890123456789012\"&&f.length()==24&&f.byte_at(23)==0x33;}","bool main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
}

TEST_CASE("comparison is byte order and works with the array")
{
ScriptFixture fixture;
CHECK(holds(fixture,"\"a\"==\"a\"&&\"a\"!=\"b\""));
CHECK(holds(fixture,"\"a\"<\"b\"&&\"b\">\"a\"&&\"a\"<=\"a\"&&\"a\">=\"a\""));
CHECK(holds(fixture,"\"z\"<\"\xC4\xB1\""));
CHECK(holds(fixture,"\"ab\"<\"abc\""));
REQUIRE(fixture.run("string main(){array<string> a={\"c\",\"a\",\"b\"};a.sort();a.insert_last(\"\xC5\x9F\");return join(a,\",\")+a.find(\"b\");}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="a,b,c,\xC5\x9F" "1");
}

TEST_CASE("assignment and copies keep values independent")
{
ScriptFixture fixture;
REQUIRE(fixture.run("string main(){string a=\"a string long enough to live on the heap\";string b=a;b+=\"!\";string c;c=b;c[0];return a+\"|\"+b+\"|\"+c;}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="a string long enough to live on the heap|a string long enough to live on the heap!|a string long enough to live on the heap!");
CHECK(holds(fixture,"string(\"abc\")==\"abc\""));
}

TEST_CASE("format fills {} placeholders")
{
ScriptFixture fixture;
CHECK(holds(fixture,"format(\"{} ve {}\",1,\"iki\")==\"1 ve iki\""));
CHECK(value_of(fixture,"format(\"{{}} {}\",5)")=="{} 5");
CHECK(value_of(fixture,"format(\"{} {}\",1)")=="1 {}");
CHECK(value_of(fixture,"format(\"{}\")")=="{}");
CHECK(value_of(fixture,"format(\"{ } }} {{\")")=="{ } } {");
CHECK(value_of(fixture,"format(\"{}|{}|{}|{}|{}\",0.1f,2.5,true,int8(-1),uint64(18446744073709551615))")=="0.1|2.5|true|-1|18446744073709551615");
REQUIRE(fixture.run("enum Color{green=3} string main(){string s=\"\xC5\x9F\";return format(\"{}{}\",green,s);}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="3\xC5\x9F");
CHECK(value_of(fixture,"format(\"{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}\",1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6)")=="1234567890123456");
}

TEST_CASE("C++ failures become script exceptions")
{
ScriptFixture fixture;
CHECK(error_of(fixture,"string s=\"ab\".repeat(1500000000);")=="out of memory");
REQUIRE(fixture.run("string main(){try{string s=\"ab\".repeat(1500000000);}catch{return exception_info();}return \"no\";}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="out of memory");
}

TEST_CASE("throw, exception_info and getExceptionInfo")
{
ScriptFixture fixture;
CHECK(error_of(fixture,"throw(\"boom\");")=="boom");
REQUIRE(fixture.run("string main(){try{throw(\"g\xC3\xBC\xC3\xA7\");}catch{return exception_info()+\"|\"+getExceptionInfo();}return \"no\";}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="g\xC3\xBC\xC3\xA7|g\xC3\xBC\xC3\xA7");
REQUIRE(fixture.run("string main(){try{string s=\"a\"[5];}catch{return getExceptionInfo();}return \"no\";}","string main()")==asEXECUTION_FINISHED);
CHECK(fixture.returned_string()=="index out of range");
}

TEST_CASE("string constants are shared by concurrent contexts")
{
ScriptFixture fixture;
REQUIRE(fixture.run("bool main(){for(int i=0;i<200;++i){string s=\"\xC5\x9F" "ehir merkezi \"+i;if(\"\xC5\x9F" "ehir\".length()!=5||\"\xC5\x9F" "ehir\"[1]!=\"e\"||s.length()<14||\"bir uzun sabit metin, k\xC3\xBC\xC3\xA7\xC3\xBCk de\xC4\x9Fil\".hash()==0)return false;}return true;}","bool main()")==asEXECUTION_FINISHED);
asIScriptFunction* entry=fixture.engine->GetModule("test")->GetFunctionByDecl("bool main()");
std::atomic<int> successes=0;
std::vector<std::thread> threads;
for(int worker=0;worker<4;++worker){
threads.emplace_back([&]{
asIScriptContext* context=fixture.engine->CreateContext();
bool ok=context!=nullptr;
for(int round=0;ok&&round<25;++round)ok=context->Prepare(entry)>=0&&context->Execute()==asEXECUTION_FINISHED&&context->GetReturnByte()==1;
if(context)context->Release();
asThreadCleanup();
if(ok)++successes;
});
}
for(std::thread& thread:threads)thread.join();
CHECK(successes==4);
}

TEST_CASE("plugin strings with NUL bytes and Turkish text round-trip exactly")
{
ScriptFixture fixture;
g_abi=fixture.abi();
REQUIRE(g_abi->register_function(fixture.abi(),"string bytes_out()",bytes_out,nullptr)==ZODIAC_OK);
REQUIRE(g_abi->register_function(fixture.abi(),"string bytes_echo(const string &in)",bytes_echo,nullptr)==ZODIAC_OK);
REQUIRE(g_abi->register_function(fixture.abi(),"array<string>@ bytes_list()",bytes_list,nullptr)==ZODIAC_OK);
REQUIRE(g_abi->register_function(fixture.abi(),"string bytes_join(array<string>@)",bytes_join,nullptr)==ZODIAC_OK);
g_seen.clear();
g_terminated=false;
CHECK(holds(fixture,"bytes_out().byte_length()==5&&bytes_out().length()==4&&bytes_out().byte_at(1)==0&&bytes_out()[2]==\"\xC5\x9F\"&&bytes_out()==\"a\\x00\xC5\x9F" "b\""));
CHECK(holds(fixture,"bytes_echo(bytes_out()+\"\xC4\xB0\")==bytes_out()+\"\xC4\xB0\""));
CHECK(g_seen==std::string(g_plugin_bytes)+"\xC4\xB0");
CHECK(g_terminated);
CHECK(holds(fixture,"bytes_list().length()==2&&bytes_list()[0]==\"\\x00\xC4\xB0\"&&bytes_list()[1]==bytes_out()"));
CHECK(value_of(fixture,"bytes_join(bytes_list())")==std::string("\0\xC4\xB0",3)+std::string(g_plugin_bytes));
CHECK(fixture.logs.empty());
}
