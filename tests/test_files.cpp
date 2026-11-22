// Tests the script file object and the file helpers (core/vfs/files.hpp; spec 8.7 of the core-types design) through scripts run
// against ScriptFixture's Vfs: every mode, text and binary reads (line ends, the BOM, a line end cut by a count), read_line, the
// misuse and failure texts, the helpers and last_file_error, copies and moves across "/" and "data:/", file_modified against
// date_time::now(), reads at the 64 KiB buffer's edges, and a sparse file of more than 4 GiB read at its end (Review Focus 3).
#include "script_fixture.hpp"
#include <core/vfs/vfs.hpp>
#include <platform/clock.hpp>
#include <core/vfs/folder_source.hpp>
#include <platform/file.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <string_view>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using zodiac_test::ScriptFixture;

namespace {
namespace fs=std::filesystem;
// Runs "bool main(){<body>}" and returns its result.
bool holds(ScriptFixture& fixture,std::string_view body)
{
const std::string source="bool main(){"+std::string(body)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"bool main()")==asEXECUTION_FINISHED);
return fixture.context->GetReturnByte()!=0;
}
// Runs "string main(){<body>}" and returns the text.
std::string text_of(ScriptFixture& fixture,std::string_view body)
{
const std::string source="string main(){"+std::string(body)+"}";
INFO(source);
const int result=fixture.run(source.c_str(),"string main()");
INFO(fixture.exception_string());
REQUIRE(result==asEXECUTION_FINISHED);
return fixture.returned_string();
}
// Runs "int64 main(){<body>}" and returns the value.
std::int64_t int64_of(ScriptFixture& fixture,std::string_view body)
{
const std::string source="int64 main(){"+std::string(body)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"int64 main()")==asEXECUTION_FINISHED);
return static_cast<std::int64_t>(fixture.context->GetReturnQWord());
}
// Runs "void main(){<body>}", which must raise a script exception, and returns its text.
std::string error_of(ScriptFixture& fixture,std::string_view body)
{
const std::string source="void main(){"+std::string(body)+"}";
INFO(source);
REQUIRE(fixture.run(source.c_str(),"void main()")==asEXECUTION_EXCEPTION);
return fixture.exception_string();
}
// Writes bytes to <root>/name from C++, making the folders on the way.
void put(const fs::path& file,std::string_view bytes)
{
fs::create_directories(file.parent_path());
std::ofstream out(file,std::ios::binary|std::ios::trunc);
out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
REQUIRE(out.good());
}
std::string contents(const fs::path& file)
{
std::ifstream in(file,std::ios::binary);
REQUIRE(in.good());
return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}
// size bytes that differ at every offset of a 64 KiB block and from block to block.
std::string pattern(std::size_t size)
{
std::string bytes(size,'\0');
for(std::size_t i=0;i<size;++i)bytes[i]=static_cast<char>((i*131+i/65536*7+1)&0xFF);
return bytes;
}
// A sparse file of offset + text.size() bytes holding text at offset and nothing (a hole) before it. Fails rather than write the
// hole out when the file system cannot keep it sparse.
void make_sparse(const fs::path& file,std::uint64_t offset,std::string_view text)
{
fs::create_directories(file.parent_path());
#ifdef _WIN32
const HANDLE handle=CreateFileW(file.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
REQUIRE(handle!=INVALID_HANDLE_VALUE);
DWORD returned=0;
const bool sparse=DeviceIoControl(handle,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)!=0;
LARGE_INTEGER end;
end.QuadPart=static_cast<LONGLONG>(offset);
const bool sized=sparse&&SetFilePointerEx(handle,end,nullptr,FILE_BEGIN)&&SetEndOfFile(handle);
DWORD written=0;
const bool wrote=sized&&WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&written==text.size();
CloseHandle(handle);
REQUIRE(sparse);
REQUIRE(sized);
REQUIRE(wrote);
DWORD high=0;
const DWORD low=GetCompressedFileSizeW(file.c_str(),&high);
REQUIRE((low!=INVALID_FILE_SIZE||GetLastError()==NO_ERROR));
const std::uint64_t allocated=(static_cast<std::uint64_t>(high)<<32)|low;
#else
const int descriptor=::open(file.c_str(),O_RDWR|O_CREAT|O_TRUNC,0600);
REQUIRE(descriptor>=0);
const bool sized=ftruncate(descriptor,static_cast<off_t>(offset))==0;
const bool wrote=sized&&pwrite(descriptor,text.data(),text.size(),static_cast<off_t>(offset))==static_cast<ssize_t>(text.size());
struct stat info{};
const bool stated=fstat(descriptor,&info)==0;
::close(descriptor);
REQUIRE(sized);
REQUIRE(wrote);
REQUIRE(stated);
const std::uint64_t allocated=static_cast<std::uint64_t>(info.st_blocks)*512;
#endif
// The hole takes no space: at most a few blocks hold the text.
REQUIRE(allocated<(std::uint64_t{1}<<20));
}
// A source that is no host folder: its files live in memory, so copies and moves through it go through the sources (the buffered
// path), and its write targets can be told to fail on write_at or on commit.
class MemorySource final:public zodiac::vfs::Source{
public:
using FileError=zodiac::vfs::FileError;
std::map<std::string,std::string,std::less<>> files;
std::optional<FileError> fail_write;
std::optional<FileError> fail_commit;
std::optional<FileError> fail_remove;
// A write target fails once it has taken this many bytes in all; a reader fails reading at or past this offset.
std::optional<std::uint64_t> fail_write_after;
std::optional<std::uint64_t> fail_read_at;
int syncs=0;
std::expected<zodiac::vfs::EntryInfo,FileError> stat(std::string_view relative) override
{
zodiac::vfs::EntryInfo info;
if(relative.empty()){
info.kind=zodiac::vfs::EntryKind::directory;
return info;
}
const auto found=files.find(relative);
if(found==files.end())return std::unexpected(FileError::not_found);
info.kind=zodiac::vfs::EntryKind::file;
info.size=found->second.size();
info.name=std::string(relative);
return info;
}
std::expected<std::shared_ptr<zodiac::vfs::Reader>,FileError> open_read(std::string_view relative) override
{
const auto found=files.find(relative);
if(found==files.end())return std::unexpected(FileError::not_found);
return std::make_shared<Reader>(found->second,fail_read_at);
}
std::expected<std::unique_ptr<zodiac::vfs::WriteTarget>,FileError> open_write(std::string_view relative,zodiac::vfs::WriteMode mode) override
{
const auto found=files.find(relative);
if(mode==zodiac::vfs::WriteMode::update&&found==files.end())return std::unexpected(FileError::not_found);
std::string bytes=mode!=zodiac::vfs::WriteMode::truncate&&found!=files.end()?found->second:std::string();
return std::make_unique<Writer>(*this,std::string(relative),std::move(bytes));
}
std::expected<void,FileError> list(std::string_view relative,std::vector<zodiac::vfs::EntryInfo>& out) override
{
if(!relative.empty())return std::unexpected(FileError::not_found);
for(const auto& [name,bytes]:files){
zodiac::vfs::EntryInfo& info=out.emplace_back();
info.kind=zodiac::vfs::EntryKind::file;
info.name=name;
}
return {};
}
std::expected<void,FileError> remove(std::string_view relative) override
{
const auto found=files.find(relative);
if(found==files.end())return std::unexpected(FileError::not_found);
if(fail_remove)return std::unexpected(*fail_remove);
files.erase(found);
return {};
}
std::expected<void,FileError> make_directory(std::string_view relative) override
{
if(relative.empty())return {};
return std::unexpected(FileError::access_denied);
}
std::expected<void,FileError> remove_directory(std::string_view,bool) override
{
return std::unexpected(FileError::access_denied);
}
bool read_only() const noexcept override{return false;}
private:
class Reader final:public zodiac::vfs::Reader{
public:
Reader(std::string bytes,std::optional<std::uint64_t> fail_at):bytes_(std::move(bytes)),fail_at_(fail_at){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override
{
if(fail_at_&&offset+buffer.size()>*fail_at_)return -static_cast<std::int64_t>(FileError::damaged);
if(offset>=bytes_.size())return 0;
const std::size_t count=std::min<std::size_t>(buffer.size(),bytes_.size()-offset);
std::memcpy(buffer.data(),bytes_.data()+offset,count);
return static_cast<std::int64_t>(count);
}
std::uint64_t size() const override{return bytes_.size();}
private:
std::string bytes_;
std::optional<std::uint64_t> fail_at_;
};
class Writer final:public zodiac::vfs::WriteTarget{
public:
Writer(MemorySource& source,std::string name,std::string bytes):source_(source),name_(std::move(name)),bytes_(std::move(bytes)){}
std::int64_t read_at(std::uint64_t offset,std::span<std::byte> buffer) override
{
if(offset>=bytes_.size())return 0;
const std::size_t count=std::min<std::size_t>(buffer.size(),bytes_.size()-offset);
std::memcpy(buffer.data(),bytes_.data()+offset,count);
return static_cast<std::int64_t>(count);
}
std::expected<void,FileError> write_at(std::uint64_t offset,std::span<const std::byte> bytes) override
{
if(source_.fail_write)return std::unexpected(*source_.fail_write);
taken_+=bytes.size();
if(source_.fail_write_after&&taken_>*source_.fail_write_after)return std::unexpected(FileError::disk_full);
if(bytes_.size()<offset+bytes.size())bytes_.resize(offset+bytes.size());
std::memcpy(bytes_.data()+offset,bytes.data(),bytes.size());
return {};
}
std::uint64_t size() const override{return bytes_.size();}
std::expected<void,FileError> truncate(std::uint64_t size) override
{
bytes_.resize(size);
return {};
}
std::expected<void,FileError> commit() override
{
if(source_.fail_commit)return std::unexpected(*source_.fail_commit);
source_.files[name_]=bytes_;
return {};
}
// Memory has no device to flush to; the count shows the flush was asked for.
std::expected<void,FileError> sync() override
{
++source_.syncs;
return {};
}
private:
MemorySource& source_;
std::string name_;
std::string bytes_;
std::uint64_t taken_=0;
};
};
std::string utf8(const fs::path& path)
{
const std::u8string text=path.generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}
// Removes a file at scope exit, also when a check fails.
class RemoveAtExit{
public:
explicit RemoveAtExit(fs::path file):file_(std::move(file)){}
~RemoveAtExit()
{
std::error_code error;
fs::remove(file_,error);
}
RemoveAtExit(const RemoveAtExit&)=delete;
RemoveAtExit& operator=(const RemoveAtExit&)=delete;
private:
fs::path file_;
};
}

TEST_CASE("every mode round-trips")
{
ScriptFixture fixture;
for(const char* write_mode:{"w","wb"}){
CAPTURE(write_mode);
CHECK(text_of(fixture,std::string("file f; if(!f.open(\"m.txt\", \"")+write_mode+"\"))return f.error; f.write(\"one\"); f.close(); f.open(\"m.txt\", \"r\"); return f.read();")=="one");
}
for(const char* append_mode:{"a","ab"}){
CAPTURE(append_mode);
CHECK(text_of(fixture,std::string("write_file(\"a.txt\", \"one\"); file f; f.open(\"a.txt\", \"")+append_mode+"\"); f.seek(0); f.write(\"two\"); f.write(\"three\"); f.close(); return read_file(\"a.txt\");")=="onetwothree");
}
for(const char* update_mode:{"r+","r+b","rb+"}){
CAPTURE(update_mode);
CHECK(text_of(fixture,std::string("write_file(\"u.txt\", \"abcdef\"); file f; f.open(\"u.txt\", \"")+update_mode+"\"); f.seek(2); f.write(\"XY\"); string rest=f.read(); f.seek(0); string all=f.read(); f.close(); return rest+\"|\"+all+\"|\"+read_file(\"u.txt\");")=="ef|abXYef|abXYef");
}
for(const char* read_mode:{"r","rb"}){
CAPTURE(read_mode);
CHECK(text_of(fixture,std::string("write_file(\"r.txt\", \"read me\"); file f; f.open(\"r.txt\", \"")+read_mode+"\"); return f.read(4)+\"|\"+f.read();")=="read| me");
}
// "w" empties an existing file and reads back what it wrote; the size and position follow the writes.
CHECK(text_of(fixture,"write_file(\"w.txt\", \"old text\"); file f; f.open(\"w.txt\", \"w\"); string s=f.size+\",\"+f.position; f.write(\"new\"); s+=\",\"+f.size+\",\"+f.position+\",\"+f.eof; f.seek(0); return s+\",\"+f.read();")=="0,0,3,3,true,new");
CHECK(contents(fixture.root()/"w.txt")=="new");
}

TEST_CASE("text mode reads line ends as \\n and skips a BOM; binary mode keeps the bytes")
{
ScriptFixture fixture;
put(fixture.root()/"crlf.txt","a\r\nb\rc\r\n\r\n");
CHECK(text_of(fixture,"file f; f.open(\"crlf.txt\", \"r\"); return f.read();")=="a\nb\rc\n\n");
CHECK(text_of(fixture,"file f; f.open(\"crlf.txt\", \"rb\"); return f.read();")=="a\r\nb\rc\r\n\r\n");
put(fixture.root()/"bom.txt","\xEF\xBB\xBFhi\r\n");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); return f.read();")=="hi\n");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); return f.read(2);")=="hi");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); string line=f.read_line(); return line+\",\"+f.position;")=="hi,7");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); f.read(); f.seek(0); return f.read();")=="hi\n");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"rb\"); return f.read();")=="\xEF\xBB\xBFhi\r\n");
// Only at position 0, and only a whole BOM.
put(fixture.root()/"late.txt","x\xEF\xBB\xBF");
CHECK(text_of(fixture,"file f; f.open(\"late.txt\", \"r\"); return f.read();")=="x\xEF\xBB\xBF");
put(fixture.root()/"half.txt","\xEF\xBB");
CHECK(text_of(fixture,"file f; f.open(\"half.txt\", \"r\"); return f.read();")=="\xEF\xBB");
// Writing never converts.
CHECK(holds(fixture,"file f; f.open(\"out.txt\", \"w\"); return f.write(\"a\\r\\nb\\n\");"));
CHECK(contents(fixture.root()/"out.txt")=="a\r\nb\n");
}

