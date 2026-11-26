// zodiac build's literal-path scan (src/cli/literal_paths.hpp; spec 8.4 of the library-and-packs design): every read-style call
// with a literal path is found with its line, write targets, comments, strings that only contain a call, and computed paths are
// not, escapes, heredocs and joined literals give the compiler's value, and .extract is paired with its pack's literal open.
#include <cli/literal_paths.hpp>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using zodiac::cli::LiteralPath;
using zodiac::cli::find_literal_paths;

namespace {
struct Expected{
Expected(int at,std::string name):line(at),path(std::move(name)){}
Expected(int at,std::string name,bool entry,std::string in,std::string with):line(at),path(std::move(name)),pack_entry(entry),pack(std::move(in)),key(std::move(with)){}
int line;
std::string path;
bool pack_entry=false;
std::string pack;
std::string key;
};
void check(std::string_view source,const std::vector<Expected>& expected)
{
const std::vector<LiteralPath> found=find_literal_paths(source);
CAPTURE(source);
REQUIRE(found.size()==expected.size());
for(std::size_t i=0;i<found.size();++i){
CAPTURE(i);
CHECK(found[i].line==expected[i].line);
CHECK(found[i].path==expected[i].path);
CHECK(found[i].pack_entry==expected[i].pack_entry);
CHECK(found[i].pack==expected[i].pack);
CHECK(found[i].key==expected[i].key);
}
}
}

TEST_CASE("every read-style call with a literal path is found, one per line")
{
check(
"string a=read_file(\"r.txt\");\n"
"bool b=file_exists(\"e.txt\");\n"
"f.open(\"a.txt\",\"r\");\n"
"int64 s=file_size(\"s.txt\");\n"
"date_time m=file_modified(\"m.txt\");\n"
"array<string>@ l=list_files(\"sounds\",\"*.ogg\");\n"
"copy_file(\"from.txt\",\"to.txt\");\n"
"move_file(\"from2.txt\",\"to2.txt\",true);\n"
"f.open(\"b.txt\",\"rb\");\n"
"p.open(\"maps.zpk\",\"secret\");\n"
"p.add_file(\"disk.txt\",\"entry.txt\");\n"
"p.extract(\"maps/1.txt\",\"data:/1.txt\");\n"
"q.open(\"other.zpk\",key,false);\n",
{{1,"r.txt"},{2,"e.txt"},{3,"a.txt"},{4,"s.txt"},{5,"m.txt"},{6,"sounds"},{7,"from.txt"},{8,"from2.txt"},{9,"b.txt"},{10,"maps.zpk"},{11,"disk.txt"},{12,"maps/1.txt",true,"maps.zpk","secret"},{13,"other.zpk"}});
}

TEST_CASE("write targets, comments, strings and computed paths are ignored")
{
check(
"write_file(\"log.txt\",text);\n"
"append_file(\"log.txt\",text);\n"
"f.open(\"save.txt\",\"w\");\n"
"f.open(\"save.txt\",\"a\");\n"
"f.open(\"save.txt\",\"r+\");\n"
"f.open(\"save.txt\",\"wb\");\n"
"// read_file(\"c.txt\");\n"
"/* read_file(\"d.txt\");\n file_exists(\"d2.txt\"); */\n"
"string s=\"read_file(\\\"in_a_string.txt\\\")\";\n"
"read_file(\"a\"+name);\n"
"read_file(name);\n"
"read_file(folder+\"/x.txt\");\n"
"p.open(\"new.zpk\",\"k\",true);\n"
"q.extract(\"x.txt\",\"data:/x.txt\");\n"
"x.read_file(\"member.txt\");\n"
"f.open(\"one_argument.txt\");\n",
{});
// copy_file's destination is never checked, the source still is.
check("copy_file(from,\"to.txt\");\ncopy_file(\"from.txt\",\"to.txt\");\n",{{2,"from.txt"}});
}

TEST_CASE("an extract names only its entry, paired with the nearest literal open of its receiver")
{
check("p.extract(\"maps/1.txt\",\"data:/1.txt\");\n",{});
check("p.open(\"maps.zpk\",\"k\");\np.extract(\"maps/1.txt\",\"data:/1.txt\");\n",{{1,"maps.zpk"},{2,"maps/1.txt",true,"maps.zpk","k"}});
// A computed key or path leaves nothing to pair with; a later file open of the name drops the pack; another receiver is another pack.
check("p.open(\"maps.zpk\",key);\np.extract(\"a.txt\",\"b.txt\");\n",{{1,"maps.zpk"}});
check("p.open(name,\"k\");\np.extract(\"a.txt\",\"b.txt\");\n",{});
check("p.open(\"m.zpk\",\"k\");\np.open(\"x.txt\",\"w\");\np.extract(\"a.txt\",\"b.txt\");\n",{{1,"m.zpk"}});
check("p.open(\"m.zpk\",\"k\");\nq.extract(\"a.txt\",\"b.txt\");\n",{{1,"m.zpk"}});
check("p.open(\"m.zpk\",\"k\");\np.open(\"n.zpk\",\"j\");\np.extract(\"a.txt\",\"b.txt\");\n",{{1,"m.zpk"},{2,"n.zpk"},{3,"a.txt",true,"n.zpk","j"}});
}

TEST_CASE("literals decode as the compiler decodes them")
{
check(
"read_file(\"sounds\\\\shot.ogg\");\n"
"read_file('single.txt');\n"
"read_file(\"joined\" \"_name.txt\");\n"
"read_file(\"\\x41\\u00e7\\U0001F600.txt\");\n"
"read_file(\"\"\"\n  heredoc.txt\"\"\");\n"
"read_file(\"\"\"inline.txt\"\"\");\n"
"read_file(\"\\q.txt\");\n"
"read_file(\"\xC5\x9F\x61rk\xC4\xB1.ogg\");\n"
"read_file(\"spans\n"
"two.txt\");\n"
"file_exists(\"after.txt\");\n",
{{1,"sounds\\shot.ogg"},{2,"single.txt"},{3,"joined_name.txt"},{4,"A\xC3\xA7\xF0\x9F\x98\x80.txt"},{5,"  heredoc.txt"},{7,"inline.txt"},{9,"\xC5\x9F\x61rk\xC4\xB1.ogg"},{10,"spans\ntwo.txt"},{12,"after.txt"}});
}
