// Tests Zodiac's script dictionary (zodiac::Dictionary registered by register_dictionary, spec 8.3): the style-C API only,
// set/get/exists/remove with every kind of value, insertion order of keys() after removals and re-inserts, the d["k"] value
// proxy and its conversions, dictionary literals, 100 000 keys, the serialize() text format and its round trip (Turkish keys,
// values with newlines and quotes, nested dictionaries and arrays), malformed input that leaves the dictionary untouched, the
// "cannot serialize" errors, script callbacks that change the dictionary while it releases or copies values, and garbage
// collection of handle cycles through dictionaries (Review Focus 4).
#include "script_fixture.hpp"
#include <core/types/dictionary.hpp>
#include <core/types/string.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using zodiac_test::ScriptFixture;

namespace zodiac {
// Reads a dictionary's index (declared a friend by Dictionary): how far its keys sit from their home slots.
struct DictionaryTestAccess {
struct Displacement {
std::uint32_t largest=0;
std::uint64_t total=0;
};
static Displacement displacement(const Dictionary& dictionary)
{
Displacement result;
if(!dictionary.index_)return result;
for(std::uint32_t at=0;at<=dictionary.mask_;++at){
const Dictionary::Slot& slot=dictionary.index_[at];
if(!slot.entry)continue;
const std::uint32_t distance=(at-dictionary.home(slot.hash))&dictionary.mask_;
result.largest=std::max(result.largest,distance);
result.total+=distance;
}
return result;
}
static std::uint32_t index_size(const Dictionary& dictionary)
{
return dictionary.index_?dictionary.mask_+1:0;
}
};
}

namespace {
// Script helpers every test source starts with: show() writes the keys in order separated by commas.
constexpr std::string_view prelude=
"class Item{int v;Item(){v=0;}Item(int x){v=x;}}\n"
"string show(const dictionary&in d){array<string>@ k=d.keys();string s;for(int i=0;i<k.length();++i){if(i>0)s+=\",\";s+=k[i];}return s;}\n";
// Runs "string main(){<body>}" after the prelude and returns the bytes of the result.
std::string value_of(ScriptFixture& fixture,std::string_view body,std::string_view globals={})
{
const std::string source=std::string(prelude)+std::string(globals)+"\nstring main(){"+std::string(body)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"string main()")==asEXECUTION_FINISHED);
return fixture.returned_string();
}
// Runs "void main(){<statements>}" after the prelude, which must raise a script exception, and returns its text.
std::string error_of(ScriptFixture& fixture,std::string_view statements)
{
const std::string source=std::string(prelude)+"void main(){"+std::string(statements)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"void main()")==asEXECUTION_EXCEPTION);
return fixture.exception_string();
}
// True when "void main(){<statements>}" after the prelude compiles.
bool compiles(ScriptFixture& fixture,std::string_view statements)
{
asIScriptModule* module=fixture.engine->GetModule("test",asGM_ALWAYS_CREATE);
REQUIRE(module!=nullptr);
const std::string source=std::string(prelude)+"void main(){"+std::string(statements)+"}";
REQUIRE(module->AddScriptSection("test",source.c_str())>=0);
fixture.messages.clear();
return module->Build()>=0;
}

// Texts deserialize() must refuse, served to scripts by sample(i).
std::vector<std::string> malformed_samples()
{
const std::string header="zodiac-dictionary 1\n";
std::vector<std::string> samples={
"",
"\n",
"zodiac-dictionary 2\n",
"zodiac-dictionary 1 \n",
"zodiac-dictionary\n",
header+"\n",
header+"int64 \"a\"\n",
header+"int64 \"a\" \n",
header+"int64 \"a\" 12x\n",
header+"int64 \"a\" +5\n",
header+"int64 \"a\" -\n",
header+"int64 \"a\" 99999999999999999999\n",
header+"int64 \"a\" 1 \n",
header+"int64  \"a\" 1\n",
header+"int64 a 1\n",
header+"int64 \"a 1\n",
header+"double \"a\" 1.5e\n",
header+"double \"a\" .\n",
header+"double \"a\" infinity\n",
header+"bool \"a\" yes\n",
header+"bool \"a\" True\n",
header+"string \"a\" \"unterminated\n",
header+"string \"a\" \"bad \\q escape\"\n",
header+"string \"a\" \"short \\x4\"\n",
header+"string \"a\" \"raw\ttab\"\n",
header+"string \"a\" \"x\"y\n",
header+"string \"a\" x\n",
header+"none \"a\" 1\n",
header+"float \"a\" 1\n",
header+"Int64 \"a\" 1\n",
header+"int64 \"a\" 1\nint64 \"a\" 2\n",
header+"dictionary \"a\" 2\nint64 \"x\" 1\n",
header+"dictionary \"a\" -1\n",
header+"dictionary \"a\" 2\nint64 \"x\" 1\nint64 \"x\" 2\n",
header+"dictionary \"a\" nul\n",
header+"array<int8> \"a\" 1\n300\n",
header+"array<uint8> \"a\" 1\n-1\n",
header+"array<int8> \"a\" 1\n-129\n",
header+"array<int16> \"a\" 1\n32768\n",
header+"array<int> \"a\" 1\n2147483648\n",
header+"array<uint16> \"a\" 1\n65536\n",
header+"array<uint> \"a\" 1\n4294967296\n",
header+"array<uint64> \"a\" 1\n18446744073709551616\n",
header+"array<float> \"a\" 1\n3.4028236e+38\n",
header+"array<float> \"a\" 1\n-1e39\n",
header+"array<int> \"a\" 2\n1\n",
header+"array<int> \"a\" 1\n1\nint64 \"b\" 2 x\n",
header+"array<Item> \"a\" 0\n",
header+"array<array<int>> \"a\" 0\n",
header+"array<string> \"a\" 1\nx\n",
header+"array<dictionary@> \"a\" 1\n1\nbool \"x\" maybe\n",
header+"array<dictionary> \"a\" 1\nnull\n",
header+"array<bool> \"a\" 1000000000\ntrue\n",
header+"int64 \"ok\" 1\nbroken\n",
};
// Nesting deeper than the format allows.
std::string deep=header;
for(int level=0;level<200;++level)deep+="dictionary \"a\" 1\n";
deep+="none \"z\"\n";
samples.push_back(deep);
return samples;
}
const std::vector<std::string>& samples()
{
static const std::vector<std::string> all=malformed_samples();
return all;
}
zodiac::String sample(int index)
{
return zodiac::String(samples().at(static_cast<std::size_t>(index)));
}
int sample_count()
{
return static_cast<int>(samples().size());
}

int g_destroyed=0;
void node_destroyed()
{
++g_destroyed;
}
}