TEST_CASE("a counted text read takes the \\n of a \\r\\n it would cut")
{
ScriptFixture fixture;
put(fixture.root()/"cut.txt","ab\r\ncd\r\r\n");
CHECK(text_of(fixture,"file f; f.open(\"cut.txt\", \"r\"); string a=f.read(3); string p=\"\"+f.position; string b=f.read(1); return a+\"|\"+p+\"|\"+b;")=="ab\n|4|c");
CHECK(text_of(fixture,"file f; f.open(\"cut.txt\", \"r\"); f.seek(4); string a=f.read(4); return a+\"|\"+f.position+\"|\"+f.eof;")=="cd\r\n|9|true");
CHECK(text_of(fixture,"file f; f.open(\"cut.txt\", \"rb\"); return f.read(3)+\"|\"+f.position;")=="ab\r|3");
// Across the 64 KiB buffer: the "\r" is the block's last byte.
std::string bytes(65535,'a');
bytes.append("\r\nz");
put(fixture.root()/"edge.txt",bytes);
CHECK(text_of(fixture,"file f; f.open(\"edge.txt\", \"r\"); string a=f.read(65536); return a.byte_length()+\",\"+a.substr(65534)+\",\"+f.position+\",\"+f.read();")=="65536,a\n,65537,z");
CHECK(text_of(fixture,"file f; f.open(\"edge.txt\", \"r\"); string a=f.read_line(); return a.byte_length()+\",\"+f.position+\",\"+f.read_line();")=="65535,65537,z");
}

TEST_CASE("read_line returns each line without its end, then \"\" and eof")
{
ScriptFixture fixture;
CHECK(text_of(fixture,"write_file(\"l.txt\", \"x\\ny\"); file f; f.open(\"l.txt\", \"r\"); string a=f.read_line(); string b=f.read_line(); string c=f.read_line(); return a+\",\"+b+\",[\"+c+\"],\"+f.eof;")=="x,y,[],true");
CHECK(text_of(fixture,"write_file(\"l.txt\", \"one\\r\\ntwo\\r\\n\\r\\nthree\\r\"); file f; f.open(\"l.txt\", \"rb\"); string s; while(!f.eof)s+=\"[\"+f.read_line()+\"]\"; return s;")=="[one][two][][three\r]");
// A line longer than the buffer, with a "\r\n" cut by the block edge.
std::string bytes(70000,'q');
bytes[65535]='\r';
bytes.insert(65536,"\n");
bytes.append("\nlast");
put(fixture.root()/"long.txt",bytes);
CHECK(text_of(fixture,"file f; f.open(\"long.txt\", \"r\"); string a=f.read_line(); string b=f.read_line(); string c=f.read_line(); return a.byte_length()+\",\"+b.byte_length()+\",\"+c+\",\"+f.eof;")=="65535,4464,last,true");
}

