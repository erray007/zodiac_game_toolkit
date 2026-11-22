// The script file object and the file helpers (contract in files.hpp).
// - The file object's methods are free functions bound with asCALL_CDECL_OBJFIRST and wrapped in guarded<> (an allocation failure
//   becomes "out of memory"); the global helpers are members of FileLibrary, which the engine owns through a user data slot, bound
//   with asCALL_THISCALL_ASGLOBAL, so they reach the Vfs without a global. They are noexcept and catch for themselves.
// - Misuse and read failures raise their script exception out of line (cold), so the read paths stay small.
#include <core/vfs/files.hpp>
#include <core/library/date_time.hpp>
#include <core/text.hpp>
#include <core/types/array.hpp>
#include <core/types/script_guard.hpp>
#include <core/types/string.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/clock.hpp>
#include <angelscript.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zodiac {
namespace {
using vfs::EntryKind;
using vfs::Failure;
using vfs::FileAction;
using vfs::FileError;
using vfs::WriteMode;

constexpr char kNotOpen[]="file is not open";
constexpr char kNotWritable[]="file is not open for writing";
constexpr char kOutOfMemory[]="out of memory";
constexpr asPWORD files_slot=0x5A46494C;
// The read buffer of a file object: a power of two, so refills start at aligned offsets (a pack's 64 KiB chunks, a disk's pages).
constexpr std::size_t kBufferSize=65536;
constexpr std::uint64_t kMaxText=static_cast<std::uint64_t>(String::max_byte_length);

thread_local std::string last_error;

FileError error_of(std::int64_t result) noexcept
{
return vfs::read_error(result);
}
std::span<std::byte> bytes_at(char* data,std::size_t size) noexcept
{
return {reinterpret_cast<std::byte*>(data),size};
}
std::span<const std::byte> bytes_of(std::string_view text) noexcept
{
return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
[[gnu::noinline,gnu::cold]] void raise_failure(std::string_view path,const Failure& failure) noexcept
{
try{
raise_script_exception(vfs::read_failure_text(path,failure).c_str());
}
catch(...){
raise_current_exception();
}
}
[[gnu::noinline,gnu::cold]] void raise_read_failure(std::string_view path,std::int64_t result) noexcept
{
raise_failure(path,Failure{error_of(result),{}});
}
[[gnu::noinline,gnu::cold]] void raise_bad_mode(std::string_view mode) noexcept
{
try{
std::string text(1,'"');
text.append(one_line(mode)).append("\" is not a file mode");
raise_script_exception(text.c_str());
}
catch(...){
raise_current_exception();
}
}
// The helpers' failure: last_file_error gets the text (or nothing when even that cannot be had).
[[gnu::noinline,gnu::cold]] bool failed(FileAction action,std::string_view path,const Failure& failure) noexcept
{
try{
last_error=vfs::failure_text(action,path,failure);
}
catch(...){
last_error.clear();
}
return false;
}
bool failed(FileAction action,std::string_view path,FileError code) noexcept
{
return failed(action,path,Failure{code,{}});
}
// A copy's or move's failure: from is read (or, ending a move by copy and delete, deleted), to is written.
bool failed(std::string_view from,std::string_view to,const vfs::MoveFailure& failure) noexcept
{
if(failure.to)return failed(FileAction::write,to,failure.failure);
return failed(failure.removing?FileAction::remove:FileAction::read,from,failure.failure);
}
bool succeeded() noexcept
{
last_error.clear();
return true;
}

// Text mode: removes the "\r" of every "\r\n" in place and returns the new size. A lone "\r" stays.
std::size_t drop_carriage_returns(char* data,std::size_t size) noexcept
{
const char* const end=data+size;
const char* in=static_cast<const char*>(std::memchr(data,'\r',size));
if(!in)return size;
char* out=data+(in-data);
// in points at a "\r"; each step copies it (or, before a "\n", only what follows it) up to the next "\r".
while(in<end){
const char* from=in+1<end&&in[1]=='\n'?in+1:in;
const char* next=static_cast<const char*>(std::memchr(in+1,'\r',static_cast<std::size_t>(end-in-1)));
if(!next)next=end;
const auto count=static_cast<std::size_t>(next-from);
std::memmove(out,from,count);
out+=count;
in=next;
}
return static_cast<std::size_t>(out-data);
}

class ScriptFile{
public:
explicit ScriptFile(vfs::Vfs& vfs) noexcept:vfs_(&vfs){}
~ScriptFile(){close();}
ScriptFile(const ScriptFile&)=delete;
ScriptFile& operator=(const ScriptFile&)=delete;
void add_ref() noexcept{++references_;}
void release() noexcept{if(--references_==0)delete this;}
bool open(std::string_view path,std::string_view mode);
String read_rest();
String read_count(std::int64_t count);
String read_line();
bool write(std::string_view data);
bool seek(std::int64_t position) noexcept;
bool sync();
void close() noexcept;
bool is_open() const noexcept{return reader_||writer_;}
std::int64_t size() const noexcept{return static_cast<std::int64_t>(size_);}
std::int64_t position() const noexcept{return static_cast<std::int64_t>(position_);}
bool eof() const noexcept{return position_>=size_;}
const std::string& error() const noexcept{return error_;}
private:
std::int64_t fetch(std::uint64_t at,std::span<std::byte> to)
{
return reader_?reader_->read_at(at,to):writer_->read_at(at,to);
}
bool buffered(std::uint64_t at) const noexcept
{
return at>=buffer_start_&&at-buffer_start_<buffer_size_;
}
std::int64_t load(std::uint64_t at);
std::int64_t read_into(std::uint64_t at,char* to,std::size_t size);
bool skip_bom();
String read_text(std::uint64_t count);
void mirror(std::uint64_t at,std::string_view data) noexcept;
vfs::Vfs* vfs_;
std::shared_ptr<vfs::Reader> reader_;
std::unique_ptr<vfs::WriteTarget> writer_;
std::unique_ptr<std::byte[]> buffer_;
// The buffer holds the file's bytes [buffer_start_, buffer_start_ + buffer_size_); buffer_start_ is a multiple of kBufferSize.
std::uint64_t buffer_start_=0;
std::size_t buffer_size_=0;
std::uint64_t position_=0;
std::uint64_t size_=0;
std::string path_;
std::string error_;
int references_=1;
bool text_=false;
bool append_=false;
};

bool ScriptFile::open(std::string_view path,std::string_view mode)
{
// r, w or a, then "+" (r only) and "b" in either order. A bad mode throws before anything changes, the open file included.
const char kind=mode.empty()?'\0':mode[0];
bool plus=false;
bool binary=false;
bool valid=kind=='r'||kind=='w'||kind=='a';
for(std::size_t i=1;valid&&i<mode.size();++i){
if(mode[i]=='+'&&kind=='r'&&!plus)plus=true;
else if(mode[i]=='b'&&!binary)binary=true;
else valid=false;
}
if(!valid){
raise_bad_mode(mode);
return false;
}
close();
path_.assign(path);
if(kind=='r'&&!plus){
auto reader=vfs_->open_read(path);
if(!reader){
error_=vfs::read_failure_text(path,reader.error());
return false;
}
reader_=std::move(*reader);
size_=reader_->size();
}
else{
auto writer=vfs_->open_write(path,kind=='w'?WriteMode::truncate:kind=='a'?WriteMode::append:WriteMode::update);
if(!writer){
// "r+" reads a file that must exist: a missing one is a read failure, anything else a write failure.
const bool missing=plus&&writer.error().code==FileError::not_found;
error_=vfs::failure_text(missing?FileAction::read:FileAction::write,path,writer.error());
return false;
}
writer_=std::move(*writer);
size_=writer_->size();
}
text_=!binary;
append_=kind=='a';
error_.clear();
return true;
}

// Loads the aligned block holding at: 0, or a negated FileError.
std::int64_t ScriptFile::load(std::uint64_t at)
{
if(!buffer_)buffer_=std::make_unique_for_overwrite<std::byte[]>(kBufferSize);
const std::uint64_t start=at&~static_cast<std::uint64_t>(kBufferSize-1);
buffer_size_=0;
const std::int64_t got=fetch(start,{buffer_.get(),kBufferSize});
if(got<0)return got;
buffer_start_=start;
buffer_size_=static_cast<std::size_t>(got);
return 0;
}

// Copies size bytes from offset at into to: the bytes copied (fewer only at the end of the file), or a negated FileError. What the
// buffer holds comes from it; a rest of kBufferSize or more is read straight into to, a smaller one through a refill.
std::int64_t ScriptFile::read_into(std::uint64_t at,char* to,std::size_t size)
{
std::size_t done=0;
if(buffered(at)){
const auto offset=static_cast<std::size_t>(at-buffer_start_);
done=std::min(size,buffer_size_-offset);
std::memcpy(to,buffer_.get()+offset,done);
}
while(done<size){
const std::uint64_t next=at+done;
const std::size_t left=size-done;
if(left>=kBufferSize){
const std::int64_t got=fetch(next,bytes_at(to+done,left));
if(got<0)return got;
done+=static_cast<std::size_t>(got);
break;
}
if(const std::int64_t loaded=load(next);loaded<0)return loaded;
const auto offset=static_cast<std::size_t>(next-buffer_start_);
if(offset>=buffer_size_)break;
const std::size_t count=std::min(left,buffer_size_-offset);
std::memcpy(to+done,buffer_.get()+offset,count);
done+=count;
// A short block is the end of the file.
if(buffer_size_<kBufferSize)break;
}
return static_cast<std::int64_t>(done);
}

// Text mode, at position 0: steps over a UTF-8 BOM. False after raising a read failure.
bool ScriptFile::skip_bom()
{
char head[3];
const std::int64_t got=read_into(0,head,3);
if(got<0){
raise_read_failure(path_,got);
return false;
}
if(got==3&&head[0]=='\xEF'&&head[1]=='\xBB'&&head[2]=='\xBF')position_=3;
return true;
}

// count (at least 1, at most what is left) bytes from position_ on, made into text in text mode, straight into the result.
String ScriptFile::read_text(std::uint64_t count)
{
if(count>kMaxText)throw std::bad_alloc();
const auto wanted=static_cast<std::size_t>(count);
std::int64_t failure=0;
std::uint64_t consumed=0;
String text=String::filled(wanted,[&](char* data)->std::size_t{
const std::int64_t got=read_into(position_,data,wanted);
if(got<=0){
failure=got;
return 0;
}
consumed=static_cast<std::uint64_t>(got);
if(!text_)return static_cast<std::size_t>(got);
// Never empty: only a "\r" before a "\n" goes.
const std::size_t size=drop_carriage_returns(data,static_cast<std::size_t>(got));
// The "\r" of a "\r\n" that the count cut in two: the "\n" comes with it, so a line end is never split.
if(data[size-1]=='\r'&&position_+consumed<size_){
char next;
const std::int64_t peeked=read_into(position_+consumed,&next,1);
if(peeked<0){
failure=peeked;
return 0;
}
if(peeked==1&&next=='\n'){
data[size-1]='\n';
++consumed;
}
}
return size;
});
if(failure<0){
raise_read_failure(path_,failure);
return String();
}
position_+=consumed;
return text;
}

String ScriptFile::read_rest()
{
if(!is_open()){
raise_script_exception(kNotOpen);
return String();
}
if(text_&&position_==0&&size_>=3&&!skip_bom())return String();
if(position_>=size_)return String();
return read_text(size_-position_);
}

String ScriptFile::read_count(std::int64_t count)
{
if(!is_open()){
raise_script_exception(kNotOpen);
return String();
}
// The BOM is stepped over only by a read that takes bytes, so read(0) never moves the position.
if(count<=0)return String();
if(text_&&position_==0&&size_>=3&&!skip_bom())return String();
if(position_>=size_)return String();
return read_text(std::min(static_cast<std::uint64_t>(count),size_-position_));
}

String ScriptFile::read_line()
{
if(!is_open()){
raise_script_exception(kNotOpen);
return String();
}
if(text_&&position_==0&&size_>=3&&!skip_bom())return String();
String line;
// A "\r" that ended a piece, held back until the next byte tells whether it begins the line end.
bool held=false;
while(position_<size_){
if(!buffered(position_)){
if(const std::int64_t loaded=load(position_);loaded<0){
raise_read_failure(path_,loaded);
return String();
}
// The file is shorter than when it was opened.
if(!buffered(position_))break;
}
const auto offset=static_cast<std::size_t>(position_-buffer_start_);
const char* piece=reinterpret_cast<const char*>(buffer_.get())+offset;
const auto available=static_cast<std::size_t>(std::min<std::uint64_t>(buffer_size_-offset,size_-position_));
const char* end=static_cast<const char*>(std::memchr(piece,'\n',available));
if(held){
held=false;
if(end!=piece)line.append(std::string_view("\r",1));
}
if(end){
auto length=static_cast<std::size_t>(end-piece);
position_+=length+1;
if(length&&piece[length-1]=='\r')--length;
line.append(std::string_view(piece,length));
return line;
}
std::size_t length=available;
position_+=length;
if(piece[length-1]=='\r'){
--length;
held=true;
}
line.append(std::string_view(piece,length));
}
if(held)line.append(std::string_view("\r",1));
return line;
}

// After writing data at at: the bytes the buffer holds are replaced, and a buffer that held the end of the file takes what follows
// on directly (up to its size), so a read after a write needs no refill.
void ScriptFile::mirror(std::uint64_t at,std::string_view data) noexcept
{
if(!buffer_)return;
const std::uint64_t held_end=buffer_start_+buffer_size_;
const std::uint64_t limit=at<=held_end?buffer_start_+kBufferSize:held_end;
const std::uint64_t begin=std::max(at,buffer_start_);
const std::uint64_t end=std::min<std::uint64_t>(at+data.size(),limit);
if(begin>=end)return;
std::memcpy(buffer_.get()+(begin-buffer_start_),data.data()+(begin-at),static_cast<std::size_t>(end-begin));
buffer_size_=std::max(buffer_size_,static_cast<std::size_t>(end-buffer_start_));
}

bool ScriptFile::write(std::string_view data)
{
if(!is_open()){
raise_script_exception(kNotOpen);
return false;
}
if(!writer_){
raise_script_exception(kNotWritable);
return false;
}
if(append_)position_=size_;
if(!data.empty()){
if(auto written=writer_->write_at(position_,bytes_of(data));!written){
error_=vfs::write_failure_text(path_,Failure{written.error(),{}});
return false;
}
mirror(position_,data);
position_+=data.size();
size_=std::max(size_,position_);
}
error_.clear();
return true;
}

// Flushes what was written to the device. A file opened "r" has written nothing: nothing to flush.
bool ScriptFile::sync()
{
if(!is_open()){
raise_script_exception(kNotOpen);
return false;
}
if(!writer_)return true;
if(auto synced=writer_->sync();!synced){
error_=vfs::write_failure_text(path_,Failure{synced.error(),{}});
return false;
}
error_.clear();
return true;
}

bool ScriptFile::seek(std::int64_t position) noexcept
{
if(!is_open()){
raise_script_exception(kNotOpen);
return false;
}
if(position<0||static_cast<std::uint64_t>(position)>size_)return false;
position_=static_cast<std::uint64_t>(position);
error_.clear();
return true;
}

void ScriptFile::close() noexcept
{
if(writer_){
try{
if(auto committed=writer_->commit();!committed)error_=vfs::write_failure_text(path_,Failure{committed.error(),{}});
}
catch(...){
}
writer_.reset();
}
reader_.reset();
size_=0;
position_=0;
// The buffer itself stays for the next open.
buffer_start_=0;
buffer_size_=0;
}

bool file_open(ScriptFile& file,const String& path,const String& mode)
{
return file.open(path.bytes(),mode.bytes());
}
String file_read(ScriptFile& file)
{
return file.read_rest();
}
String file_read_count(ScriptFile& file,std::int64_t count)
{
return file.read_count(count);
}
String file_read_line(ScriptFile& file)
{
return file.read_line();
}
bool file_write(ScriptFile& file,const String& data)
{
return file.write(data.bytes());
}
std::int64_t file_get_size(const ScriptFile& file) noexcept
{
return file.size();
}
std::int64_t file_position(const ScriptFile& file) noexcept
{
return file.position();
}
bool file_eof(const ScriptFile& file) noexcept
{
return file.eof();
}
bool file_sync(ScriptFile& file)
{
return file.sync();
}
bool file_seek(ScriptFile& file,std::int64_t position) noexcept
{
return file.seek(position);
}
void file_close(ScriptFile& file) noexcept
{
file.close();
}
bool file_is_open(const ScriptFile& file) noexcept
{
return file.is_open();
}
String file_error(const ScriptFile& file)
{
return String(file.error());
}
void file_add_ref(ScriptFile& file) noexcept
{
file.add_ref();
}
void file_release(ScriptFile& file) noexcept
{
file.release();
}
String last_file_error()
{
return String(last_error);
}

class FileLibrary{
public:
FileLibrary(vfs::Vfs& vfs,asITypeInfo* string_array) noexcept:vfs_(vfs),string_array_(string_array){}
ScriptFile* make_file() noexcept
{
auto* file=new(std::nothrow) ScriptFile(vfs_);
if(!file)[[unlikely]]raise_script_exception(kOutOfMemory);
return file;
}
bool file_exists(const String& path) noexcept
{
return is_kind(path,EntryKind::file);
}
bool directory_exists(const String& path) noexcept
{
return is_kind(path,EntryKind::directory);
}
String read_file(const String& path) noexcept
{
try{
return read_whole(path.bytes());
}
catch(...){
raise_current_exception();
}
return String();
}
bool write_file(const String& path,const String& data,bool sync) noexcept
{
// Through a temporary file and a rename (Vfs::write_file): the file is the old one or the new one, never a part.
const auto written=vfs_.write_file(path.bytes(),bytes_of(data.bytes()),sync);
return written?succeeded():failed(FileAction::write,path.bytes(),written.error());
}
bool append_file(const String& path,const String& data) noexcept
{
return store(path.bytes(),data.bytes(),WriteMode::append);
}
bool delete_file(const String& path) noexcept
{
const auto removed=vfs_.remove(path.bytes());
return removed?succeeded():failed(FileAction::remove,path.bytes(),removed.error());
}
bool copy_file(const String& from,const String& to,bool sync) noexcept
{
const auto copied=vfs_.copy(from.bytes(),to.bytes(),sync);
return copied?succeeded():failed(from.bytes(),to.bytes(),copied.error());
}
bool move_file(const String& from,const String& to,bool sync) noexcept
{
const auto moved=vfs_.move(from.bytes(),to.bytes(),sync);
return moved?succeeded():failed(from.bytes(),to.bytes(),moved.error());
}
std::int64_t file_size(const String& path) noexcept
{
const auto info=vfs_.stat(path.bytes());
return info&&info->kind==EntryKind::file?static_cast<std::int64_t>(info->size):-1;
}
DateTime file_modified(const String& path) noexcept
{
const auto info=vfs_.stat(path.bytes());
if(!info||(info->kind!=EntryKind::file&&info->kind!=EntryKind::directory)){
raise_failure(path.bytes(),info?Failure{FileError::not_found,{}}:info.error());
return DateTime{0};
}
// The clock date_time::now() reads: the local wall-clock time at that instant, as a zone-less civil time.
return DateTime{platform::utc_to_local_civil_ms(info->modified_utc_ms)};
}
bool create_directory(const String& path,bool recursive) noexcept
{
const auto made=vfs_.create_directory(path.bytes(),recursive);
return made?succeeded():failed(FileAction::create,path.bytes(),made.error());
}
bool delete_directory(const String& path,bool recursive) noexcept
{
const auto removed=vfs_.delete_directory(path.bytes(),recursive);
return removed?succeeded():failed(FileAction::remove,path.bytes(),removed.error());
}
ScriptArray* list_files(const String& path,const String& pattern) noexcept
{
return list(path.bytes(),vfs::ListKind::files,pattern.bytes());
}
ScriptArray* list_directories(const String& path) noexcept
{
return list(path.bytes(),vfs::ListKind::directories,"*");
}
private:
bool is_kind(const String& path,EntryKind kind) noexcept
{
const auto info=vfs_.stat(path.bytes());
return info&&info->kind==kind;
}
String read_whole(std::string_view path)
{
const auto reader=vfs_.open_read(path);
if(!reader){
raise_failure(path,reader.error());
return String();
}
vfs::Reader& source=**reader;
const std::uint64_t size=source.size();
if(size>kMaxText)throw std::bad_alloc();
std::int64_t failure=0;
String text=String::filled(static_cast<std::size_t>(size),[&](char* data)->std::size_t{
std::size_t done=0;
while(done<size){
const std::int64_t got=source.read_at(done,bytes_at(data+done,static_cast<std::size_t>(size)-done));
if(got<0){
failure=got;
return 0;
}
if(got==0)break;
done+=static_cast<std::size_t>(got);
}
return done;
});
if(failure<0){
raise_read_failure(path,failure);
return String();
}
return text;
}
bool store(std::string_view path,std::string_view data,WriteMode mode) noexcept
{
auto target=vfs_.open_write(path,mode);
if(!target)return failed(FileAction::write,path,target.error());
vfs::WriteTarget& out=**target;
const std::uint64_t at=mode==WriteMode::append?out.size():0;
if(!data.empty())if(auto written=out.write_at(at,bytes_of(data));!written)return failed(FileAction::write,path,written.error());
if(const auto committed=out.commit();!committed)return failed(FileAction::write,path,committed.error());
return succeeded();
}
ScriptArray* list(std::string_view path,vfs::ListKind kind,std::string_view pattern) noexcept
{
try{
const auto names=vfs_.list(path,kind,pattern);
if(!names)failed(FileAction::read,path,names.error());
else last_error.clear();
const std::size_t count=names?names->size():0;
if(count>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::bad_alloc();
// The array raises the script exception when it cannot allocate its elements.
ScriptArray* array=ScriptArray::create(string_array_,static_cast<int>(count));
if(!array)return nullptr;
for(std::size_t i=0;i<count;++i)*static_cast<String*>(array->at(static_cast<int>(i)))=std::string_view((*names)[i]);
return array;
}
catch(...){
raise_current_exception();
}
return nullptr;
}
vfs::Vfs& vfs_;
asITypeInfo* string_array_;
};

void cleanup_library(asIScriptEngine* engine)
{
delete static_cast<FileLibrary*>(engine->GetUserData(files_slot));
}
void check(int result,const char* declaration)
{
if(result<0)throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
void method(asIScriptEngine* engine,const char* declaration,const asSFuncPtr& pointer)
{
check(engine->RegisterObjectMethod("file",declaration,pointer,asCALL_CDECL_OBJFIRST),declaration);
}
void helper(asIScriptEngine* engine,FileLibrary* library,const char* declaration,const asSFuncPtr& pointer)
{
check(engine->RegisterGlobalFunction(declaration,pointer,asCALL_THISCALL_ASGLOBAL,library),declaration);
}
}

void register_files(asIScriptEngine* engine,vfs::Vfs& vfs)
{
asITypeInfo* string_array=engine->GetTypeInfoByDecl("array<string>");
if(!string_array)throw std::runtime_error("cannot register files: array<string> is not available");
auto owned=std::make_unique<FileLibrary>(vfs,string_array);
FileLibrary* library=owned.get();
// From here the engine owns the library (and frees it when it is released), even if a registration below fails.
delete static_cast<FileLibrary*>(engine->SetUserData(owned.release(),files_slot));
engine->SetEngineUserDataCleanupCallback(cleanup_library,files_slot);

// A reference type holding no handles, so it is not garbage collected.
check(engine->RegisterObjectType("file",0,asOBJ_REF),"file");
check(engine->RegisterObjectBehaviour("file",asBEHAVE_FACTORY,"file@ f()",asMETHOD(FileLibrary,make_file),asCALL_THISCALL_ASGLOBAL,library),"file()");
check(engine->RegisterObjectBehaviour("file",asBEHAVE_ADDREF,"void f()",asFUNCTION(file_add_ref),asCALL_CDECL_OBJFIRST),"file addref");
check(engine->RegisterObjectBehaviour("file",asBEHAVE_RELEASE,"void f()",asFUNCTION(file_release),asCALL_CDECL_OBJFIRST),"file release");
method(engine,"bool open(const string &in path, const string &in mode)",asFUNCTION(guarded<&file_open>::call));
method(engine,"string read()",asFUNCTION(guarded<&file_read>::call));
method(engine,"string read(int64 count)",asFUNCTION(guarded<&file_read_count>::call));
method(engine,"string read_line()",asFUNCTION(guarded<&file_read_line>::call));
method(engine,"bool write(const string &in data)",asFUNCTION(guarded<&file_write>::call));
method(engine,"int64 get_size() const property",asFUNCTION(file_get_size));
method(engine,"int64 get_position() const property",asFUNCTION(file_position));
method(engine,"bool get_eof() const property",asFUNCTION(file_eof));
method(engine,"bool seek(int64 position)",asFUNCTION(file_seek));
method(engine,"bool sync()",asFUNCTION(guarded<&file_sync>::call));
method(engine,"void close()",asFUNCTION(file_close));
method(engine,"bool get_is_open() const property",asFUNCTION(file_is_open));
method(engine,"string get_error() const property",asFUNCTION(guarded<&file_error>::call));

helper(engine,library,"bool file_exists(const string &in path)",asMETHOD(FileLibrary,file_exists));
helper(engine,library,"bool directory_exists(const string &in path)",asMETHOD(FileLibrary,directory_exists));
helper(engine,library,"string read_file(const string &in path)",asMETHOD(FileLibrary,read_file));
helper(engine,library,"bool write_file(const string &in path, const string &in data, bool sync = false)",asMETHOD(FileLibrary,write_file));
helper(engine,library,"bool append_file(const string &in path, const string &in data)",asMETHOD(FileLibrary,append_file));
helper(engine,library,"bool delete_file(const string &in path)",asMETHOD(FileLibrary,delete_file));
helper(engine,library,"bool copy_file(const string &in from, const string &in to, bool sync = false)",asMETHOD(FileLibrary,copy_file));
helper(engine,library,"bool move_file(const string &in from, const string &in to, bool sync = false)",asMETHOD(FileLibrary,move_file));
helper(engine,library,"int64 file_size(const string &in path)",asMETHOD(FileLibrary,file_size));
helper(engine,library,"date_time file_modified(const string &in path)",asMETHOD(FileLibrary,file_modified));
helper(engine,library,"bool create_directory(const string &in path, bool recursive = true)",asMETHOD(FileLibrary,create_directory));
helper(engine,library,"bool delete_directory(const string &in path, bool recursive = false)",asMETHOD(FileLibrary,delete_directory));
helper(engine,library,"array<string>@ list_files(const string &in path, const string &in pattern = \"*\")",asMETHOD(FileLibrary,list_files));
helper(engine,library,"array<string>@ list_directories(const string &in path)",asMETHOD(FileLibrary,list_directories));
check(engine->RegisterGlobalFunction("string last_file_error()",asFUNCTION(guarded<&last_file_error>::call),asCALL_CDECL),"string last_file_error()");
}
}