TEST_CASE("dictionary is Zodiac's dictionary with style-C names only")
{
ScriptFixture fixture;
CHECK(compiles(fixture,"dictionary d;d.set(\"a\",1);int64 i;d.get(\"a\",i);bool e=d.exists(\"a\");bool r=d.remove(\"a\");array<string>@ k=d.keys();int n=d.length();bool m=d.empty();d.clear();string s=d.serialize();bool ok=d.deserialize(s);dictionary_value v=d[\"a\"];dictionary c=d;c=d;"
"bool b1=d.move_by(\"a\",1);bool b2=d.move(\"a\",0);int p=d.index_of(\"a\");string q=d.key_at(0);"));
for(std::string_view call:{"getKeys()","isEmpty()","getSize()","delete(\"a\")","deleteAll()","size()","erase(\"a\")"}){
CAPTURE(call);
CHECK_FALSE(compiles(fixture,"dictionary d;d."+std::string(call)+";"));
}
CHECK_FALSE(compiles(fixture,"dictionaryValue v;"));
asITypeInfo* type=fixture.engine->GetTypeInfoByName("dictionary");
REQUIRE(type!=nullptr);
CHECK((type->GetFlags()&asOBJ_GC)!=0);
}

TEST_CASE("set, get, exists and remove with every kind of value")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d;d.set(\"i\",42);d.set(\"f\",1.5);d.set(\"b\",true);d.set(\"s\",\"\xC5\x9F" "ehir\");Item it(7);d.set(\"obj\",it);d.set(\"h\",@it);\n"
"int64 i;double f;bool b;string s;Item o;Item@ h;\n"
"bool all=d.get(\"i\",i)&&d.get(\"f\",f)&&d.get(\"b\",b)&&d.get(\"s\",s)&&d.get(\"obj\",o)&&d.get(\"h\",@h);\n"
"it.v=8;Item o2;d.get(\"obj\",o2);\n"
"string r=all+\",\"+i+\",\"+f+\",\"+b+\",\"+s+\",\"+o.v+\",\"+o2.v+\",\"+h.v+\",\"+(h is it);\n"
"int64 missing=5;r+=\"|\"+d.get(\"nope\",missing)+\",\"+missing;\n"
"string wrong=\"x\";r+=\",\"+d.get(\"i\",wrong)+\",\"+wrong;\n"
"Item@ none;r+=\",\"+d.get(\"i\",@none)+\",\"+(none is null);\n"
"r+=\"|\"+d.exists(\"s\")+\",\"+d.remove(\"s\")+\",\"+d.exists(\"s\")+\",\"+d.remove(\"s\")+\",\"+d.length()+\",\"+d.empty();\n"
"d.clear();r+=\",\"+d.length()+\",\"+d.empty()+\",\"+d.exists(\"i\");\n"
"return r;")
=="true,42,1.5,true,\xC5\x9F" "ehir,7,7,8,true|false,5,false,,false,true|true,true,false,false,5,false,0,true,false");
// Numbers convert to every numeric type and bool; integers are kept as int64, so large values survive.
CHECK(value_of(fixture,
"dictionary d;d.set(\"i\",42);d.set(\"f\",-2.75);d.set(\"b\",true);d.set(\"big\",int64(9007199254740993));d.set(\"u\",uint64(18446744073709551615));\n"
"int small;double wide;int trunc;bool nonzero;int8 wrapped;float narrow;uint64 u;int64 big;int from_bool;\n"
"d.get(\"i\",small);d.get(\"i\",wide);d.get(\"f\",trunc);d.get(\"i\",nonzero);d.set(\"w\",300);d.get(\"w\",wrapped);d.get(\"f\",narrow);d.get(\"u\",u);d.get(\"big\",big);d.get(\"b\",from_bool);\n"
"return small+\",\"+wide+\",\"+trunc+\",\"+nonzero+\",\"+wrapped+\",\"+narrow+\",\"+u+\",\"+big+\",\"+from_bool;")
=="42,42,-2,true,44,-2.75,18446744073709551615,9007199254740993,1");
// A setting replaces the value in place. (A failed get() writes nothing, but AngelScript resets an object &out argument.)
CHECK(value_of(fixture,"dictionary d;d.set(\"k\",\"text\");d.set(\"k\",5);int64 v;string s=\"kept\";return d.get(\"k\",v)+\",\"+v+\",\"+d.get(\"k\",s)+\",\"+s.length()+\",\"+d.length();")=="true,5,false,0,1");
}

TEST_CASE("keys() lists keys in insertion order after removals and re-inserts")
{
ScriptFixture fixture;
CHECK(value_of(fixture,"dictionary d;d.set(\"a\",1);d.set(\"b\",2);d.set(\"c\",3);d.set(\"d\",4);d.remove(\"b\");d.set(\"b\",5);d.set(\"a\",6);d.remove(\"d\");d.set(\"e\",7);int64 a;d.get(\"a\",a);return show(d)+\"|\"+a;")=="a,c,b,e|6");
std::string expected;
for(int i=1;i<100;i+=2)expected+="k"+std::to_string(i)+",";
for(int i=100;i<150;++i)expected+="k"+std::to_string(i)+",";
expected+="k0";
CHECK(value_of(fixture,"dictionary d;for(int i=0;i<100;++i)d.set(\"k\"+i,i);for(int i=0;i<100;i+=2)d.remove(\"k\"+i);for(int i=100;i<150;++i)d.set(\"k\"+i,i);d.set(\"k0\",0);return show(d);")==expected);
CHECK(value_of(fixture,"dictionary d;for(int i=0;i<40;++i)d.set(\"k\"+i,i);d.clear();d.set(\"x\",1);d.set(\"y\",2);d.remove(\"x\");d.set(\"x\",3);return show(d)+\"|\"+d.length();")=="y,x|2");
// Long keys (heap text, over String's 23 inline bytes) and Turkish ones come back whole and in order; an empty dictionary,
// a cleared one and one emptied by removes give an empty array.
const std::string long_key(300,'k');
CHECK(value_of(fixture,"dictionary d;string a=\""+long_key+"\";string b=a+\"\xC5\x9F\";d.set(a,1);d.set(\"short\",2);d.set(b,3);d.set(\"\xC4\xB1\xC5\x9F\xC4\xB1k\",4);"
"array<string>@ k=d.keys();return k.length()+\",\"+(k[0]==a)+\",\"+k[1]+\",\"+(k[2]==b)+\",\"+k[2].byte_length()+\",\"+k[3];")
=="4,true,short,true,302,\xC4\xB1\xC5\x9F\xC4\xB1k");
CHECK(value_of(fixture,"dictionary d;array<string>@ k=d.keys();dictionary e;e.set(\"x\",1);e.clear();array<string>@ c=e.keys();"
"dictionary f;f.set(\"y\",1);f.remove(\"y\");array<string>@ r=f.keys();k.insert_last(\"z\");return k.length()+\",\"+c.length()+\",\"+r.length()+\",\"+k[0];")
=="1,0,0,z");
// keys() is a copy.
CHECK(value_of(fixture,"dictionary d;d.set(\"a\",1);array<string>@ k=d.keys();d.set(\"b\",2);k.insert_last(\"z\");return show(d)+\"|\"+k.length();")=="a,b|2");
}