TEST_CASE("the file object's misuse and failures have exact texts")
{
ScriptFixture fixture;
CHECK(error_of(fixture,"file f; f.read();")=="file is not open");
CHECK(error_of(fixture,"file f; f.read(3);")=="file is not open");
CHECK(error_of(fixture,"file f; f.read_line();")=="file is not open");
CHECK(error_of(fixture,"file f; f.write(\"x\");")=="file is not open");
CHECK(error_of(fixture,"file f; f.seek(0);")=="file is not open");
CHECK(error_of(fixture,"write_file(\"r.txt\", \"x\"); file f; f.open(\"r.txt\", \"r\"); f.close(); f.read();")=="file is not open");
CHECK(error_of(fixture,"write_file(\"r.txt\", \"x\"); file f; f.open(\"r.txt\", \"rb\"); f.write(\"y\");")=="file is not open for writing");
CHECK(error_of(fixture,"file f; f.open(\"r.txt\", \"rw\");")=="\"rw\" is not a file mode");
CHECK(error_of(fixture,"file f; f.open(\"r.txt\", \"w+\");")=="\"w+\" is not a file mode");
CHECK(error_of(fixture,"file f; f.open(\"r.txt\", \"\");")=="\"\" is not a file mode");
CHECK(text_of(fixture,"file f; bool opened=f.open(\"nope.txt\", \"r\"); return opened+\",\"+f.is_open+\",\"+f.error;")=="false,false,cannot read \"nope.txt\": file not found");
CHECK(text_of(fixture,"file f; f.open(\"nope.txt\", \"r+\"); return f.error;")=="cannot read \"nope.txt\": file not found");
CHECK(text_of(fixture,"file f; f.open(\"con\", \"w\"); return f.error;")=="\"con\" is not a valid file name");
CHECK(text_of(fixture,"file f; f.open(\"../x\", \"r\"); return f.error;")=="\"../x\" leaves its mount");
CHECK(text_of(fixture,"file f; f.open(\"\", \"r\"); return f.error;")=="cannot read \"\": is a directory");
// A closed file: no size, no position, at its end; seeking outside the file changes nothing.
CHECK(text_of(fixture,"file f; return f.size+\",\"+f.position+\",\"+f.eof+\",\"+f.is_open;")=="0,0,true,false");
CHECK(text_of(fixture,"write_file(\"s.txt\", \"abc\"); file f; f.open(\"s.txt\", \"r\"); f.seek(1); bool a=f.seek(-1); bool b=f.seek(4); bool c=f.seek(3); return a+\",\"+b+\",\"+c+\",\"+f.position;")=="false,false,true,3");
CHECK(text_of(fixture,"write_file(\"s.txt\", \"abc\"); file f; f.open(\"s.txt\", \"r\"); return \"[\"+f.read(0)+f.read(-5)+\"]\"+f.read(100)+\"[\"+f.read()+f.read(1)+f.read_line()+\"]\";")=="[]abc[]");
// Opening again closes the first file.
CHECK(text_of(fixture,"write_file(\"1.txt\", \"one\"); write_file(\"2.txt\", \"two\"); file f; f.open(\"1.txt\", \"r\"); f.read(1); f.open(\"2.txt\", \"r\"); return f.read()+\",\"+f.position;")=="two,3");
}

TEST_CASE("read_file returns the bytes or throws")
{
ScriptFixture fixture;
CHECK(error_of(fixture,"read_file(\"missing.txt\");")=="cannot read \"missing.txt\": file not found");
CHECK(error_of(fixture,"read_file(\"sounds/\");")=="cannot read \"sounds/\": is a directory");
CHECK(error_of(fixture,"read_file(\"nul.txt\");")=="\"nul.txt\" is not a valid file name");
put(fixture.root()/"b.bin",std::string("\r\n\0\xFF",4));
CHECK(text_of(fixture,"return read_file(\"b.bin\");")==std::string("\r\n\0\xFF",4));
put(fixture.root()/"empty.txt","");
CHECK(text_of(fixture,"return \"[\"+read_file(\"empty.txt\")+\"]\";")=="[]");
// BGT-style backslashes and any case.
put(fixture.root()/"Sounds"/"Shot.ogg","bang");
CHECK(text_of(fixture,"return read_file(\"sounds\\\\shot.OGG\");")=="bang");
}

