// The literal-path scan and zodiac build's warnings about it (contract in cli/literal_paths.hpp).
#include <cli/literal_paths.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/path_utf8.hpp>
#include <core/text.hpp>
#include <core/vfs/path.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace zodiac::cli {
namespace {
namespace fs=std::filesystem;

enum class Kind:std::uint8_t{identifier,string,punctuation,other};
struct Token{
Kind kind=Kind::other;
// The punctuation character.
char punctuation=0;
// A string's decoding failed (an escape the compiler refuses or drops).
bool invalid=false;
int line=1;
// An identifier's text.
std::string_view text;
// A string's decoded value.
std::string value;
};

bool identifier_byte(unsigned char c) noexcept
{
return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c>=0x80;
}
int hex_digit(char c) noexcept
{
if(c>='0'&&c<='9')return c-'0';
if(c>='a'&&c<='f')return c-'a'+10;
if(c>='A'&&c<='F')return c-'A'+10;
return -1;
}
void append_utf8(std::string& out,std::uint32_t code)
{
if(code<0x80)out.push_back(static_cast<char>(code));
else if(code<0x800){
out.push_back(static_cast<char>(0xC0|code>>6));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}
else if(code<0x10000){
out.push_back(static_cast<char>(0xE0|code>>12));
out.push_back(static_cast<char>(0x80|(code>>6&0x3F)));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}
else{
out.push_back(static_cast<char>(0xF0|code>>18));
out.push_back(static_cast<char>(0x80|(code>>12&0x3F)));
out.push_back(static_cast<char>(0x80|(code>>6&0x3F)));
out.push_back(static_cast<char>(0x80|(code&0x3F)));
}
}
// The body of a "..." or '...' string with its escapes decoded as asCCompiler::ProcessStringConstant does; false for one it
// would refuse or change (an unknown escape, a short \u, a code point that is no Unicode scalar value).
bool decode_escapes(std::string_view body,std::string& out)
{
out.reserve(body.size());
for(std::size_t i=0;i<body.size();++i){
if(body[i]!='\\'){
out.push_back(body[i]);
continue;
}
if(++i==body.size())return false;
switch(body[i]){
case '"':out.push_back('"');break;
case '\'':out.push_back('\'');break;
case 'n':out.push_back('\n');break;
case 'r':out.push_back('\r');break;
case 't':out.push_back('\t');break;
case '0':out.push_back('\0');break;
case '\\':out.push_back('\\');break;
case 'x':case 'X':{
unsigned value=0;
std::size_t digits=0;
for(;digits<2&&i+1<body.size()&&hex_digit(body[i+1])>=0;++digits)value=value*16+static_cast<unsigned>(hex_digit(body[++i]));
if(digits==0)return false;
out.push_back(static_cast<char>(value));
break;
}
case 'u':case 'U':{
const std::size_t wanted=body[i]=='u'?4:8;
std::uint32_t value=0;
for(std::size_t digit=0;digit<wanted;++digit){
if(i+1>=body.size()||hex_digit(body[i+1])<0)return false;
value=value*16+static_cast<std::uint32_t>(hex_digit(body[++i]));
}
if(value>0x10FFFF||(value>=0xD800&&value<=0xDFFF))return false;
append_utf8(out,value);
break;
}
default:return false;
}
}
return true;
}
// A """...""" body as asCCompiler::ProcessHeredocStringConstant gives it (the engine's default trim mode 1): the first line goes
// when it holds only spaces, tabs and '\r' and a line break follows, and then so does what follows the last line break when it is
// only such characters.
std::string heredoc_value(std::string_view body)
{
std::size_t start=0;
bool multiline=false;
for(;start<body.size();++start){
if(body[start]=='\n'){
multiline=true;
++start;
break;
}
if(body[start]!=' '&&body[start]!='\t'&&body[start]!='\r'){
start=0;
break;
}
}
if(!multiline)return std::string(body);
std::size_t end=body.size();
for(std::size_t at=body.size();at>0;--at){
const char c=body[at-1];
if(c=='\n'){
end=at;
break;
}
if(c!=' '&&c!='\t'&&c!='\r')break;
}
if(end<=start)return {};
return std::string(body.substr(start,end-start));
}

std::vector<Token> tokenize(std::string_view source)
{
std::vector<Token> tokens;
int line=1;
const std::size_t size=source.size();
for(std::size_t i=0;i<size;){
const char c=source[i];
if(c=='\n'){
++line;
++i;
continue;
}
if(c==' '||c=='\t'||c=='\r'||c=='\f'||c=='\v'){
++i;
continue;
}
if(c=='/'&&i+1<size&&source[i+1]=='/'){
const std::size_t end=source.find('\n',i);
i=end==std::string_view::npos?size:end;
continue;
}
if(c=='/'&&i+1<size&&source[i+1]=='*'){
const std::size_t end=source.find("*/",i+2);
const std::size_t stop=end==std::string_view::npos?size:end+2;
line+=static_cast<int>(std::count(source.begin()+static_cast<std::ptrdiff_t>(i),source.begin()+static_cast<std::ptrdiff_t>(stop),'\n'));
i=stop;
continue;
}
Token token;
token.line=line;
if(c=='"'&&source.substr(i,3)=="\"\"\""){
const std::size_t end=source.find("\"\"\"",i+3);
const std::size_t body_end=end==std::string_view::npos?size:end;
const std::string_view body=source.substr(i+3,body_end-i-3);
token.kind=Kind::string;
token.value=heredoc_value(body);
token.invalid=end==std::string_view::npos;
line+=static_cast<int>(std::ranges::count(body,'\n'));
i=end==std::string_view::npos?size:end+3;
tokens.push_back(std::move(token));
continue;
}
if(c=='"'||c=='\''){
std::size_t at=i+1;
for(;at<size&&source[at]!=c;++at){
if(source[at]=='\\'&&at+1<size)++at;
}
const std::string_view body=source.substr(i+1,std::min(at,size)-i-1);
token.kind=Kind::string;
token.invalid=at>=size||!decode_escapes(body,token.value);
line+=static_cast<int>(std::ranges::count(body,'\n'));
i=at>=size?size:at+1;
tokens.push_back(std::move(token));
continue;
}
const auto byte=static_cast<unsigned char>(c);
if(identifier_byte(byte)){
std::size_t at=i+1;
while(at<size&&identifier_byte(static_cast<unsigned char>(source[at])))++at;
// A number (it starts with a digit) is no identifier; "1.5" leaves ".5", which is punctuation and a number, harmless here.
token.kind=c>='0'&&c<='9'?Kind::other:Kind::identifier;
token.text=source.substr(i,at-i);
i=at;
tokens.push_back(std::move(token));
continue;
}
token.kind=Kind::punctuation;
token.punctuation=c;
++i;
tokens.push_back(std::move(token));
}
return tokens;
}

bool is_punctuation(const Token& token,char c) noexcept
{
return token.kind==Kind::punctuation&&token.punctuation==c;
}

// One argument of a call: the tokens [first, last).
struct Argument{
std::size_t first=0;
std::size_t last=0;
};
// The arguments of the call whose '(' is at open, split at commas outside brackets; empty when the call is not closed.
std::vector<Argument> arguments_of(const std::vector<Token>& tokens,std::size_t open)
{
std::vector<Argument> arguments;
int depth=0;
std::size_t first=open+1;
for(std::size_t i=open+1;i<tokens.size();++i){
const Token& token=tokens[i];
if(token.kind!=Kind::punctuation)continue;
const char c=token.punctuation;
if(c=='('||c=='['||c=='{')++depth;
else if(c==')'||c==']'||c=='}'){
if(depth==0){
if(c!=')')return {};
if(i>first||!arguments.empty())arguments.push_back({first,i});
return arguments;
}
--depth;
}
else if(c==','&&depth==0){
arguments.push_back({first,i});
first=i+1;
}
}
return {};
}
// The literal an argument is (adjacent string literals joined), with the line of its first piece; nullopt when it is anything else.
std::optional<std::pair<std::string,int>> literal_of(const std::vector<Token>& tokens,const Argument& argument)
{
if(argument.first==argument.last)return std::nullopt;
std::string value;
for(std::size_t i=argument.first;i<argument.last;++i){
const Token& token=tokens[i];
if(token.kind!=Kind::string||token.invalid)return std::nullopt;
value.append(token.value);
}
return std::pair{std::move(value),tokens[argument.first].line};
}
bool is_identifier(const std::vector<Token>& tokens,const Argument& argument,std::string_view text) noexcept
{
return argument.last==argument.first+1&&tokens[argument.first].kind==Kind::identifier&&tokens[argument.first].text==text;
}
bool file_mode(std::string_view mode) noexcept
{
constexpr std::array<std::string_view,9> modes{"r","w","a","r+","rb","wb","ab","r+b","rb+"};
return std::ranges::find(modes,mode)!=modes.end();
}

// A pack's literal open, for pairing an .extract with it.
struct PackOpen{
std::optional<std::string> path;
std::optional<std::string> key;
};

// Calls whose first argument is a path that is read.
constexpr std::array<std::string_view,7> kReadingFunctions{"read_file","file_exists","file_size","file_modified","list_files","copy_file","move_file"};
// Calls whose first argument is a path that is written.
constexpr std::array<std::string_view,2> kWritingFunctions{"write_file","append_file"};

enum class Declared:std::uint8_t{file,pack,both};
// The names declared with the type file or pack in source ("file f", "pack@ p", class members too): a declaration is the type name,
// an optional '@', and a name not followed by '(' (that would be a function returning one). A name declared with both types (in
// two scopes) is both, so neither rule is sure of it.
std::unordered_map<std::string_view,Declared> declared_types(const std::vector<Token>& tokens)
{
std::unordered_map<std::string_view,Declared> types;
for(std::size_t i=0;i+1<tokens.size();++i){
const Token& type=tokens[i];
if(type.kind!=Kind::identifier||(type.text!="file"&&type.text!="pack"))continue;
// "x.file" is a member named file, no type.
if(i>0&&is_punctuation(tokens[i-1],'.'))continue;
std::size_t at=i+1;
if(is_punctuation(tokens[at],'@'))++at;
if(at>=tokens.size()||tokens[at].kind!=Kind::identifier)continue;
if(at+1<tokens.size()&&is_punctuation(tokens[at+1],'('))continue;
const Declared kind=type.text=="file"?Declared::file:Declared::pack;
const auto [slot,inserted]=types.try_emplace(tokens[at].text,kind);
if(!inserted&&slot->second!=kind)slot->second=Declared::both;
}
return types;
}

std::string relative_text(const fs::path& path,const fs::path& root)
{
const std::u8string text=path.lexically_relative(root).generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
void insert_with_folders(std::unordered_set<std::string>& folds,std::string fold)
{
for(std::size_t at=fold.find('/');at!=std::string::npos;at=fold.find('/',at+1))folds.insert(fold.substr(0,at));
folds.insert(std::move(fold));
}
std::optional<std::string> read_source(const fs::path& path)
{
std::ifstream in(path,std::ios::binary);
if(!in)return std::nullopt;
std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
if(in.bad())return std::nullopt;
if(text.starts_with("\xEF\xBB\xBF"))text.erase(0,3);
return text;
}
}

std::vector<LiteralPath> find_literal_paths(std::string_view source,LiteralUses* uses)
{
const std::vector<Token> tokens=tokenize(source);
const std::unordered_map<std::string_view,Declared> types=declared_types(tokens);
const auto type_of=[&types](std::string_view receiver)->std::optional<Declared>{
const auto found=types.find(receiver);
if(found==types.end())return std::nullopt;
return found->second;
};
std::vector<LiteralPath> found;
std::unordered_map<std::string_view,PackOpen> packs;
const auto report=[&found](std::optional<std::pair<std::string,int>> literal){
if(literal)found.push_back(LiteralPath{literal->second,std::move(literal->first),false,{},{}});
};
const auto written=[uses](std::optional<std::pair<std::string,int>> literal){
if(uses&&literal)uses->written.push_back(std::move(literal->first));
};
for(std::size_t i=0;i+1<tokens.size();++i){
const Token& name=tokens[i];
if(name.kind!=Kind::identifier||!is_punctuation(tokens[i+1],'('))continue;
const bool member=i>0&&is_punctuation(tokens[i-1],'.');
// The receiver of a member call when it is one identifier ("p.open"), else empty.
const std::string_view receiver=member&&i>1&&tokens[i-2].kind==Kind::identifier&&!(i>2&&is_punctuation(tokens[i-3],'.'))?tokens[i-2].text:std::string_view();
const std::optional<Declared> type=receiver.empty()?std::nullopt:type_of(receiver);
if(!member){
const bool reading=std::ranges::find(kReadingFunctions,name.text)!=kReadingFunctions.end();
const bool writing=std::ranges::find(kWritingFunctions,name.text)!=kWritingFunctions.end();
const bool mounting=name.text=="mount_pack";
if(!reading&&!writing&&!mounting)continue;
const std::vector<Argument> arguments=arguments_of(tokens,i+1);
if(arguments.empty())continue;
if(mounting){
if(uses){
// No literal folder (the default "/", or a computed one): the pack may provide any name.
auto at=arguments.size()>1?literal_of(tokens,arguments[1]):std::nullopt;
if(arguments.size()>1&&at)uses->mounted.push_back(std::move(at->first));
else uses->mounted_at_root=true;
}
continue;
}
if(writing){
written(literal_of(tokens,arguments.front()));
continue;
}
report(literal_of(tokens,arguments.front()));
// copy_file's and move_file's destination is written.
if(arguments.size()>1&&(name.text=="copy_file"||name.text=="move_file"))written(literal_of(tokens,arguments[1]));
continue;
}
if(name.text=="add_file"){
// Only on a declared pack: another object's add_file is no file read the scanner can name.
if(type!=Declared::pack)continue;
const std::vector<Argument> arguments=arguments_of(tokens,i+1);
if(!arguments.empty())report(literal_of(tokens,arguments.front()));
continue;
}
if(name.text=="open"){
const std::vector<Argument> arguments=arguments_of(tokens,i+1);
if(arguments.size()<2)continue;
auto path=literal_of(tokens,arguments[0]);
auto second=literal_of(tokens,arguments[1]);
const bool mode=second&&file_mode(second->first);
// A file's open: a declared file, or (the known pattern) any receiver whose second argument is a file mode literal, unless
// it is a declared pack.
if(type==Declared::file||(mode&&type!=Declared::pack)){
if(!receiver.empty())packs.erase(receiver);
if(!mode)continue;
if(second->first=="r"||second->first=="rb")report(std::move(path));
else written(std::move(path));
continue;
}
// A pack's open needs a declared pack: an open(path, text) of any other object is not taken for one.
if(type!=Declared::pack)continue;
const bool creating=arguments.size()>2&&!is_identifier(tokens,arguments[2],"false");
PackOpen& open=packs[receiver];
open.path.reset();
open.key.reset();
if(creating){
// A pack being created is written, so no pairing can check its entries.
written(std::move(path));
continue;
}
if(path)open.path=path->first;
if(second)open.key=second->first;
report(std::move(path));
continue;
}
if(name.text=="extract"&&type==Declared::pack){
const std::vector<Argument> arguments=arguments_of(tokens,i+1);
if(arguments.empty())continue;
// The destination is written.
if(arguments.size()>1)written(literal_of(tokens,arguments[1]));
const auto open=packs.find(receiver);
if(open==packs.end()||!open->second.path||!open->second.key)continue;
auto entry=literal_of(tokens,arguments.front());
if(!entry)continue;
LiteralPath literal{entry->second,std::move(entry->first),true,*open->second.path,*open->second.key};
found.push_back(std::move(literal));
}
}
return found;
}

void check_literal_paths(const Project& project,std::span<const ScriptRuntime::ScriptFile> files,std::span<const fs::path> assets,std::span<const std::string> shipped,Diagnostics& diagnostics)
{
std::unordered_set<std::string> known;
// The fold of each asset's path, to its file.
std::unordered_map<std::string,const fs::path*> asset_files;
known.reserve(assets.size()*2+shipped.size());
asset_files.reserve(assets.size());
for(const fs::path& asset:assets){
std::string fold=vfs::fold_name(relative_text(asset,project.root));
asset_files.emplace(fold,&asset);
insert_with_folders(known,std::move(fold));
}
for(const std::string& name:shipped)known.insert(vfs::fold_name(name));
// The packs opened for .extract checks, by asset fold and key; null when the pack could not be opened.
std::map<std::pair<std::string,std::string>,std::shared_ptr<pack::PackStorage>> packs;
const auto resolve=[&project](std::string_view path)->std::optional<std::string>{
auto parsed=vfs::parse_path(path,project.files.host_paths);
if(!parsed||!parsed->mount.empty())return std::nullopt;
return vfs::fold_name(parsed->relative);
};
// Every script first: a name the scripts write themselves (a save, a log) is no asset the release lacks, and a pack a script
// mounts may provide names below its folder (anything, mounted at "/" or at a computed folder).
std::vector<std::vector<LiteralPath>> reads;
reads.reserve(files.size());
LiteralUses uses;
for(const ScriptRuntime::ScriptFile& file:files){
const std::optional<std::string> source=read_source(file.path);
reads.push_back(source?find_literal_paths(*source,&uses):std::vector<LiteralPath>());
}
if(uses.mounted_at_root)return;
std::unordered_set<std::string> written;
for(const std::string& path:uses.written)if(auto fold=resolve(path))written.insert(std::move(*fold));
std::vector<std::string> mounted;
for(const std::string& at:uses.mounted){
auto parsed=vfs::parse_path(at,project.files.host_paths);
if(!parsed||!parsed->mount.empty())continue;
// A pack mounted at the root folder itself ("/", "") may provide any name.
if(parsed->relative.empty())return;
mounted.push_back(vfs::fold_name(parsed->relative)+'/');
}
for(std::size_t index=0;index<files.size();++index){
const ScriptRuntime::ScriptFile& file=files[index];
for(const LiteralPath& literal:reads[index]){
bool found=false;
if(literal.pack_entry){
const std::optional<std::string> pack_fold=resolve(literal.pack);
if(!pack_fold)continue;
const auto asset=asset_files.find(*pack_fold);
if(asset==asset_files.end())continue;
auto [slot,inserted]=packs.try_emplace({*pack_fold,literal.key});
if(inserted){
auto size=platform::stat_path(*asset->second);
if(size){
auto opened=pack::PackStorage::open(*asset->second,literal.key,false,{},pack::PackLocation{0,size->size});
if(opened)slot->second=std::move(*opened);
}
}
if(!slot->second)continue;
// An entry name follows the root mount's path rules ("maps\1.txt" is "maps/1.txt").
const auto name=vfs::parse_path(literal.path,false);
if(!name||!name->mount.empty()||name->folder||name->relative.empty())continue;
const auto entry=slot->second->stat(name->relative);
found=entry&&entry->kind==vfs::EntryKind::file;
}
else{
const std::optional<std::string> fold=resolve(literal.path);
if(!fold)continue;
found=fold->empty()||known.contains(*fold)||written.contains(*fold)||std::ranges::any_of(mounted,[&fold](const std::string& at){return fold->starts_with(at)||std::string_view(*fold)==std::string_view(at).substr(0,at.size()-1);});
}
if(found)continue;
std::string line="warning: ";
line.append(file.section).push_back(':');
line.append(std::to_string(literal.line)).append(": \"").append(one_line(literal.path)).append("\" is not among the declared assets, so the release cannot open it");
diagnostics.report(ZODIAC_LOG_WARNING,line);
}
}
}
}