TEST_CASE("move_by moves a key by places, by one at either end, and refuses every impossible move")
{
ScriptFixture fixture;
const std::string start="dictionary d;d.set(\"a\",1);d.set(\"b\",2);d.set(\"c\",3);d.set(\"d\",4);d.set(\"e\",5);string r;\n";
// By one, at the ends and back.
CHECK(value_of(fixture,start+"r+=d.move_by(\"a\",-1)+\":\"+show(d);r+=\"|\"+d.move_by(\"e\",1)+\":\"+show(d);"
"r+=\"|\"+d.move_by(\"a\",1)+\":\"+show(d);r+=\"|\"+d.move_by(\"a\",-1)+\":\"+show(d);"
"r+=\"|\"+d.move_by(\"e\",-1)+\":\"+show(d);r+=\"|\"+d.move_by(\"e\",1)+\":\"+show(d);return r;")
=="false:a,b,c,d,e|false:a,b,c,d,e|true:b,a,c,d,e|true:a,b,c,d,e|true:a,b,c,e,d|true:a,b,c,d,e");
// Long moves, exactly to either end and one place past it.
CHECK(value_of(fixture,start+"r+=d.move_by(\"a\",4)+\":\"+show(d);r+=\"|\"+d.move_by(\"a\",-4)+\":\"+show(d);"
"r+=\"|\"+d.move_by(\"a\",5)+\":\"+show(d);r+=\"|\"+d.move_by(\"c\",-3)+\":\"+show(d);r+=\"|\"+d.move_by(\"c\",3)+\":\"+show(d);"
"r+=\"|\"+d.move_by(\"c\",-2)+\":\"+show(d);r+=\"|\"+d.move_by(\"b\",2)+\":\"+show(d);return r;")
=="true:b,c,d,e,a|true:a,b,c,d,e|false:a,b,c,d,e|false:a,b,c,d,e|false:a,b,c,d,e|true:c,a,b,d,e|true:c,a,d,e,b");
// A missing key, a zero delta, the int limits and a one-key or empty dictionary change nothing.
CHECK(value_of(fixture,start+"r+=d.move_by(\"x\",1)+\",\"+d.move_by(\"c\",0)+\",\"+d.move_by(\"c\",2147483647)+\",\"+d.move_by(\"c\",int(0x80000000))+\":\"+show(d);"
"dictionary one;one.set(\"k\",1);r+=\"|\"+one.move_by(\"k\",1)+\",\"+one.move_by(\"k\",-1)+\",\"+one.move(\"k\",0)+\":\"+show(one);"
"dictionary none;r+=\"|\"+none.move_by(\"k\",1)+\",\"+none.move(\"k\",0)+\",\"+none.index_of(\"k\");return r;")
=="false,false,false,false:a,b,c,d,e|false,false,true:k|false,false,-1");
}

TEST_CASE("move puts a key at an index, and index_of and key_at follow moves, removes and re-adds")
{
ScriptFixture fixture;
const std::string start="dictionary d;d.set(\"a\",1);d.set(\"b\",2);d.set(\"c\",3);d.set(\"d\",4);d.set(\"e\",5);string r;\n";
CHECK(value_of(fixture,start+"r+=d.move(\"a\",4)+\":\"+show(d);r+=\"|\"+d.move(\"a\",0)+\":\"+show(d);r+=\"|\"+d.move(\"d\",1)+\":\"+show(d);"
"r+=\"|\"+d.move(\"b\",3)+\":\"+show(d);r+=\"|\"+d.move(\"c\",3)+\":\"+show(d);r+=\"|\"+d.move(\"e\",4)+\":\"+show(d);"
"r+=\"|\"+d.move(\"x\",0)+\",\"+d.move(\"a\",-1)+\",\"+d.move(\"a\",5)+\":\"+show(d);return r;")
=="true:b,c,d,e,a|true:a,b,c,d,e|true:a,d,b,c,e|true:a,d,c,b,e|true:a,d,b,c,e|true:a,d,b,c,e|false,false,false:a,d,b,c,e");
CHECK(value_of(fixture,start+"d.move_by(\"e\",-4);d.remove(\"c\");d.set(\"c\",9);d.move(\"c\",1);d.remove(\"a\");d.set(\"f\",6);"
"for(int i=0;i<d.length();++i){if(i>0)r+=\",\";r+=d.key_at(i)+\"=\"+d.index_of(d.key_at(i));}r+=\"|\"+d.index_of(\"a\")+\":\"+show(d);return r;")
=="e=0,c=1,b=2,d=3,f=4|-1:e,c,b,d,f");
// Against a script model of the order: thousands of random moves, removes and re-adds on a larger dictionary.
CHECK(value_of(fixture,"return run();",
"uint seed=7;\n"
"int rnd(int n){seed=seed*1103515245+12345;return int((seed>>8)%uint(n));}\n"
"string join(const array<string>&in a){string s;for(int i=0;i<a.length();++i){if(i>0)s+=\",\";s+=a[i];}return s;}\n"
"bool consistent(const dictionary&in d,const array<string>&in m){if(show(d)!=join(m)||d.length()!=m.length())return false;"
"for(int i=0;i<m.length();++i){if(d.key_at(i)!=m[i]||d.index_of(m[i])!=i)return false;}return true;}\n"
"string run(){dictionary d;array<string> m;for(int i=0;i<60;++i){d.set(\"k\"+i,i);m.insert_last(\"k\"+i);}\n"
"for(int step=0;step<4000;++step){\n"
"if(m.length()==0||rnd(9)==0){string k=\"n\"+step;d.set(k,step);m.insert_last(k);continue;}\n"
"int at=rnd(m.length());string k=m[at];int kind=rnd(4);\n"
"if(kind==0){int delta=rnd(2)==0?rnd(3)-1:rnd(m.length()*2+1)-m.length();int to=at+delta;bool ok=d.move_by(k,delta);"
"if(ok!=(delta!=0&&to>=0&&to<m.length()))return \"move_by \"+k+\" \"+delta;if(ok){m.remove_at(at);m.insert_at(to,k);}}\n"
"else if(kind==1){int to=rnd(m.length()+2)-1;bool ok=d.move(k,to);if(ok!=(to>=0&&to<m.length()))return \"move \"+k+\" \"+to;if(ok){m.remove_at(at);m.insert_at(to,k);}}\n"
"else if(kind==2){d.remove(k);m.remove_at(at);}\n"
"else{if(d.index_of(k)!=at||d.key_at(at)!=k)return \"index \"+k;}\n"
"if(step%97==0&&!consistent(d,m))return \"step \"+step;}\n"
"return consistent(d,m)?\"ok\":\"end\";}\n")
=="ok");
}

TEST_CASE("key_at and index_of misuse, and the moved order in serialize() and keys()")
{
ScriptFixture fixture;
for(std::string_view call:{"string k=d.key_at(-1);","string k=d.key_at(3);","string k=d.key_at(2147483647);","string k=d.key_at(int(0x80000000));","d.clear();string k=d.key_at(0);"}){
CAPTURE(call);
CHECK(error_of(fixture,"dictionary d;d.set(\"a\",1);d.set(\"b\",2);d.set(\"c\",3);"+std::string(call))=="index out of range");
}
CHECK(value_of(fixture,"dictionary d;d.set(\"a\",1);d.set(\"b\",2);const dictionary@ c=d;return c.index_of(\"b\")+\",\"+c.index_of(\"z\")+\",\"+c.key_at(0);")=="1,-1,a");
// The moved order is the order of serialize()'s lines, of keys() and of a deserialized copy, nested dictionaries included.
CHECK(value_of(fixture,
"dictionary d;d.set(\"a\",1);d.set(\"b\",\"two\");dictionary inner;inner.set(\"x\",1);inner.set(\"y\",2);inner.move_by(\"y\",-1);d.set(\"n\",@inner);d.set(\"c\",3.5);\n"
"d.move(\"c\",0);d.move_by(\"a\",2);string s=d.serialize();dictionary e;bool ok=e.deserialize(s);\n"
"dictionary@ back=cast<dictionary>(e[\"n\"]);\n"
"return ok+\"|\"+show(d)+\"|\"+show(e)+\"|\"+show(back)+\"|\"+e.index_of(\"a\")+\"|\"+(e.serialize()==s)+\"|\"+s;")
=="true|c,b,n,a|c,b,n,a|y,x|3|true|zodiac-dictionary 1\ndouble \"c\" 3.5\nstring \"b\" \"two\"\ndictionary \"n\" 2\nint64 \"y\" 2\nint64 \"x\" 1\nint64 \"a\" 1\n");
}