TEST_CASE("the bool helpers set last_file_error on failure and clear it on success")
{
ScriptFixture fixture;
CHECK(int64_of(fixture,"return file_size(\"missing\");")==-1);
CHECK(text_of(fixture,"bool written=write_file(\"con\", \"x\"); return written+\",\"+last_file_error();")=="false,\"con\" is not a valid file name");
CHECK(text_of(fixture,"write_file(\"con\", \"x\"); bool written=write_file(\"ok.txt\", \"x\"); return written+\",[\"+last_file_error()+\"]\";")=="true,[]");
CHECK(text_of(fixture,"bool deleted=delete_file(\"gone.txt\"); return deleted+\",\"+last_file_error();")=="false,cannot delete \"gone.txt\": file not found");
// Queries never touch it.
CHECK(text_of(fixture,"delete_file(\"gone.txt\"); file_exists(\"ok.txt\"); directory_exists(\"x\"); file_size(\"ok.txt\"); return last_file_error();")=="cannot delete \"gone.txt\": file not found");
CHECK(text_of(fixture,"return file_exists(\"ok.txt\")+\",\"+file_exists(\"OK.TXT\")+\",\"+directory_exists(\"ok.txt\")+\",\"+file_exists(\"\")+\",\"+directory_exists(\"\")+\",\"+file_exists(\"con\");")=="true,true,false,false,true,false");
CHECK(int64_of(fixture,"return file_size(\"ok.txt\");")==1);
CHECK(int64_of(fixture,"create_directory(\"d\"); return file_size(\"d\");")==-1);
CHECK(text_of(fixture,"bool a=append_file(\"ap.txt\", \"one\"); bool b=append_file(\"ap.txt\", \"two\"); return a+\",\"+b+\",\"+read_file(\"ap.txt\");")=="true,true,onetwo");
CHECK(text_of(fixture,"write_file(\"ap.txt\", \"x\"); return read_file(\"ap.txt\");")=="x");
CHECK(text_of(fixture,"bool deleted=delete_file(\"ap.txt\"); return deleted+\",\"+file_exists(\"ap.txt\")+\",[\"+last_file_error()+\"]\";")=="true,false,[]");
CHECK(text_of(fixture,"bool written=write_file(\"no/such/folder.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"no/such/folder.txt\": file not found");
CHECK(text_of(fixture,"bool written=append_file(\"no/such/folder.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"no/such/folder.txt\": file not found");
CHECK(text_of(fixture,"file f; f.open(\"no/such/folder.txt\", \"w\"); string a=f.error; f.open(\"no/such/folder.txt\", \"a\"); return a+\"|\"+f.error;")=="cannot write \"no/such/folder.txt\": file not found|cannot write \"no/such/folder.txt\": file not found");
CHECK(text_of(fixture,"file f; f.open(\"d\", \"r+\"); return f.error;")=="cannot write \"d\": is a directory");
}

TEST_CASE("folders are made, listed and removed")
{
ScriptFixture fixture;
CHECK(holds(fixture,"return create_directory(\"a/b/c\")&&directory_exists(\"A/B/c\")&&create_directory(\"a/b/c\");"));
CHECK(text_of(fixture,"bool made=create_directory(\"x/y\", false); return made+\",\"+last_file_error();")=="false,cannot create \"x/y\": file not found");
CHECK(holds(fixture,"return create_directory(\"x\", false)&&create_directory(\"x/y\", false);"));
CHECK(text_of(fixture,"write_file(\"a/b/f.txt\", \"x\"); bool removed=delete_directory(\"a\"); return removed+\",\"+last_file_error();")=="false,cannot delete \"a\": directory not empty");
CHECK(holds(fixture,"return delete_directory(\"a\", true)&&!directory_exists(\"a\")&&delete_directory(\"x/y\")&&last_file_error()==\"\";"));
CHECK(text_of(fixture,"return join(list_directories(\"\"), \"|\");")=="x");
}

TEST_CASE("list_files and list_directories give sorted names")
{
ScriptFixture fixture;
// Made before the Vfs first lists the folder: a change made by another process may take FolderSource's racy window to be seen.
put(fixture.root()/"a.txt","1");
put(fixture.root()/"B.txt","2");
put(fixture.root()/"c.log","3");
put(fixture.root()/"sub"/"inner.txt","4");
fs::create_directories(fixture.root()/"Zeta");
CHECK(text_of(fixture,"return join(list_files(\"\", \"*.TXT\"), \"|\");")=="a.txt|B.txt");
CHECK(text_of(fixture,"return join(list_files(\"/\"), \"|\");")=="a.txt|B.txt|c.log");
CHECK(text_of(fixture,"return join(list_files(\"\", \"?.*\"), \"|\");")=="a.txt|B.txt|c.log");
CHECK(text_of(fixture,"return join(list_directories(\"\"), \"|\");")=="sub|Zeta");
CHECK(text_of(fixture,"create_directory(\"x\"); return join(list_directories(\"/\"), \"|\");")=="sub|x|Zeta");
CHECK(text_of(fixture,"return join(list_files(\"SUB\"), \"|\");")=="inner.txt");
CHECK(text_of(fixture,"array<string>@ names=list_files(\"nowhere\"); return names.length()+\",\"+last_file_error();")=="0,cannot read \"nowhere\": file not found");
CHECK(text_of(fixture,"list_files(\"nowhere\"); list_files(\"\"); return \"[\"+last_file_error()+\"]\";")=="[]");
}

TEST_CASE("copy_file and move_file overwrite, within and across / and data:/")
{
ScriptFixture fixture;
CHECK(holds(fixture,"return write_file(\"src.txt\", \"payload\")&&copy_file(\"src.txt\", \"data:/saved.txt\")&&read_file(\"data:/saved.txt\")==\"payload\"&&read_file(\"src.txt\")==\"payload\";"));
CHECK(contents(fixture.data()/"saved.txt")=="payload");
// Overwriting a longer target cuts it to length.
CHECK(holds(fixture,"return write_file(\"long.txt\", \"a much longer text\")&&copy_file(\"src.txt\", \"long.txt\")&&read_file(\"long.txt\")==\"payload\";"));
// Onto itself, under another spelling: kept whole.
CHECK(holds(fixture,"return copy_file(\"src.txt\", \"SRC.TXT\")&&read_file(\"src.txt\")==\"payload\";"));
CHECK(holds(fixture,"return move_file(\"src.txt\", \"Src.txt\")&&read_file(\"src.txt\")==\"payload\";"));
// Across sources: a copy, then a delete.
CHECK(holds(fixture,"return move_file(\"data:/saved.txt\", \"back.txt\")&&!file_exists(\"data:/saved.txt\")&&read_file(\"back.txt\")==\"payload\";"));
CHECK_FALSE(fs::exists(fixture.data()/"saved.txt"));
CHECK(holds(fixture,"return move_file(\"back.txt\", \"data:/again.txt\")&&!file_exists(\"back.txt\")&&read_file(\"data:/again.txt\")==\"payload\";"));
// Within one source: a rename, over an existing file, into another folder, and seen at once by listings.
CHECK(holds(fixture,"return write_file(\"old.txt\", \"new bytes\")&&create_directory(\"dir\")&&write_file(\"dir/target.txt\", \"stale\")&&join(list_files(\"dir\"), \"|\")==\"target.txt\";"));
CHECK(holds(fixture,"return move_file(\"old.txt\", \"DIR/Target.txt\")&&!file_exists(\"old.txt\")&&read_file(\"dir/target.txt\")==\"new bytes\"&&join(list_files(\"dir\"), \"|\")==\"target.txt\";"));
CHECK(contents(fixture.root()/"dir"/"target.txt")=="new bytes");
CHECK(holds(fixture,"return move_file(\"dir/target.txt\", \"dir/renamed.txt\")&&join(list_files(\"dir\"), \"|\")==\"renamed.txt\";"));
// Failures name the path that failed.
CHECK(text_of(fixture,"bool copied=copy_file(\"none.txt\", \"x.txt\"); return copied+\",\"+last_file_error();")=="false,cannot read \"none.txt\": file not found");
CHECK(text_of(fixture,"bool moved=move_file(\"none.txt\", \"x.txt\"); return moved+\",\"+last_file_error();")=="false,cannot read \"none.txt\": file not found");
CHECK(text_of(fixture,"bool copied=copy_file(\"data:/again.txt\", \"aux.txt\"); return copied+\",\"+last_file_error();")=="false,\"aux.txt\" is not a valid file name");
CHECK(text_of(fixture,"bool moved=move_file(\"dir\", \"x.txt\"); return moved+\",\"+last_file_error();")=="false,cannot read \"dir\": is a directory");
CHECK(text_of(fixture,"bool moved=move_file(\"data:/again.txt\", \"dir\"); return moved+\",\"+last_file_error();")=="false,cannot write \"dir\": is a directory");
CHECK(text_of(fixture,"bool copied=copy_file(\"data:/again.txt\", \"dir\"); return copied+\",\"+last_file_error();")=="false,cannot write \"dir\": is a directory");
CHECK(text_of(fixture,"bool moved=move_file(\"data:/again.txt\", \"no/such/b.txt\"); return moved+\",\"+last_file_error();")=="false,cannot write \"no/such/b.txt\": file not found");
CHECK(text_of(fixture,"write_file(\"a.txt\", \"a\"); bool moved=move_file(\"a.txt\", \"no/such/b.txt\"); return moved+\",\"+last_file_error()+\",\"+file_exists(\"a.txt\");")=="false,cannot write \"no/such/b.txt\": file not found,true");
CHECK(text_of(fixture,"bool moved=move_file(\"a.txt\", \"dir\"); return moved+\",\"+last_file_error();")=="false,cannot write \"dir\": is a directory");
CHECK(text_of(fixture,"bool moved=move_file(\"a.txt\", \"../b.txt\"); return moved+\",\"+last_file_error();")=="false,\"../b.txt\" leaves its mount");
CHECK(holds(fixture,"return file_exists(\"data:/again.txt\");"));
}

TEST_CASE("Vfs::rename renames within a source and between host folders, and never out of a read-only mount")
{
ScriptFixture fixture;
put(fixture.root()/"a.txt","a");
put(fixture.folder/"ro"/"x.txt","x");
REQUIRE(fixture.vfs.mount_folder("ro",fixture.folder/"ro",false));
const auto within=fixture.vfs.rename("a.txt","b.txt");
REQUIRE(within.has_value());
CHECK(*within);
CHECK(contents(fixture.root()/"b.txt")=="a");
CHECK_FALSE(fs::exists(fixture.root()/"a.txt"));
// Two host folders: the system renames across them, and both forget what they cached.
CHECK(holds(fixture,"return file_exists(\"b.txt\")&&!file_exists(\"data:/b.txt\")&&join(list_files(\"/\"), \"|\")==\"b.txt\";"));
const auto across=fixture.vfs.rename("b.txt","data:/b.txt");
REQUIRE(across.has_value());
CHECK(*across);
CHECK_FALSE(fs::exists(fixture.root()/"b.txt"));
CHECK(contents(fixture.data()/"b.txt")=="a");
CHECK(holds(fixture,"return !file_exists(\"b.txt\")&&file_exists(\"data:/b.txt\")&&join(list_files(\"/\"), \"|\")==\"\"&&join(list_files(\"data:/\"), \"|\")==\"b.txt\";"));
const auto read_only=fixture.vfs.rename("ro/x.txt","y.txt");
REQUIRE_FALSE(read_only.has_value());
CHECK(read_only.error().failure.code==zodiac::vfs::FileError::read_only_pack);
CHECK_FALSE(read_only.error().to);
CHECK(text_of(fixture,"bool moved=move_file(\"ro/x.txt\", \"y.txt\"); return moved+\",\"+file_exists(\"ro/x.txt\")+\",\"+file_exists(\"y.txt\")+\",\"+last_file_error();")=="false,true,false,\"ro/x.txt\" is in a read-only pack; save files in data:/ instead");
CHECK(text_of(fixture,"bool copied=copy_file(\"ro/x.txt\", \"y.txt\"); return copied+\",\"+read_file(\"y.txt\");")=="true,x");
}

TEST_CASE("file_modified is local civil time, on date_time::now()'s clock")
{
ScriptFixture fixture;
REQUIRE(holds(fixture,"return write_file(\"m.txt\", \"x\");"));
const auto info=fixture.vfs.stat("m.txt");
REQUIRE(info.has_value());
// date_time() is 1970-01-01 00:00, so the difference is the civil milliseconds themselves.
CHECK(int64_of(fixture,"return file_modified(\"m.txt\")-date_time();")==zodiac::platform::utc_to_local_civil_ms(info->modified_utc_ms));
// Written just before: within seconds of now() (not of utc_now(), unless the zone is UTC).
const std::int64_t behind=int64_of(fixture,"return date_time::now()-file_modified(\"m.txt\");");
CHECK(behind>-5000);
CHECK(behind<60000);
CHECK(int64_of(fixture,"create_directory(\"folder\"); return date_time::now()-file_modified(\"folder\");")<60000);
CHECK(error_of(fixture,"file_modified(\"missing.txt\");")=="cannot read \"missing.txt\": file not found");
CHECK(error_of(fixture,"file_modified(\"prn\");")=="\"prn\" is not a valid file name");
}

TEST_CASE("reads at the buffer's edges give the file's bytes")
{
ScriptFixture fixture;
for(const std::size_t size:{std::size_t{0},std::size_t{1},std::size_t{65535},std::size_t{65536},std::size_t{65537},std::size_t{3*65536+7}}){
CAPTURE(size);
const std::string bytes=pattern(size);
put(fixture.root()/"edge.bin",bytes);
CHECK(text_of(fixture,"return read_file(\"edge.bin\");")==bytes);
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); return f.read();")==bytes);
// In small pieces, through the buffer.
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); string s; while(!f.eof)s+=f.read(1000); return s;")==bytes);
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); string s; while(!f.eof)s+=f.read(65536); return s;")==bytes);
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); return f.read(7)+f.read(70000)+f.read();")==bytes);
}
const std::string bytes=pattern(3*65536+7);
put(fixture.root()/"edge.bin",bytes);
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); f.seek(65535); return f.read(3);")==bytes.substr(65535,3));
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); f.seek(65536); string a=f.read(1); f.seek(65535); return a+f.read(2);")==bytes.substr(65536,1)+bytes.substr(65535,2));
// One read over three blocks, after a small one filled the buffer.
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); f.read(10); f.seek(65530); return f.read(2*65536+20);")==bytes.substr(65530,2*65536+20));
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"rb\"); f.seek(3*65536); return f.read(100)+\"|\"+f.eof;")==bytes.substr(3*65536)+"|true");
// Reads after writes see the written bytes, in the buffer and beyond it.
CHECK(text_of(fixture,"file f; f.open(\"edge.bin\", \"r+b\"); f.read(5); f.seek(2); f.write(\"XYZ\"); f.seek(0); string head=f.read(6); f.seek(f.size); f.write(\"tail\"); f.seek(f.size-6); return head+\"|\"+f.read();")==bytes.substr(0,2)+"XYZ"+bytes.substr(5,1)+"|"+bytes.substr(bytes.size()-2)+"tail");
CHECK(text_of(fixture,"file f; f.open(\"grow.bin\", \"wb\"); f.write(\"ab\"); f.seek(0); string a=f.read(1); f.seek(2); f.write(\"cd\"); f.seek(1); return a+f.read();")=="abcd");
}

TEST_CASE("a sparse file over 4 GiB is read at its end")
{
ScriptFixture fixture;
const fs::path file=fixture.root()/"big.bin";
const RemoveAtExit cleanup(file);
constexpr std::uint64_t offset=5368709120;
make_sparse(file,offset,"END");
CHECK(int64_of(fixture,"return file_size(\"big.bin\");")==5368709123);
CHECK(text_of(fixture,"file f; f.open(\"big.bin\", \"rb\"); string s=f.size+\",\"; f.seek(5368709120); s+=f.read(3); return s+\",\"+f.position+\",\"+f.eof;")=="5368709123,END,5368709123,true");
CHECK(text_of(fixture,"file f; f.open(\"big.bin\", \"rb\"); f.seek(5368709119); return f.read(10);")==std::string("\0END",4));
}

TEST_CASE("failure texts name what failed: read, write, delete or create")
{
using zodiac::vfs::Failure;
using zodiac::vfs::FileAction;
using zodiac::vfs::FileError;
CHECK(zodiac::vfs::failure_text(FileAction::read,"a",Failure{FileError::not_found,{}})=="cannot read \"a\": file not found");
CHECK(zodiac::vfs::failure_text(FileAction::write,"a",Failure{FileError::disk_full,{}})=="cannot write \"a\": disk full");
CHECK(zodiac::vfs::failure_text(FileAction::remove,"a",Failure{FileError::access_denied,{}})=="cannot delete \"a\": access denied");
CHECK(zodiac::vfs::failure_text(FileAction::create,"a\nb",Failure{FileError::not_found,{}})=="cannot create \"a\\nb\": file not found");
CHECK(zodiac::vfs::write_failure_text("a",Failure{FileError::io,{}})=="cannot write \"a\": input/output error");
// A text of its own is never wrapped.
CHECK(zodiac::vfs::write_failure_text("con",Failure{FileError::bad_path,"\"con\" is not a valid file name"})=="\"con\" is not a valid file name");
// A full disk is named on every platform.
CHECK(zodiac::vfs::detail::file_error(std::error_code(ENOSPC,std::generic_category()))==FileError::disk_full);
#ifdef _WIN32
CHECK(zodiac::vfs::detail::file_error(std::error_code(ERROR_DISK_FULL,std::system_category()))==FileError::disk_full);
CHECK(zodiac::vfs::detail::file_error(std::error_code(ERROR_HANDLE_DISK_FULL,std::system_category()))==FileError::disk_full);
#endif
CHECK(zodiac::vfs::read_error(-static_cast<std::int64_t>(FileError::disk_full))==FileError::disk_full);
CHECK(zodiac::vfs::read_error(-1000)==FileError::io);
}