TEST_CASE("d[key] is a value proxy with conversions")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d;d[\"n\"]=5;d[\"f\"]=2.75;d[\"s\"]=\"text\";d[\"b\"]=true;Item it(3);d[\"o\"]=it;d[\"h\"]=@it;\n"
"int n=int(d[\"n\"]);int64 n64=int64(d[\"n\"]);double nf=double(d[\"n\"]);int fi=int(d[\"f\"]);float ff=float(d[\"f\"]);string s=string(d[\"s\"]);bool b=bool(d[\"b\"]);\n"
"Item@ h=cast<Item>(d[\"h\"]);Item@ oh=cast<Item>(d[\"o\"]);it.v=4;\n"
"string r=n+\",\"+n64+\",\"+nf+\",\"+fi+\",\"+ff+\",\"+s+\",\"+b+\",\"+h.v+\",\"+(h is it)+\",\"+oh.v+\",\"+(oh is it);\n"
"string none=string(d[\"n\"]);r+=\"|\"+none.length();\n"
"int z=int(d[\"new\"]);r+=\",\"+z+\",\"+d.exists(\"new\");\n"
"d[\"copy\"]=d[\"n\"];d[\"hh\"]=d[\"h\"];r+=\",\"+int(d[\"copy\"])+\",\"+(cast<Item>(d[\"hh\"]) is it);\n"
"dictionary_value v=d[\"f\"];d[\"f\"]=1;r+=\",\"+double(v)+\",\"+int(d[\"f\"]);\n"
"d[\"n\"]=\"now text\";r+=\",\"+string(d[\"n\"]);\n"
"return r;")
=="5,5,5,2,2.75,text,true,4,true,3,false|0,0,true,5,true,2.75,1,now text");
CHECK(error_of(fixture,"dictionary d;d.set(\"a\",1);const dictionary@ c=d;int q=int(c[\"nope\"]);")=="key \"nope\" not found");
CHECK(error_of(fixture,"dictionary d;const dictionary@ c=d;int q=int(c[\"line\\nbreak\"]);")=="key \"line\\nbreak\" not found");
CHECK(value_of(fixture,"dictionary d;d.set(\"a\",7);const dictionary@ c=d;return \"\"+int(c[\"a\"]);")=="7");
// dictionary_value as a member and an array element is copied by the engine's own copies.
CHECK(value_of(fixture,"Holder a;a.v=5;Holder b=a;a.v=\"x\";array<dictionary_value> list(2);list[0]=2.5;array<dictionary_value> copy=list;list[0]=1;return int(b.v)+\",\"+string(a.v)+\",\"+double(copy[0])+\",\"+int(list[0]);","class Holder{dictionary_value v;}")=="5,x,2.5,1");
}

TEST_CASE("dictionary literals")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d={{\"a\",1},{\"b\",\"x\"},{\"c\",2.5},{\"d\",true},{\"e\",Item(4)},{\"a\",9}};\n"
"int64 a;string b;double c;bool e;Item o;d.get(\"a\",a);d.get(\"b\",b);d.get(\"c\",c);d.get(\"d\",e);d.get(\"e\",o);\n"
"return show(d)+\"|\"+a+\",\"+b+\",\"+c+\",\"+e+\",\"+o.v;")
=="a,b,c,d,e|9,x,2.5,true,4");
CHECK(value_of(fixture,"dictionary d={};dictionary h={{\"k\",1}};return d.length()+\",\"+h.length();")=="0,1");
}

TEST_CASE("100 000 keys insert, look up, remove and re-insert")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d;for(int i=0;i<100000;++i)d.set(\"key\"+i,i);\n"
"int64 sum=0;for(int i=0;i<100000;++i){int64 v;if(d.get(\"key\"+i,v))sum+=v;}\n"
"bool ok=d.length()==100000&&sum==int64(99999)*100000/2&&!d.exists(\"key100000\");\n"
"for(int i=0;i<100000;i+=2)d.remove(\"key\"+i);\n"
"array<string>@ k=d.keys();ok=ok&&d.length()==50000&&k.length()==50000&&k[0]==\"key1\"&&k[49999]==\"key99999\"&&!d.exists(\"key0\")&&d.exists(\"key1\");\n"
"for(int i=0;i<100000;i+=2)d.set(\"key\"+i,i);\n"
"@k=d.keys();ok=ok&&d.length()==100000&&k[50000]==\"key0\"&&k[99999]==\"key99998\";\n"
"sum=0;for(int i=0;i<100000;++i)sum+=int64(d[\"key\"+i]);\n"
"return ok+\",\"+(sum==int64(99999)*100000/2);")
=="true,true");
}

TEST_CASE("serialize() writes Zodiac's versioned text format")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d;d.set(\"name\",\"Ali \\\"Veli\\\"\\n\xC5\x9F" "ehir\\\\\");d.set(\"score\",42);d.set(\"speed\",1.5);d.set(\"alive\",true);\n"
"dictionary inner;inner.set(\"x\",-1);d.set(\"pos\",inner);array<int> list={1,2};d.set(\"list\",list);\n"
"array<dictionary@> ds={inner,null};d.set(\"ds\",@ds);dictionary@ nothing;d.set(\"gone\",@nothing);d[\"nothing\"];\n"
"return d.serialize();")
=="zodiac-dictionary 1\n"
"string \"name\" \"Ali \\\"Veli\\\"\\n\xC5\x9F" "ehir\\\\\"\n"
"int64 \"score\" 42\n"
"double \"speed\" 1.5\n"
"bool \"alive\" true\n"
"dictionary \"pos\" 1\n"
"int64 \"x\" -1\n"
"array<int> \"list\" 2\n"
"1\n"
"2\n"
"array<dictionary@> \"ds\" 2\n"
"1\n"
"int64 \"x\" -1\n"
"null\n"
"dictionary \"gone\" null\n"
"none \"nothing\"\n");
CHECK(value_of(fixture,"dictionary d;return d.serialize();")=="zodiac-dictionary 1\n");
CHECK(value_of(fixture,"dictionary d;d.set(\"c\",\"\\t\\r\\x01\\x7f\");return d.serialize();")=="zodiac-dictionary 1\nstring \"c\" \"\\t\\r\\x01\\x7f\"\n");
}