TEST_CASE("failed writes and commits are reported with their reason")
{
ScriptFixture fixture;
auto memory=std::make_shared<MemorySource>();
REQUIRE(fixture.vfs.mount("mem",memory,true,nullptr));
memory->fail_write=zodiac::vfs::FileError::disk_full;
CHECK(text_of(fixture,"file f; if(!f.open(\"mem/w.txt\", \"w\"))return f.error; bool written=f.write(\"x\"); return written+\",\"+f.error;")=="false,cannot write \"mem/w.txt\": disk full");
CHECK(text_of(fixture,"bool written=write_file(\"mem/w.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"mem/w.txt\": disk full");
CHECK(text_of(fixture,"bool written=append_file(\"mem/w.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"mem/w.txt\": disk full");
CHECK(text_of(fixture,"write_file(\"x.txt\", \"bytes\"); bool copied=copy_file(\"x.txt\", \"mem/c.txt\"); return copied+\",\"+last_file_error();")=="false,cannot write \"mem/c.txt\": disk full");
CHECK(text_of(fixture,"bool moved=move_file(\"x.txt\", \"mem/c.txt\"); return moved+\",\"+last_file_error()+\",\"+file_exists(\"x.txt\");")=="false,cannot write \"mem/c.txt\": disk full,true");
// An empty write never reaches the target.
CHECK(holds(fixture,"file f; f.open(\"mem/w.txt\", \"w\"); return f.write(\"\");"));
memory->fail_write.reset();
memory->fail_commit=zodiac::vfs::FileError::access_denied;
CHECK(text_of(fixture,"file f; f.open(\"mem/c.txt\", \"w\"); bool written=f.write(\"x\"); string before=f.error; f.close(); return written+\",[\"+before+\"],\"+f.error+\",\"+f.is_open;")=="true,[],cannot write \"mem/c.txt\": access denied,false");
CHECK(text_of(fixture,"bool written=write_file(\"mem/c.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"mem/c.txt\": access denied");
CHECK(text_of(fixture,"bool copied=copy_file(\"x.txt\", \"mem/c.txt\"); return copied+\",\"+last_file_error();")=="false,cannot write \"mem/c.txt\": access denied");
CHECK_FALSE(memory->files.contains("c.txt"));
memory->fail_commit.reset();
CHECK(holds(fixture,"return write_file(\"mem/c.txt\", \"stored\")&&read_file(\"mem/c.txt\")==\"stored\";"));
CHECK(memory->files.at("c.txt")=="stored");
}

TEST_CASE("copies and moves through a source without host files take the buffered path")
{
ScriptFixture fixture;
auto memory=std::make_shared<MemorySource>();
REQUIRE(fixture.vfs.mount("mem",memory,true,nullptr));
// More than one 256 KiB piece.
const std::string bytes=pattern(5*65536+7);
put(fixture.root()/"big.bin",bytes);
CHECK(holds(fixture,"return copy_file(\"big.bin\", \"mem/big.bin\");"));
CHECK(memory->files.at("big.bin")==bytes);
CHECK(holds(fixture,"return copy_file(\"mem/big.bin\", \"copy.bin\");"));
CHECK(contents(fixture.root()/"copy.bin")==bytes);
// Over a longer target: cut to length.
memory->files["long.txt"]="a much longer text";
CHECK(holds(fixture,"return write_file(\"short.txt\", \"short\")&&copy_file(\"short.txt\", \"mem/long.txt\");"));
CHECK(memory->files.at("long.txt")=="short");
// A move is a copy, then a delete.
CHECK(holds(fixture,"return move_file(\"mem/big.bin\", \"moved.bin\")&&!file_exists(\"mem/big.bin\");"));
CHECK_FALSE(memory->files.contains("big.bin"));
CHECK(contents(fixture.root()/"moved.bin")==bytes);
CHECK(holds(fixture,"return move_file(\"moved.bin\", \"mem/back.bin\")&&!file_exists(\"moved.bin\");"));
CHECK(memory->files.at("back.bin")==bytes);
CHECK_FALSE(fs::exists(fixture.root()/"moved.bin"));
CHECK(text_of(fixture,"bool copied=copy_file(\"mem/none.bin\", \"x.bin\"); return copied+\",\"+last_file_error();")=="false,cannot read \"mem/none.bin\": file not found");
// The copy succeeds and removing the source fails: a delete failure, and both files are there.
memory->fail_remove=zodiac::vfs::FileError::access_denied;
CHECK(text_of(fixture,"bool moved=move_file(\"mem/back.bin\", \"stuck.bin\"); return moved+\",\"+last_file_error()+\",\"+file_exists(\"mem/back.bin\")+\",\"+file_exists(\"stuck.bin\");")=="false,cannot delete \"mem/back.bin\": access denied,true,true");
CHECK(contents(fixture.root()/"stuck.bin")==bytes);
}

TEST_CASE("copies between host files use the system's copy and give the same bytes and a fresh time")
{
ScriptFixture fixture;
const std::string bytes=pattern(3*65536+7);
put(fixture.root()/"big.bin",bytes);
// A day old: a copy is a new file, on every path.
const fs::path old=fixture.root()/"big.bin";
fs::last_write_time(old,fs::last_write_time(old)-std::chrono::hours(24));
CHECK(int64_of(fixture,"return date_time::now()-file_modified(\"big.bin\");")>23*3600000);
CHECK(holds(fixture,"return copy_file(\"big.bin\", \"data:/big.bin\")&&copy_file(\"big.bin\", \"sub/../copy.bin\");"));
CHECK(contents(fixture.data()/"big.bin")==bytes);
CHECK(contents(fixture.root()/"copy.bin")==bytes);
CHECK(int64_of(fixture,"return date_time::now()-file_modified(\"data:/big.bin\");")<60000);
CHECK(int64_of(fixture,"return date_time::now()-file_modified(\"copy.bin\");")<60000);
// The copy is seen at once by listings, and is a file of its own.
CHECK(text_of(fixture,"return join(list_files(\"data:/\"), \"|\");")=="big.bin");
CHECK(holds(fixture,"return write_file(\"data:/big.bin\", \"changed\")&&read_file(\"big.bin\").byte_length()==196615;"));
// Over a longer file, and an empty one.
CHECK(holds(fixture,"write_file(\"e.txt\", \"\"); return copy_file(\"e.txt\", \"copy.bin\")&&read_file(\"copy.bin\")==\"\";"));
// platform::copy_file itself, and which side it blames.
auto direct=zodiac::platform::copy_file(old,fixture.root()/"direct.bin");
CHECK_FALSE(direct.error);
CHECK_FALSE(direct.same);
CHECK_FALSE(direct.metadata);
CHECK(contents(fixture.root()/"direct.bin")==bytes);
// Over a longer file: emptied first, after the identities were compared.
put(fixture.root()/"longer.bin",bytes+bytes);
CHECK_FALSE(zodiac::platform::copy_file(old,fixture.root()/"longer.bin").error);
CHECK(contents(fixture.root()/"longer.bin")==bytes);
// Onto itself: nothing written.
direct=zodiac::platform::copy_file(old,old);
CHECK(direct.same);
CHECK_FALSE(direct.error);
CHECK(contents(old)==bytes);
direct=zodiac::platform::copy_file(fixture.root()/"none.bin",fixture.root()/"x.bin");
CHECK(direct.error);
CHECK(direct.from_failed);
direct=zodiac::platform::copy_file(old,fixture.root()/"no"/"such"/"x.bin");
CHECK(direct.error);
CHECK_FALSE(direct.from_failed);
}

TEST_CASE("one file under two paths is never copied onto itself or moved away")
{
ScriptFixture fixture;
fixture.vfs.set_host_paths(true);
REQUIRE(holds(fixture,"return write_file(\"save.txt\", \"keep me\");"));
const std::string host="host:"+utf8(fixture.root()/"save.txt");
CAPTURE(host);
// A host path and the mounted path of one file.
CHECK(holds(fixture,"return move_file(\""+host+"\", \"save.txt\")&&move_file(\"save.txt\", \""+host+"\");"));
CHECK(holds(fixture,"return copy_file(\""+host+"\", \"save.txt\")&&copy_file(\"save.txt\", \""+host+"\");"));
CHECK(contents(fixture.root()/"save.txt")=="keep me");
// Two mounts of one folder.
REQUIRE(fixture.vfs.mount_folder("alias",fixture.root(),true));
CHECK(holds(fixture,"return read_file(\"alias/save.txt\")==\"keep me\";"));
CHECK(holds(fixture,"return move_file(\"alias/save.txt\", \"save.txt\")&&move_file(\"save.txt\", \"alias/SAVE.TXT\")&&copy_file(\"alias/save.txt\", \"save.txt\");"));
CHECK(contents(fixture.root()/"save.txt")=="keep me");
// One entry under two spellings.
CHECK(holds(fixture,"return copy_file(\"save.txt\", \"SAVE.txt\")&&move_file(\"SAVE.TXT\", \"save.txt\")&&read_file(\"save.txt\")==\"keep me\";"));
CHECK(contents(fixture.root()/"save.txt")=="keep me");
// A real move between a host path and a mount.
put(fixture.folder/"outside.txt","from outside");
const std::string outside="host:"+utf8(fixture.folder/"outside.txt");
CHECK(holds(fixture,"return move_file(\""+outside+"\", \"inside.txt\")&&read_file(\"inside.txt\")==\"from outside\"&&!file_exists(\""+outside+"\");"));
CHECK_FALSE(fs::exists(fixture.folder/"outside.txt"));
}

TEST_CASE("a bad mode leaves the open file, read(0) leaves the BOM, and \"a\" reads")
{
ScriptFixture fixture;
CHECK(text_of(fixture,"write_file(\"keep.txt\", \"kept\"); file f; f.open(\"keep.txt\", \"r\"); f.read(1); string text; try{f.open(\"other.txt\", \"q\");}catch{text=exception_info();} return text+\",\"+f.is_open+\",\"+f.position+\",\"+f.read();")=="\"q\" is not a file mode,true,1,ept");
put(fixture.root()/"bom.txt","\xEF\xBB\xBFhi\r\n");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); string a=f.read(0); string p=\"\"+f.position; string b=f.read(1); return \"[\"+a+\"]\"+p+\",\"+b+\",\"+f.position;")=="[]0,h,4");
CHECK(text_of(fixture,"file f; f.open(\"bom.txt\", \"r\"); string p=\"\"+f.position+f.eof; string line=f.read_line(); return p+\",\"+line+\",\"+f.position;")=="0false,hi,7");
CHECK(text_of(fixture,"write_file(\"am.txt\", \"one\"); file f; f.open(\"am.txt\", \"a\"); string a=f.read(); f.write(\"two\"); string p=\"\"+f.position; f.seek(0); return a+\"|\"+p+\"|\"+f.read()+\"|\"+f.position;")=="one|6|onetwo|6");
CHECK(text_of(fixture,"write_file(\"ab.txt\", \"x\\r\\ny\"); file f; f.open(\"ab.txt\", \"ab\"); return f.read(2)+\"|\"+f.read_line()+\"|\"+f.read();")=="x\r||y");
}

TEST_CASE("a link moved or copied onto its own target leaves it whole, and a moved link is the link itself")
{
ScriptFixture fixture;
const fs::path root=fixture.root();
put(root/"a.txt","target bytes");
// A relative file link, made without administrator rights where Windows allows it (Developer Mode).
#ifdef _WIN32
const bool linked=CreateSymbolicLinkW((root/"link").c_str(),L"a.txt",0x2)!=0;
#else
std::error_code link_error;
fs::create_symlink("a.txt",root/"link",link_error);
const bool linked=!link_error;
#endif
if(!linked){
// On CI the runner can make links, so a failure there is a failure, never a silent skip.
const bool on_ci=std::getenv("CI")!=nullptr||std::getenv("GITHUB_ACTIONS")!=nullptr;
REQUIRE_MESSAGE(!on_ci,"the CI runner could not make a symbolic link");
MESSAGE("skipped: this system cannot make a symbolic link without administrator rights");
return;
}
REQUIRE(fs::is_symlink(root/"link"));
MESSAGE("link test ran with a real symbolic link");
CHECK(holds(fixture,"return read_file(\"link\")==\"target bytes\";"));
// One source: renaming the link over its target would leave a link to itself, so nothing is done.
CHECK(text_of(fixture,"bool moved=move_file(\"link\", \"a.txt\"); return moved+\",[\"+last_file_error()+\"]\";")=="true,[]");
CHECK(contents(root/"a.txt")=="target bytes");
CHECK(fs::is_symlink(root/"link"));
CHECK_FALSE(fs::is_symlink(root/"a.txt"));
CHECK(holds(fixture,"return copy_file(\"link\", \"a.txt\")&&copy_file(\"a.txt\", \"link\")&&move_file(\"a.txt\", \"link\");"));
CHECK(contents(root/"a.txt")=="target bytes");
CHECK(fs::is_symlink(root/"link"));
// The same through host paths (host: to host:, and a host path to the mount).
fixture.vfs.set_host_paths(true);
const std::string link="host:"+utf8(root/"link");
const std::string target="host:"+utf8(root/"a.txt");
CHECK(holds(fixture,"return move_file(\""+link+"\", \""+target+"\")&&move_file(\""+link+"\", \"a.txt\")&&copy_file(\""+link+"\", \""+target+"\");"));
CHECK(contents(root/"a.txt")=="target bytes");
CHECK(fs::is_symlink(root/"link"));
// platform::copy_file follows the link and sees one file.
const auto copied=zodiac::platform::copy_file(root/"link",root/"a.txt");
CHECK(copied.same);
CHECK_FALSE(copied.error);
CHECK(contents(root/"a.txt")=="target bytes");
// Moved elsewhere, a link moves itself (on Windows too), never what it leads to.
CHECK(holds(fixture,"return move_file(\"link\", \"moved_link\")&&read_file(\"moved_link\")==\"target bytes\"&&!file_exists(\"link\");"));
CHECK(fs::is_symlink(root/"moved_link"));
CHECK_FALSE(fs::exists(fs::symlink_status(root/"link")));
CHECK_FALSE(fs::is_symlink(root/"a.txt"));
CHECK(contents(root/"a.txt")=="target bytes");
// A replacing write or copy through a link changes the file it names, as a write in place does, and the link stays.
CHECK(holds(fixture,"return write_file(\"moved_link\", \"new bytes\")&&read_file(\"a.txt\")==\"new bytes\";"));
CHECK(fs::is_symlink(root/"moved_link"));
CHECK(holds(fixture,"return write_file(\"c.txt\", \"copied\")&&copy_file(\"c.txt\", \"moved_link\")&&read_file(\"a.txt\")==\"copied\";"));
CHECK(fs::is_symlink(root/"moved_link"));
}

namespace {
// The temporary files (temp_prefix) in folder, as the system lists them.
std::vector<std::string> temp_files(const fs::path& folder)
{
std::vector<std::string> names;
std::error_code error;
for(const auto& entry:fs::directory_iterator(folder,error)){
const std::string name=utf8(entry.path().filename());
if(zodiac::vfs::is_temp_name(name))names.push_back(name);
}
return names;
}
}

TEST_CASE("a failed copy or write_file leaves the target as it was, and no temporary file")
{
ScriptFixture fixture;
const fs::path root=fixture.root();
auto memory=std::make_shared<MemorySource>();
REQUIRE(fixture.vfs.mount("mem",memory,true,nullptr));
const std::string bytes=pattern(5*65536+7);
put(root/"big.bin",bytes);
put(root/"target.bin","the old bytes");
// From a source without host files to a host file: the read fails midway, after the first 256 KiB piece was written.
memory->files["big.bin"]=bytes;
memory->fail_read_at=300000;
CHECK(text_of(fixture,"bool copied=copy_file(\"mem/big.bin\", \"target.bin\"); return copied+\",\"+last_file_error();")=="false,cannot read \"mem/big.bin\": damaged data");
CHECK(contents(root/"target.bin")=="the old bytes");
CHECK(temp_files(root).empty());
CHECK(text_of(fixture,"bool moved=move_file(\"mem/big.bin\", \"target.bin\"); return moved+\",\"+file_exists(\"mem/big.bin\");")=="false,true");
CHECK(contents(root/"target.bin")=="the old bytes");
CHECK(temp_files(root).empty());
memory->fail_read_at.reset();
CHECK(holds(fixture,"return copy_file(\"mem/big.bin\", \"target.bin\");"));
CHECK(contents(root/"target.bin")==bytes);
CHECK(temp_files(root).empty());
// To a source without host files: the write fails midway, and the entry keeps its old bytes (nothing is stored before commit).
memory->files["old.bin"]="the old entry";
memory->fail_write_after=300000;
CHECK(text_of(fixture,"bool copied=copy_file(\"big.bin\", \"mem/old.bin\"); return copied+\",\"+last_file_error();")=="false,cannot write \"mem/old.bin\": disk full");
CHECK(memory->files.at("old.bin")=="the old entry");
CHECK(text_of(fixture,"string big=read_file(\"big.bin\"); bool written=write_file(\"mem/old.bin\", big); return written+\",\"+last_file_error();")=="false,cannot write \"mem/old.bin\": disk full");
CHECK(memory->files.at("old.bin")=="the old entry");
memory->fail_write_after.reset();
// write_file replaces a host file at once, leaves no temporary file, and the listing never shows one.
CHECK(holds(fixture,"return write_file(\"target.bin\", \"replaced\")&&read_file(\"target.bin\")==\"replaced\"&&join(list_files(\"\"), \"|\")==\"big.bin|target.bin\";"));
CHECK(contents(root/"target.bin")=="replaced");
CHECK(temp_files(root).empty());
CHECK(holds(fixture,"return write_file(\"data:/save.txt\", \"one\")&&write_file(\"data:/save.txt\", \"two\")&&read_file(\"data:/save.txt\")==\"two\";"));
CHECK(temp_files(fixture.data()).empty());
// A failed write_file to a missing folder makes nothing.
CHECK(text_of(fixture,"bool written=write_file(\"no/such/x.txt\", \"x\"); return written+\",\"+last_file_error();")=="false,cannot write \"no/such/x.txt\": file not found");
#ifndef _WIN32
// A replaced file keeps its permission bits.
fs::permissions(root/"target.bin",fs::perms::owner_read|fs::perms::owner_write);
CHECK(holds(fixture,"return write_file(\"target.bin\", \"private\")&&copy_file(\"big.bin\", \"target.bin\");"));
CHECK((fs::status(root/"target.bin").permissions()&fs::perms::all)==(fs::perms::owner_read|fs::perms::owner_write));
#endif
}