TEST_CASE("serialize and deserialize round trip")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary d;\n"
"d.set(\"\xC5\x9F" "ehir ad\xC4\xB1\",\"\xC4\xB0stanbul \\\"merkez\\\"\\nsat\xC4\xB1r 2\\ttab\\\\back\");\n"
"d.set(\"\xC3\xA7ok\",-1234567890123456789);d.set(\"pi\",3.141592653589793);d.set(\"tiny\",5e-324);d.set(\"huge\",1e300*1e300);d.set(\"nan\",(1e300*1e300)-(1e300*1e300));d.set(\"neg0\",-0.0);d.set(\"yok\",false);\n"
"d.set(\"line\\nkey \\\"q\\\"\",\"\");\n"
"dictionary inner;inner.set(\"x\",1);inner.set(\"ad\",\"Ay\xC5\x9F" "e\");d.set(\"inner\",inner);\n"
"array<int> nums={1,-2,2147483647};d.set(\"nums\",nums);array<string> words={\"a\\nb\",\"\xC4\x9F\",\"\"};d.set(\"words\",words);\n"
"array<dictionary@> ds={inner,null};d.set(\"ds\",ds);array<dictionary> dv(2);dv[1].set(\"z\",true);d.set(\"dv\",dv);\n"
"array<double> ws={0.1,2,-1e-7};d.set(\"ws\",ws);array<float> fs={0.1f,3,3.4028235e+38f,-3.4028235e+38f,1.4e-45f};d.set(\"fs\",fs);array<bool> flags={true,false};d.set(\"flags\",flags);\n"
"array<int8> i8={-128,127};d.set(\"i8\",i8);array<int16> i16={-32768,32767};array<int> i32={-2147483647-1,2147483647};d.set(\"i32\",i32);d.set(\"i16\",i16);array<int64> i64={-9223372036854775807-1};d.set(\"i64\",i64);\n"
"array<uint8> u8={0,255};d.set(\"u8\",u8);array<uint16> u16={65535};d.set(\"u16\",u16);array<uint> u32={4294967295};d.set(\"u32\",u32);array<uint64> u64={18446744073709551615};d.set(\"u64\",u64);\n"
"array<int> empty;d.set(\"empty\",empty);array<int>@ nolist;d.set(\"nolist\",@nolist);d[\"none\"];\n"
"string text=d.serialize();\n"
"dictionary e;e.set(\"old\",1);bool ok=e.deserialize(text);\n"
"string r=ok+\",\"+(e.serialize()==text)+\",\"+(show(e)==show(d))+\",\"+e.exists(\"old\");\n"
"string city;e.get(\"\xC5\x9F" "ehir ad\xC4\xB1\",city);r+=\",\"+(city==\"\xC4\xB0stanbul \\\"merkez\\\"\\nsat\xC4\xB1r 2\\ttab\\\\back\");\n"
"int64 big;e.get(\"\xC3\xA7ok\",big);double pi;e.get(\"pi\",pi);double tiny;e.get(\"tiny\",tiny);double huge;e.get(\"huge\",huge);double nan;e.get(\"nan\",nan);double neg0;e.get(\"neg0\",neg0);\n"
"r+=\",\"+big+\",\"+(pi==3.141592653589793)+\",\"+(tiny==5e-324)+\",\"+huge+\",\"+(nan!=nan)+\",\"+(neg0==0&&text.find(\"\\\"neg0\\\" -0\\n\")>=0);\n"
"dictionary@ in2=cast<dictionary>(e[\"inner\"]);string ad;in2.get(\"ad\",ad);r+=\",\"+in2.length()+ad;\n"
"array<int>@ n2=cast<array<int>>(e[\"nums\"]);r+=\",\"+n2.length()+\":\"+n2[2];\n"
"array<string>@ w2=cast<array<string>>(e[\"words\"]);r+=\",\"+(w2[0]==\"a\\nb\")+w2[1]+w2[2].length();\n"
"array<dictionary@>@ d2=cast<array<dictionary@>>(e[\"ds\"]);r+=\",\"+d2.length()+(d2[1] is null)+int(d2[0][\"x\"]);\n"
"array<dictionary>@ v2=cast<array<dictionary>>(e[\"dv\"]);r+=\",\"+v2.length()+v2[0].length()+bool(v2[1][\"z\"]);\n"
"array<uint64>@ q=cast<array<uint64>>(e[\"u64\"]);r+=\",\"+q[0];\n"
"array<float>@ f2=cast<array<float>>(e[\"fs\"]);if(f2.length()!=5||f2[2]!=3.4028235e+38f||f2[3]!=-3.4028235e+38f||f2[4]!=1.4e-45f||f2[4]==0)r+=\",floats\";\n"
"array<int>@ n32=cast<array<int>>(e[\"i32\"]);array<int16>@ n16=cast<array<int16>>(e[\"i16\"]);array<uint8>@ n8=cast<array<uint8>>(e[\"u8\"]);array<uint>@ nu=cast<array<uint>>(e[\"u32\"]);\n"
"if(n32[0]!=-2147483647-1||n32[1]!=2147483647||n16[0]!=-32768||n16[1]!=32767||n8[1]!=255||nu[0]!=4294967295)r+=\",integers\";\n"
"r+=\",\"+(cast<array<int>>(e[\"nolist\"]) is null)+\",\"+e.exists(\"none\");\n"
"return r;")
=="true,true,true,false,true,-1234567890123456789,true,true,inf,true,true,2Ay\xC5\x9F" "e,3:2147483647,true\xC4\x9F" "0,2true1,20true,18446744073709551615,true,true");
// Windows line ends are accepted; a header alone is an empty dictionary.
CHECK(value_of(fixture,"dictionary d;d.set(\"x\",1);bool a=d.deserialize(\"zodiac-dictionary 1\\r\\nint64 \\\"k\\\" 5\\r\\n\");int64 k;d.get(\"k\",k);string s=a+\",\"+show(d)+\",\"+k;bool b=d.deserialize(\"zodiac-dictionary 1\");return s+\",\"+b+\",\"+d.length();")=="true,k,5,true,0");
}

TEST_CASE("deserialize() refuses malformed input and leaves the dictionary unchanged")
{
ScriptFixture fixture;
REQUIRE(fixture.engine->RegisterGlobalFunction("string sample(int)",asFUNCTION(sample),asCALL_CDECL)>=0);
REQUIRE(fixture.engine->RegisterGlobalFunction("int sample_count()",asFUNCTION(sample_count),asCALL_CDECL)>=0);
CHECK(value_of(fixture,
"dictionary d;d.set(\"keep\",1);d.set(\"also\",\"x\");dictionary inner;inner.set(\"n\",2);d.set(\"inner\",inner);string before=d.serialize();string failures;\n"
"for(int i=0;i<sample_count();++i){if(d.deserialize(sample(i)))failures+=\"accepted \"+i+\",\";if(d.serialize()!=before)failures+=\"changed \"+i+\",\";}\n"
"return failures+d.length();")
=="3");
}

TEST_CASE("serialize() refuses values it cannot write, naming the key")
{
ScriptFixture fixture;
CHECK(error_of(fixture,"dictionary d;d.set(\"ok\",1);d.set(\"thing\",Item(1));string s=d.serialize();")=="cannot serialize \"thing\": unsupported type Item");
CHECK(error_of(fixture,"dictionary d;Item it;d.set(\"h\",@it);string s=d.serialize();")=="cannot serialize \"h\": unsupported type Item@");
CHECK(error_of(fixture,"dictionary d;array<Item@> a;d.set(\"list\",a);string s=d.serialize();")=="cannot serialize \"list\": unsupported type array<Item@>");
CHECK(error_of(fixture,"dictionary d;array<array<int>> a;d.set(\"grid\",a);string s=d.serialize();")=="cannot serialize \"grid\": unsupported type array<array<int>>");
CHECK(error_of(fixture,"dictionary d;Item it;dictionary inner;inner.set(\"deep\",@it);d.set(\"outer\",inner);string s=d.serialize();")=="cannot serialize \"deep\": unsupported type Item@");
CHECK(error_of(fixture,"dictionary@ d=dictionary();d.set(\"self\",@d);string s=d.serialize();")=="cannot serialize \"self\": the dictionary contains itself");
CHECK(error_of(fixture,"dictionary@ d=dictionary();array<dictionary@> a={d};d.set(\"loop\",@a);string s=d.serialize();")=="cannot serialize \"loop\": the dictionary contains itself");
CHECK(error_of(fixture,"dictionary d;d.set(\"line\\nbreak\",Item(1));string s=d.serialize();")=="cannot serialize \"line\\nbreak\": unsupported type Item");
const std::string long_key(70,'k');
CHECK(error_of(fixture,"dictionary d;d.set(\""+long_key+"\",Item(1));string s=d.serialize();")=="cannot serialize \""+std::string(64,'k')+"...\": unsupported type Item");
// Nesting: max_depth levels below the top round-trip, one more is refused by both directions.
const std::string chain="dictionary root;dictionary@ at=root;for(int i=0;i<LEVELS;++i){dictionary@ next=dictionary();at.set(\"n\"+i,@next);@at=next;}at.set(\"leaf\",1);";
auto with_levels=[&chain](int levels){
std::string text=chain;
text.replace(text.find("LEVELS"),6,std::to_string(levels));
return text;
};
CHECK(value_of(fixture,with_levels(zodiac::Dictionary::max_depth)+"string text=root.serialize();dictionary back;bool ok=back.deserialize(text);return ok+\",\"+(back.serialize()==text);")=="true,true");
CHECK(error_of(fixture,with_levels(zodiac::Dictionary::max_depth+1)+"string s=root.serialize();")=="cannot serialize \"n100\": nested too deeply");
}

TEST_CASE("assignment copies the values and shares the handles")
{
ScriptFixture fixture;
CHECK(value_of(fixture,
"dictionary a;Item it(1);a.set(\"v\",it);a.set(\"h\",@it);a.set(\"n\",3);dictionary b;b.set(\"old\",1);b=a;\n"
"Item@ bv=cast<Item>(b[\"v\"]);Item@ av=cast<Item>(a[\"v\"]);a=a;\n"
"return show(b)+\"|\"+(bv is av)+\",\"+(cast<Item>(b[\"h\"]) is it)+\",\"+bv.v+\",\"+show(a);")
=="v,h,n|false,true,1,v,h,n");
}

TEST_CASE("script code run while the dictionary releases or copies values cannot break it")
{
ScriptFixture fixture;
constexpr const char* globals=
"dictionary g;dictionary src;int made=0;int destroyed=0;\n"
"class Noisy{int id;Noisy(int i){id=i;++made;}~Noisy(){++destroyed;g.set(\"by\"+id,id);g.remove(\"victim\"+id);}}\n"
"class Clearer{Clearer(){++made;}~Clearer(){++destroyed;g.clear();g.set(\"after\",1);}}\n"
"class Copier{int v;Copier(){v=0;++made;}Copier(int x){v=x;++made;}Copier(const Copier&in o){v=o.v;++made;src.remove(\"drop\");g.set(\"during\",1);}~Copier(){++destroyed;}\n"
"Copier& opAssign(const Copier&in o){v=o.v;src.remove(\"drop\");g.set(\"during\",1);return this;}}\n"
"class Thief{int v;Thief(){v=0;++made;}Thief(int x){v=x;++made;}~Thief(){++destroyed;}Thief& opAssign(const Thief&in o){v=o.v;g.remove(\"t\");return this;}}\n"
"class Wiper{int v;Wiper(){v=0;++made;}Wiper(int x){v=x;++made;}Wiper(const Wiper&in o){v=o.v;++made;g.clear();}~Wiper(){++destroyed;}Wiper& opAssign(const Wiper&in o){v=o.v;g.clear();return this;}}\n"
"bool consistent(const dictionary&in d){array<string>@ k=d.keys();if(k.length()!=d.length())return false;for(int i=0;i<k.length();++i)if(!d.exists(k[i]))return false;return true;}\n";
CHECK(value_of(fixture,
"string r;\n"
"{Noisy@ n=Noisy(1);g.set(\"a\",1);g.set(\"n1\",@n);g.set(\"victim1\",2);g.set(\"z\",3);}\n"
"g.remove(\"n1\");r+=show(g)+\"|\";\n"
"{Noisy@ n=Noisy(2);g.set(\"victim2\",0);g.set(\"slot\",@n);}\n"
"g.set(\"slot\",5);r+=show(g)+\"|\"+int(g[\"slot\"])+\"|\";\n"
"{Noisy@ x=Noisy(3);Noisy@ y=Noisy(4);g.set(\"x\",@x);g.set(\"y\",@y);}\n"
"g.clear();r+=g.length()+\",\"+g.exists(\"by3\")+\",\"+g.exists(\"by4\")+\",\"+consistent(g)+\"|\";\n"
"g.clear();{Clearer@ c=Clearer();g.set(\"p\",1);g.set(\"c\",@c);Noisy@ m=Noisy(5);g.set(\"m\",@m);g.set(\"q\",2);}\n"
"g.clear();r+=g.exists(\"after\")+\",\"+g.exists(\"p\")+\",\"+g.exists(\"q\")+\",\"+consistent(g)+\"|\";\n"
"src.set(\"c\",Copier(9));src.set(\"drop\",1);src.set(\"k\",2);g.clear();g.set(\"old\",1);\n"
"g=src;Copier@ gc=cast<Copier>(g[\"c\"]);Copier@ sc=cast<Copier>(src[\"c\"]);\n"
"r+=show(g)+\",\"+show(src)+\",\"+(gc !is sc)+\",\"+gc.v+\"|\";\n"
"g.clear();g.set(\"t\",Thief(5));Thief x;bool got=g.get(\"t\",x);r+=got+\",\"+x.v+\",\"+g.exists(\"t\")+\"|\";\n"
"g.clear();g.set(\"a\",1);g[\"w\"]=Wiper(1);r+=g.length()+\",\"+consistent(g);\n"
"for(int i=0;i<40;++i)g.set(\"f\"+i,i);r+=\",\"+g.length()+\",\"+consistent(g)+\"|\";\n"
"g.clear();src.clear();@gc=null;@sc=null;\n"
"return r+(made-destroyed);",globals)
=="a,z,by1|a,z,by1,slot,by2|5|2,true,true,true|true,false,false,true|c,drop,k,c,k,true,9|true,5,false|0,true,40,true|1");
}