TEST_CASE("temporary names are never visible to scripts, and a crash's leftovers go")
{
ScriptFixture fixture;
const fs::path root=fixture.root();
CHECK(text_of(fixture,"bool written=write_file(\".zodiac-tmp-mine.tmp\", \"x\"); return written+\",\"+last_file_error();")=="false,\".zodiac-tmp-mine.tmp\" is not a valid file name");
CHECK(text_of(fixture,"bool written=write_file(\"sub/.ZODIAC-TMP-x\", \"x\"); return written+\",\"+last_file_error();")=="false,\"sub/.ZODIAC-TMP-x\" is not a valid file name");
CHECK(holds(fixture,"return write_file(\".zodiac-tmpx\", \"not reserved\")&&write_file(\"zodiac-tmp-y\", \"not reserved\");"));
// The generated form is exact: the prefix, 8 hex digits of the creation time, 8 random ones, ".tmp".
CHECK(zodiac::vfs::make_temp_name(0x01234567,0x89abcdef)==".zodiac-tmp-0123456789abcdef.tmp");
CHECK(zodiac::vfs::temp_name_time(".zodiac-tmp-0123456789abcdef.tmp")==std::optional<std::int64_t>(0x01234567));
CHECK_FALSE(zodiac::vfs::temp_name_time(".zodiac-tmp-0123456789ABCDEF.tmp"));
CHECK_FALSE(zodiac::vfs::temp_name_time(".ZODIAC-TMP-0123456789abcdef.tmp"));
CHECK_FALSE(zodiac::vfs::temp_name_time(".zodiac-tmp-0123456789abcde.tmp"));
CHECK_FALSE(zodiac::vfs::temp_name_time(".zodiac-tmp-notes.txt"));
// At the next scan: a leftover made two days ago (by its name) goes, whatever its file time; one made now stays even with an old
// file time (CopyFileExW gives the temporary file its source's time); one this process holds stays however old; a name of another
// form stays, even with the prefix. None is listed.
const auto now_s=zodiac::platform::utc_now_ms()/1000;
const std::string crashed=zodiac::vfs::make_temp_name(now_s-2*86400,1);
const std::string copying=zodiac::vfs::make_temp_name(now_s,2);
const std::string held=zodiac::vfs::make_temp_name(now_s-2*86400,3);
const auto old_time=fs::file_time_type::clock::now()-std::chrono::hours(48);
for(const std::string& name:{crashed,copying,held,std::string(".zodiac-tmp-notes.txt")}){
put(root/name,"temporary");
fs::last_write_time(root/name,old_time);
}
zodiac::vfs::hold_temp(held);
CHECK(text_of(fixture,"return join(list_files(\"/\"), \"|\");")==".zodiac-tmpx|zodiac-tmp-y");
zodiac::vfs::release_temp(held);
CHECK_FALSE(fs::exists(root/crashed));
CHECK(fs::exists(root/copying));
CHECK(fs::exists(root/held));
CHECK(fs::exists(root/".zodiac-tmp-notes.txt"));
CHECK_FALSE(holds(fixture,"return file_exists(\""+copying+"\")||file_exists(\".zodiac-tmp-notes.txt\");"));
}

TEST_CASE("sync flushes saves and open files")
{
ScriptFixture fixture;
auto memory=std::make_shared<MemorySource>();
REQUIRE(fixture.vfs.mount("mem",memory,true,nullptr));
CHECK(holds(fixture,"return write_file(\"s.txt\", \"synced\", true)&&read_file(\"s.txt\")==\"synced\";"));
CHECK(holds(fixture,"return write_file(\"s.txt\", \"again\", true)&&read_file(\"s.txt\")==\"again\";"));
CHECK(holds(fixture,"return copy_file(\"s.txt\", \"data:/s.txt\", true)&&read_file(\"data:/s.txt\")==\"again\";"));
CHECK(holds(fixture,"return move_file(\"data:/s.txt\", \"moved.txt\", true)&&read_file(\"moved.txt\")==\"again\"&&!file_exists(\"data:/s.txt\");"));
CHECK(temp_files(fixture.root()).empty());
// Without host files the flush is the write target's.
CHECK(holds(fixture,"return write_file(\"mem/a.txt\", \"x\", true)&&copy_file(\"s.txt\", \"mem/b.txt\", true)&&move_file(\"moved.txt\", \"mem/c.txt\", true);"));
CHECK(memory->syncs==3);
CHECK(memory->files.at("c.txt")=="again");
CHECK(holds(fixture,"return write_file(\"mem/d.txt\", \"x\")&&copy_file(\"s.txt\", \"mem/e.txt\");"));
CHECK(memory->syncs==3);
// file.sync(): what was written so far reaches the device; nothing to do for "r"; misuse when closed.
CHECK(text_of(fixture,"file f; f.open(\"w.txt\", \"w\"); f.write(\"part\"); bool a=f.sync(); f.write(\" more\"); bool b=f.sync(); f.close(); return a+\",\"+b+\",\"+read_file(\"w.txt\");")=="true,true,part more");
CHECK(holds(fixture,"file f; f.open(\"w.txt\", \"r\"); return f.sync()&&f.error==\"\";"));
CHECK(holds(fixture,"file f; f.open(\"mem/f.txt\", \"w\"); f.write(\"x\"); return f.sync();"));
CHECK(memory->syncs==4);
CHECK(error_of(fixture,"file f; f.sync();")=="file is not open");
CHECK(error_of(fixture,"file f; f.open(\"w.txt\", \"r\"); f.close(); f.sync();")=="file is not open");
}

#ifdef _WIN32
namespace {
// Writes bytes into the alternate data stream name of file.
void put_stream(const fs::path& file,const wchar_t* name,std::string_view bytes)
{
fs::path stream=file;
stream+=name;
const HANDLE handle=CreateFileW(stream.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
REQUIRE(handle!=INVALID_HANDLE_VALUE);
DWORD written=0;
const bool wrote=WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size();
CloseHandle(handle);
REQUIRE(wrote);
}
bool has_stream(const fs::path& file,const wchar_t* name)
{
fs::path stream=file;
stream+=name;
return GetFileAttributesW(stream.c_str())!=INVALID_FILE_ATTRIBUTES;
}
}

TEST_CASE("Windows copies drop the mark of the web, keep other streams, and get past a hidden target")
{
ScriptFixture fixture;
const fs::path root=fixture.root();
put(root/"web.bin","downloaded");
put_stream(root/"web.bin",L":Zone.Identifier","[ZoneTransfer]\r\nZoneId=3\r\n");
put_stream(root/"web.bin",L":extra","kept");
CHECK(holds(fixture,"return copy_file(\"web.bin\", \"copy.bin\")&&read_file(\"copy.bin\")==\"downloaded\";"));
CHECK_FALSE(has_stream(root/"copy.bin",L":Zone.Identifier"));
CHECK(has_stream(root/"copy.bin",L":extra"));
CHECK(has_stream(root/"web.bin",L":Zone.Identifier"));
// CopyFileExW refuses a hidden target; the copy then goes through the sources, as on every other path.
put(root/"hidden.bin","an older and longer text");
REQUIRE(SetFileAttributesW((root/"hidden.bin").c_str(),FILE_ATTRIBUTE_HIDDEN));
const auto refused=zodiac::platform::copy_file(root/"web.bin",root/"hidden.bin");
CHECK(zodiac::platform::error_kind(refused.error)==zodiac::platform::ErrorKind::access_denied);
CHECK_FALSE(refused.from_failed);
CHECK(text_of(fixture,"bool copied=copy_file(\"web.bin\", \"hidden.bin\"); return copied+\",[\"+last_file_error()+\"],\"+read_file(\"hidden.bin\");")=="true,[],downloaded");
REQUIRE(SetFileAttributesW((root/"hidden.bin").c_str(),FILE_ATTRIBUTE_SYSTEM));
CHECK(holds(fixture,"return write_file(\"web2.bin\", \"second\")&&copy_file(\"web2.bin\", \"hidden.bin\")&&read_file(\"hidden.bin\")==\"second\";"));
SetFileAttributesW((root/"hidden.bin").c_str(),FILE_ATTRIBUTE_NORMAL);
// A read-only source: CopyFileExW copies the attribute to the temporary file, which is cleared before the rename.
put(root/"locked.bin","read only");
REQUIRE(SetFileAttributesW((root/"locked.bin").c_str(),FILE_ATTRIBUTE_READONLY));
CHECK(holds(fixture,"return copy_file(\"locked.bin\", \"unlocked.bin\")&&read_file(\"unlocked.bin\")==\"read only\"&&write_file(\"unlocked.bin\", \"writable\");"));
CHECK((GetFileAttributesW((root/"unlocked.bin").c_str())&FILE_ATTRIBUTE_READONLY)==0);
CHECK(temp_files(root).empty());
// A read-only target cannot be replaced (as it cannot be written in place): the copy fails, the target and source stay, and the
// read-only temporary file is still removed.
CHECK(text_of(fixture,"bool copied=copy_file(\"web.bin\", \"locked.bin\"); return copied+\",\"+last_file_error()+\",\"+read_file(\"locked.bin\");")=="false,cannot write \"locked.bin\": access denied,read only");
CHECK(text_of(fixture,"bool copied=copy_file(\"locked.bin\", \"locked2.bin\"); bool again=copy_file(\"web.bin\", \"locked2.bin\"); return copied+\",\"+again;")=="true,true");
CHECK(temp_files(root).empty());
CHECK(zodiac::platform::discard_file(root/"locked.bin").value()==0);
CHECK_FALSE(fs::exists(root/"locked.bin"));
}
#endif