TEST_CASE("handle cycles through dictionaries are collected and the engine shuts down cleanly")
{
constexpr const char* source=
"class Node{dictionary d;~Node(){node_destroyed();}}\n"
"class Holder{dictionary_value v;~Holder(){node_destroyed();}}\n"
"class Leaf{~Leaf(){node_destroyed();}}\n"
"int main(){\n"
"int made=0;\n"
"for(int i=0;i<50;++i){dictionary@ d=dictionary();d.set(\"self\",@d);d[\"again\"]=@d;Leaf leaf;d.set(\"leaf\",@leaf);++made;}\n"
"for(int i=0;i<50;++i){Node n;n.d.set(\"me\",@n);++made;}\n"
"for(int i=0;i<50;++i){Holder h;h.v=@h;++made;}\n"
"for(int i=0;i<50;++i){array<dictionary@> list;dictionary d;d.set(\"list\",@list);list.insert_last(d);Leaf leaf;d[\"leaf\"]=@leaf;++made;}\n"
"return made;\n"
"}\n";
g_destroyed=0;
{
ScriptFixture fixture;
// Without the automatic collector's steps during the run, only the full cycle below can free the cycles.
REQUIRE(fixture.engine->SetEngineProperty(asEP_AUTO_GARBAGE_COLLECT,false)>=0);
REQUIRE(fixture.engine->RegisterGlobalFunction("void node_destroyed()",asFUNCTION(node_destroyed),asCALL_CDECL)>=0);
REQUIRE(fixture.run(source,"int main()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnDWord()==200);
fixture.release_context();
CHECK(g_destroyed==0);
REQUIRE(fixture.engine->GarbageCollect(asGC_FULL_CYCLE)>=0);
CHECK(g_destroyed==200);
asUINT live=1;
fixture.engine->GetGCStatistics(&live);
CHECK(live==0);
}
CHECK(g_destroyed==200);
g_destroyed=0;
{
ScriptFixture fixture;
REQUIRE(fixture.engine->RegisterGlobalFunction("void node_destroyed()",asFUNCTION(node_destroyed),asCALL_CDECL)>=0);
REQUIRE(fixture.run(source,"int main()")==asEXECUTION_FINISHED);
fixture.release_context();
}
// Left to the engine's shutdown, every object of every cycle is still destroyed.
CHECK(g_destroyed==200);
}

namespace {
// count keys "<prefix><n>" (n counting up from 0) whose String::hash() satisfies wanted.
template<typename Wanted>
std::vector<std::string> keys_where(std::string_view prefix,std::size_t count,Wanted wanted)
{
std::vector<std::string> keys;
std::string key(prefix);
for(std::uint64_t n=0;keys.size()<count;++n){
key.resize(prefix.size());
key+=std::to_string(n);
if(wanted(zodiac::string_detail::hash_bytes(key.data(),key.size())))keys.push_back(key);
}
return keys;
}
// key as serialize() writes it (the contract's quoting): '"' and '\' escaped, \n, \r and \t, other bytes below 0x20 and 0x7F
// as \xNN in lowercase hex, every other byte as is. (Not named quoted: with <filesystem> in, ADL would find std::quoted.)
std::string quoted_key(std::string_view key)
{
constexpr char hex[]="0123456789abcdef";
std::string out="\"";
for(const char c:key){
const auto byte=static_cast<unsigned char>(c);
if(byte=='"'||byte=='\\'){
out+='\\';
out+=c;
}
else if(byte=='\n')out+="\\n";
else if(byte=='\r')out+="\\r";
else if(byte=='\t')out+="\\t";
else if(byte<0x20||byte==0x7F){
out+="\\x";
out+=hex[byte>>4];
out+=hex[byte&0xF];
}
else out+=c;
}
return out+'"';
}
// count keys whose full 64-bit String::hash() values are all equal, found by construction: hash() is wyhash's structure
// with its public secrets, and wyhash multiplies (input word ^ secret) by another word, so an input word equal to the
// secret zeroes the product and with it everything mixed into it, whatever the seed. With secret1, wyhash's public
// 0xE7037ED1A0B428DB, as hash() uses it:
// - 16 bytes (inline storage): a = (read32(0) << 32) | read32(8) meets secret1 when bytes 0-3 hold 0xE7037ED1 and bytes 8-11
//   hold 0xA0B428DB (little-endian), so mix(a ^ secret1, b ^ seed) is 0 and the result no longer depends on b, which holds
//   bytes 4-7 and 12-15: those carry the key's number.
// - 32 bytes (heap storage): the first 16-byte step is mix(read64(0) ^ secret1, read64(8) ^ seed), so bytes 0-7 equal to
//   secret1 make the state 0 whatever bytes 8-15 (the key's number) are; bytes 16-31 are the same in every key.
std::vector<std::string> colliding_keys(std::size_t count,bool heap)
{
constexpr std::uint64_t secret1=0xE7037ED1A0B428DBu;
const auto put32=[](std::string& key,std::size_t at,std::uint32_t value){
for(int k=0;k<4;++k)key[at+static_cast<std::size_t>(k)]=static_cast<char>(value>>(8*k));
};
std::vector<std::string> keys;
keys.reserve(count);
for(std::size_t n=0;n<count;++n){
std::string digits=std::to_string(n);
digits.insert(0,8-digits.size(),'0');
std::string key(heap?32:16,'\0');
if(heap){
put32(key,0,static_cast<std::uint32_t>(secret1));
put32(key,4,static_cast<std::uint32_t>(secret1>>32));
key.replace(8,8,digits);
key.replace(16,16,"flooding a table");
}
else{
put32(key,0,static_cast<std::uint32_t>(secret1>>32));
key.replace(4,4,digits,0,4);
put32(key,8,static_cast<std::uint32_t>(secret1));
key.replace(12,4,digits,4,4);
}
keys.push_back(std::move(key));
}
return keys;
}
// Inserts keys into a fresh dictionary and checks that no probe run grows long, that every key is found, that the keys
// and serialize() keep insertion order, and the same again after every other key is removed.
void check_spread(ScriptFixture& fixture,const std::vector<std::string>& keys)
{
REQUIRE(fixture.run("dictionary@ make(){return dictionary();}","dictionary@ make()")==asEXECUTION_FINISHED);
auto* dictionary=static_cast<zodiac::Dictionary*>(fixture.context->GetReturnObject());
REQUIRE(dictionary!=nullptr);
dictionary->add_ref();
fixture.release_context();
for(std::size_t at=0;at<keys.size();++at)dictionary->set(zodiac::String(keys[at]),static_cast<std::int64_t>(at));
REQUIRE(dictionary->length()==static_cast<int>(keys.size()));
// keys.size() is a power of two: the index holds twice as many slots, so a home slot takes all its low bits.
CHECK(zodiac::DictionaryTestAccess::index_size(*dictionary)==2*keys.size());
// At load 1/2, linear probing over a uniform mapping moves a key 0.5 slots on average, and a run of L slots has a chance of
// about e^(-0.19 L), so no key moves 128 slots (measured: at most 30); keys sharing one home slot would move about
// keys.size()/2 on average and up to keys.size()-1.
constexpr std::uint32_t longest=128;
auto spread=zodiac::DictionaryTestAccess::displacement(*dictionary);
CHECK(spread.largest<=longest);
CHECK(spread.total<=3*keys.size());
std::string expected="zodiac-dictionary 1\n";
for(std::size_t at=0;at<keys.size();++at){
const zodiac::DictionaryValue* value=dictionary->find(zodiac::String(keys[at]));
REQUIRE(value!=nullptr);
std::int64_t read=-1;
CHECK(dictionary->get(zodiac::String(keys[at]),&read,asTYPEID_INT64));
CHECK(read==static_cast<std::int64_t>(at));
expected+="int64 "+quoted_key(keys[at])+" "+std::to_string(at)+"\n";
}
CHECK(dictionary->serialize().bytes()==expected);
expected="zodiac-dictionary 1\n";
for(std::size_t at=0;at<keys.size();++at){
if(at%2==0)CHECK(dictionary->remove(zodiac::String(keys[at])));
else expected+="int64 "+quoted_key(keys[at])+" "+std::to_string(at)+"\n";
}
spread=zodiac::DictionaryTestAccess::displacement(*dictionary);
CHECK(spread.largest<=longest);
CHECK(spread.total<=3*keys.size());
for(std::size_t at=0;at<keys.size();++at)CHECK(dictionary->exists(zodiac::String(keys[at]))==(at%2==1));
CHECK(dictionary->serialize().bytes()==expected);
dictionary->release();
}
}

TEST_CASE("keys chosen to collide in String::hash()'s bits do not pile up in one probe run")
{
// String::hash() is stable and visible to scripts, so a player or a save file can pick keys whose hashes share the bits a
// table of 2048 slots would use (the low 11 or the high 11); the slots are placed by String::table_hash(), keyed per
// process, so they spread like any other keys.
ScriptFixture fixture;
constexpr std::size_t count=1024;
INFO("low bits");
check_spread(fixture,keys_where("low",count,[](std::uint64_t hash){return (hash&0xFFF)==0;}));
INFO("high bits");
check_spread(fixture,keys_where("high",count,[](std::uint64_t hash){return (hash>>52)==0;}));
INFO("ordinary keys");
check_spread(fixture,keys_where("key",count,[](std::uint64_t){return true;}));
}

TEST_CASE("keys with equal String::hash() do not share a probe run")
{
// Keys whose whole 64-bit stable hashes are equal (colliding_keys builds them offline, as an attacker could) would share
// one home slot under any mapping of String::hash(); the slots use String::table_hash(), whose secrets are drawn per
// process, so equal stable hashes say nothing about the table hashes and the keys spread like any other keys.
ScriptFixture fixture;
constexpr std::size_t count=1024;
for(const bool heap:{false,true}){
const std::string what=heap?"32-byte heap keys":"16-byte inline keys";
INFO(what);
const std::vector<std::string> keys=colliding_keys(count,heap);
const std::uint64_t shared=zodiac::String(keys[0]).hash();
std::size_t equal=0;
for(const std::string& key:keys)equal+=zodiac::String(key).hash()==shared?1:0;
REQUIRE(equal==count);
REQUIRE(std::is_sorted(keys.begin(),keys.end()));
REQUIRE(std::adjacent_find(keys.begin(),keys.end())==keys.end());
check_spread(fixture,keys);
}
}

TEST_CASE("small dictionaries made and dropped in a loop and on other threads reuse their cached blocks intact")
{
// Task 5c fix round 1: a small dictionary's object, first segment and 16- or 32-slot index come from per-thread caches. Sizes 0-20
// cover every cached block (indexes of 16 and 32 slots) and uncached ones (a 64-slot index, a second segment); each round checks
// that a reused block holds nothing of its last dictionary (stale keys, stale index slots).
ScriptFixture fixture;
const char* source=
"array<dictionary@> made;\n"
"bool loop(){for(int round=0;round<300;++round){dictionary d;int count=round%21;for(int i=0;i<count;++i)d.set(\"k\"+i,int64(i*round));"
"if(d.length()!=count||d.exists(\"k\"+count))return false;for(int i=0;i<count;++i){int64 v=-1;if(!d.get(\"k\"+i,v)||v!=i*round)return false;}"
"if(count>0&&(!d.remove(\"k0\")||d.exists(\"k0\")||d.length()!=count-1))return false;}return true;}\n"
"void make(){for(int i=0;i<40;++i){dictionary@ d=dictionary();for(int k=0;k<i%19;++k)d.set(\"m\"+k,int64(k+i));made.insert_last(d);}}\n"
"void drop(){made.resize(0);}\n"
"bool check(){if(made.length()!=40)return false;for(int i=0;i<40;++i){dictionary@ d=made[i];if(d.length()!=i%19)return false;"
"for(int k=0;k<i%19;++k){int64 v=-1;if(!d.get(\"m\"+k,v)||v!=k+i)return false;}if(d.exists(\"m\"+(i%19)))return false;}return true;}\n";
REQUIRE(fixture.run(source,"bool loop()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
asIScriptModule* module=fixture.engine->GetModule("test");
REQUIRE(module!=nullptr);
asIScriptFunction* make=module->GetFunctionByDecl("void make()");
asIScriptFunction* drop=module->GetFunctionByDecl("void drop()");
asIScriptFunction* check=module->GetFunctionByDecl("bool check()");
REQUIRE((make&&drop&&check));
asPrepareMultithread();
// Runs function of the built module on the calling thread; false when it did not finish, or when it returns bool and returned false.
// The garbage collection after it (collect) frees the dropped dictionaries on that thread.
const auto call=[&fixture](asIScriptFunction* function,bool collect){
asIScriptContext* context=fixture.engine->CreateContext();
bool ok=context&&context->Prepare(function)>=0&&context->Execute()==asEXECUTION_FINISHED;
if(ok&&function->GetReturnTypeId()==asTYPEID_BOOL)ok=context->GetReturnByte()!=0;
if(context)context->Release();
if(collect)fixture.engine->GarbageCollect(asGC_FULL_CYCLE);
return ok;
};
// Each worker counts its failures (a failed REQUIRE cannot leave a thread) and the test requires none after it.
int failed=0;
// Made on a worker, then checked and freed (by the garbage collector) on this thread: their blocks go to this thread's cache.
std::thread maker([&]{
if(!call(make,false))++failed;
asThreadCleanup();
});
maker.join();
CHECK(call(check,false));
CHECK(call(drop,true));
// A thread that makes, drops and collects three times (filling its own cache from the blocks it frees) and ends: its cached blocks are
// freed with it.
std::thread filler([&]{
for(int round=0;round<3;++round){
if(!call(make,false)||!call(check,false)||!call(drop,true))++failed;
}
asThreadCleanup();
});
filler.join();
CHECK(failed==0);
// This thread then makes dictionaries from its cache and from the allocator, and they read back intact.
CHECK(call(make,false));
CHECK(call(check,false));
CHECK(call(drop,true));
REQUIRE(fixture.run(source,"bool loop()")==asEXECUTION_FINISHED);
CHECK(fixture.context->GetReturnByte()!=0);
}
