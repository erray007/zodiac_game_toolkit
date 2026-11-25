// The script pack object (spec 8.3 of the library-and-packs design; contract in core/vfs/files.hpp, register_pack): a pack file
// opened through the Vfs, over the shared pack::PackStorage of that file.
// - The methods are free functions bound with asCALL_CDECL_OBJFIRST and wrapped in guarded<> (an allocation failure becomes
//   "out of memory"); the factory is a member of PackLibrary, which the engine owns through a user data slot, so a pack reaches
//   the Vfs without a global.
// - Each object holds one shared_ptr to the storage: close drops only that one (after flushing), so other pack objects, mounts
//   and readers of the file keep it open, and the last holder closes it.
// - mount_pack mounts a pack source (pack_source.hpp) with the object as the mount's owner, so unmount_pack, close, a new open
//   and the object's release remove exactly its mounts (Vfs::unmount).
#include <core/vfs/files.hpp>
#include <core/pack/pack_format.hpp>
#include <core/pack/pack_source.hpp>
#include <core/pack/pack_storage.hpp>
#include <core/vfs/path.hpp>
#include <core/text.hpp>
#include <core/types/array.hpp>
#include <core/types/script_guard.hpp>
#include <core/types/string.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/clock.hpp>
#include <angelscript.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zodiac {
namespace {
using vfs::Failure;
using vfs::FileAction;
using vfs::FileError;

constexpr char kNotOpen[]="pack is not open";
constexpr char kAlreadyMounted[]="already mounted there";
constexpr char kNullPack[]="Null pointer access";
constexpr char kOutOfMemory[]="out of memory";
constexpr asPWORD packs_slot=0x5A50414B;
constexpr std::uint64_t kMaxText=static_cast<std::uint64_t>(String::max_byte_length);

std::span<const std::byte> bytes_of(std::string_view text) noexcept
{
return {reinterpret_cast<const std::byte*>(text.data()),text.size()};
}
std::string quoted(std::string_view text)
{
std::string out(1,'"');
out.append(one_line(text)).push_back('"');
return out;
}
// `"<name>" is not a valid entry name`.
std::string bad_name_text(std::string_view name)
{
return quoted(name).append(" is not a valid entry name");
}
// `cannot read|delete "<name>" from the pack: <reason>`.
std::string entry_text(FileAction action,std::string_view name,FileError code)
{
std::string text(action==FileAction::remove?"cannot delete ":"cannot read ");
text.append(quoted(name)).append(" from the pack: ").append(vfs::reason_text(code));
return text;
}
[[gnu::noinline,gnu::cold]] void raise_text(const std::string& text) noexcept
{
raise_script_exception(text.c_str());
}

class ScriptPack{
public:
ScriptPack(vfs::Vfs& vfs,asITypeInfo* string_array) noexcept:vfs_(&vfs),string_array_(string_array){}
~ScriptPack(){close();}
ScriptPack(const ScriptPack&)=delete;
ScriptPack& operator=(const ScriptPack&)=delete;
void add_ref() noexcept{++references_;}
void release() noexcept{if(--references_==0)delete this;}
bool open(std::string_view path,std::string_view key,bool create);
void close() noexcept;
bool is_open() const noexcept{return storage_!=nullptr;}
const std::string& error() const noexcept{return error_;}
bool exists(std::string_view name) const;
ScriptArray* list() const;
int count() const;
std::int64_t size(std::string_view name) const;
bool add_file(std::string_view from,std::string_view name);
bool add(std::string_view name,std::string_view data);
bool remove(std::string_view name);
String read(std::string_view name) const;
bool extract(std::string_view name,std::string_view to);
bool compact();
bool flush();
std::int64_t wasted_bytes() const;
bool mount(std::string_view at,bool writable);
void unmount() noexcept{vfs_->unmount(this);}
private:
bool fail(std::string text) noexcept
{
error_=std::move(text);
return false;
}
bool write_failed(FileError code)
{
return fail(vfs::write_failure_text(path_,Failure{code,{}}));
}
bool succeeded() noexcept
{
error_.clear();
return true;
}
// The entry name for a change of the pack, or nullopt with error set: not open, an invalid name, or a pack it cannot write.
std::optional<std::string> change_name(std::string_view name);
vfs::Vfs* vfs_;
asITypeInfo* string_array_;
std::shared_ptr<pack::PackStorage> storage_;
std::string path_;
std::string error_;
int references_=1;
// The mount the pack file was found through is writable, and so is the file.
bool writable_=false;
};

bool ScriptPack::open(std::string_view path,std::string_view key,bool create)
{
close();
auto found=vfs_->host_file(path,false);
FileAction action=FileAction::read;
bool missing=false;
if(!found&&found.error().code==FileError::not_found&&create){
missing=true;
action=FileAction::write;
found=vfs_->host_file(path,true);
}
if(!found)return fail(vfs::failure_text(action,path,found.error()));
// A pack inside a pack (a source without host files): Task 10 keeps *.zpk assets plain, so dev and release agree.
if(!*found)return fail(quoted(path).append(" is inside a pack; a pack file must be a plain file"));
auto storage=pack::PackStorage::open((*found)->path,pack::KeySource(std::in_place_type<std::string>,key),create);
if(missing)vfs_->forget(path);
if(!storage){
if(storage.error().in_use)return fail(quoted(path).append(pack::kInUseText));
return fail(vfs::failure_text(storage.error().action,path,storage.error().failure));
}
storage_=std::move(*storage);
path_.assign(path);
writable_=(*found)->writable&&!storage_->read_only();
storage_->set_log(path_,[log=vfs_->log()](std::string_view line){
if(log)log(ZODIAC_LOG_ERROR,line);
});
return succeeded();
}

void ScriptPack::close() noexcept
{
if(!storage_)return;
try{
if(auto flushed=storage_->flush();!flushed)write_failed(flushed.error());
}
catch(...){
error_=kOutOfMemory;
}
// The mounts this object made (Task 8).
vfs_->unmount(this);
storage_.reset();
}

std::optional<std::string> ScriptPack::change_name(std::string_view name)
{
if(!storage_){
fail(kNotOpen);
return std::nullopt;
}
auto entry=pack::canonical_entry_name(name);
if(!entry){
fail(bad_name_text(name));
return std::nullopt;
}
if(!writable_){
write_failed(FileError::access_denied);
return std::nullopt;
}
return entry;
}

bool ScriptPack::exists(std::string_view name) const
{
if(!storage_)return false;
const auto entry=pack::canonical_entry_name(name);
if(!entry)return false;
const auto info=storage_->stat(*entry);
return info&&info->kind==vfs::EntryKind::file;
}

ScriptArray* ScriptPack::list() const
{
const std::vector<std::string> names=storage_?storage_->list():std::vector<std::string>();
if(names.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::bad_alloc();
// The array raises the script exception when it cannot allocate its elements.
ScriptArray* array=ScriptArray::create(string_array_,static_cast<int>(names.size()));
if(!array)return nullptr;
for(std::size_t i=0;i<names.size();++i)*static_cast<String*>(array->at(static_cast<int>(i)))=std::string_view(names[i]);
return array;
}

int ScriptPack::count() const
{
if(!storage_)return 0;
const std::size_t files=storage_->count();
return files>static_cast<std::size_t>(std::numeric_limits<int>::max())?std::numeric_limits<int>::max():static_cast<int>(files);
}

std::int64_t ScriptPack::size(std::string_view name) const
{
if(!storage_)return -1;
const auto entry=pack::canonical_entry_name(name);
if(!entry)return -1;
const auto info=storage_->stat(*entry);
return info&&info->kind==vfs::EntryKind::file?static_cast<std::int64_t>(info->size):-1;
}

bool ScriptPack::add_file(std::string_view from,std::string_view name)
{
const auto entry=change_name(name);
if(!entry)return false;
const auto reader=vfs_->open_read(from);
if(!reader)return fail(vfs::read_failure_text(from,reader.error()));
const auto info=vfs_->stat(from);
const std::int64_t modified=info?info->modified_utc_ms:platform::utc_now_ms();
if(auto added=storage_->add_reader(*entry,**reader,modified);!added){
if(added.error().to)return write_failed(added.error().code);
return fail(vfs::read_failure_text(from,Failure{added.error().code,{}}));
}
return succeeded();
}

bool ScriptPack::add(std::string_view name,std::string_view data)
{
const auto entry=change_name(name);
if(!entry)return false;
if(auto added=storage_->add(*entry,bytes_of(data),platform::utc_now_ms());!added)return write_failed(added.error());
return succeeded();
}

bool ScriptPack::remove(std::string_view name)
{
const auto entry=change_name(name);
if(!entry)return false;
const auto removed=storage_->remove(*entry);
if(!removed)return write_failed(removed.error());
if(!*removed)return fail(entry_text(FileAction::remove,name,FileError::not_found));
return succeeded();
}

String ScriptPack::read(std::string_view name) const
{
if(!storage_){
raise_script_exception(kNotOpen);
return String();
}
const auto entry=pack::canonical_entry_name(name);
if(!entry){
raise_text(bad_name_text(name));
return String();
}
const auto reader=storage_->open_reader(*entry);
if(!reader){
raise_text(entry_text(FileAction::read,name,reader.error()));
return String();
}
vfs::Reader& source=**reader;
const std::uint64_t size=source.size();
if(size>kMaxText)throw std::bad_alloc();
std::int64_t failure=0;
String text=String::filled(static_cast<std::size_t>(size),[&](char* data)->std::size_t{
std::size_t done=0;
while(done<size){
const std::int64_t got=source.read_at(done,{reinterpret_cast<std::byte*>(data+done),static_cast<std::size_t>(size)-done});
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
raise_text(entry_text(FileAction::read,name,vfs::read_error(failure)));
return String();
}
return text;
}

bool ScriptPack::extract(std::string_view name,std::string_view to)
{
if(!storage_)return fail(kNotOpen);
const auto entry=pack::canonical_entry_name(name);
if(!entry)return fail(bad_name_text(name));
const auto reader=storage_->open_reader(*entry);
if(!reader)return fail(entry_text(FileAction::read,name,reader.error()));
if(auto written=vfs_->write_from(to,**reader);!written){
if(written.error().to)return fail(vfs::write_failure_text(to,written.error().failure));
return fail(entry_text(FileAction::read,name,written.error().failure.code));
}
return succeeded();
}

bool ScriptPack::compact()
{
if(!storage_)return fail(kNotOpen);
if(!writable_)return write_failed(FileError::access_denied);
const auto compacted=storage_->compact();
// The folder saw a temporary file come and go, and the pack file replaced.
vfs_->forget(path_);
if(!compacted)return write_failed(compacted.error());
return succeeded();
}

bool ScriptPack::flush()
{
if(!storage_)return fail(kNotOpen);
if(auto flushed=storage_->flush();!flushed)return write_failed(flushed.error());
return succeeded();
}

std::int64_t ScriptPack::wasted_bytes() const
{
if(!storage_)return 0;
const std::uint64_t wasted=storage_->wasted_bytes();
return wasted>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())?std::numeric_limits<std::int64_t>::max():static_cast<std::int64_t>(wasted);
}

bool ScriptPack::mount(std::string_view at,bool writable)
{
if(!storage_)return fail(kNotOpen);
// The rules Vfs::mount applies (a folder path of the VFS, never a host path), with their text.
if(auto point=vfs::parse_path(at,false);!point)return fail(std::move(point.error()));
// A pack this object cannot change takes no writes through a mount either: the same failure as its own changes.
if(writable&&!writable_)return write_failed(FileError::access_denied);
if(!vfs_->mount(at,pack::make_pack_source(storage_,writable),writable,this))return fail(kAlreadyMounted);
return succeeded();
}

bool pack_open(ScriptPack& pack,const String& path,const String& key,bool create)
{
return pack.open(path.bytes(),key.bytes(),create);
}
void pack_close(ScriptPack& pack) noexcept
{
pack.close();
}
bool pack_is_open(const ScriptPack& pack) noexcept
{
return pack.is_open();
}
String pack_error(const ScriptPack& pack)
{
return String(pack.error());
}
bool pack_exists(const ScriptPack& pack,const String& name)
{
return pack.exists(name.bytes());
}
ScriptArray* pack_list(const ScriptPack& pack)
{
return pack.list();
}
int pack_count(const ScriptPack& pack)
{
return pack.count();
}
std::int64_t pack_size(const ScriptPack& pack,const String& name)
{
return pack.size(name.bytes());
}
bool pack_add_file(ScriptPack& pack,const String& from,const String& name)
{
return pack.add_file(from.bytes(),name.bytes());
}
bool pack_add(ScriptPack& pack,const String& name,const String& data)
{
return pack.add(name.bytes(),data.bytes());
}
bool pack_remove(ScriptPack& pack,const String& name)
{
return pack.remove(name.bytes());
}
String pack_read(const ScriptPack& pack,const String& name)
{
return pack.read(name.bytes());
}
bool pack_extract(ScriptPack& pack,const String& name,const String& to)
{
return pack.extract(name.bytes(),to.bytes());
}
bool pack_compact(ScriptPack& pack)
{
return pack.compact();
}
bool pack_flush(ScriptPack& pack)
{
return pack.flush();
}
std::int64_t pack_wasted_bytes(const ScriptPack& pack)
{
return pack.wasted_bytes();
}
// The pack@ handle a script passes comes with a reference for the callee, released here.
struct HandleRelease{
ScriptPack* pack;
~HandleRelease(){if(pack)pack->release();}
};
bool mount_pack(ScriptPack* pack,const String& at,bool writable)
{
const HandleRelease release{pack};
if(!pack){
raise_script_exception(kNullPack);
return false;
}
return pack->mount(at.bytes(),writable);
}
void unmount_pack(ScriptPack* pack) noexcept
{
const HandleRelease release{pack};
if(!pack){
raise_script_exception(kNullPack);
return;
}
pack->unmount();
}
void pack_add_ref(ScriptPack& pack) noexcept
{
pack.add_ref();
}
void pack_release(ScriptPack& pack) noexcept
{
pack.release();
}

class PackLibrary{
public:
PackLibrary(vfs::Vfs& vfs,asITypeInfo* string_array) noexcept:vfs_(vfs),string_array_(string_array){}
ScriptPack* make_pack() noexcept
{
auto* pack=new(std::nothrow) ScriptPack(vfs_,string_array_);
if(!pack)[[unlikely]]raise_script_exception(kOutOfMemory);
return pack;
}
private:
vfs::Vfs& vfs_;
asITypeInfo* string_array_;
};

void cleanup_library(asIScriptEngine* engine)
{
delete static_cast<PackLibrary*>(engine->GetUserData(packs_slot));
}
void check(int result,const char* declaration)
{
if(result<0)throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
void method(asIScriptEngine* engine,const char* declaration,const asSFuncPtr& pointer)
{
check(engine->RegisterObjectMethod("pack",declaration,pointer,asCALL_CDECL_OBJFIRST),declaration);
}
}

void register_pack(asIScriptEngine* engine,vfs::Vfs& vfs)
{
asITypeInfo* string_array=engine->GetTypeInfoByDecl("array<string>");
if(!string_array)throw std::runtime_error("cannot register pack: array<string> is not available");
auto owned=std::make_unique<PackLibrary>(vfs,string_array);
PackLibrary* library=owned.get();
// From here the engine owns the library (and frees it when it is released), even if a registration below fails.
delete static_cast<PackLibrary*>(engine->SetUserData(owned.release(),packs_slot));
engine->SetEngineUserDataCleanupCallback(cleanup_library,packs_slot);

// A reference type holding no handles, so it is not garbage collected.
check(engine->RegisterObjectType("pack",0,asOBJ_REF),"pack");
check(engine->RegisterObjectBehaviour("pack",asBEHAVE_FACTORY,"pack@ f()",asMETHOD(PackLibrary,make_pack),asCALL_THISCALL_ASGLOBAL,library),"pack()");
check(engine->RegisterObjectBehaviour("pack",asBEHAVE_ADDREF,"void f()",asFUNCTION(pack_add_ref),asCALL_CDECL_OBJFIRST),"pack addref");
check(engine->RegisterObjectBehaviour("pack",asBEHAVE_RELEASE,"void f()",asFUNCTION(pack_release),asCALL_CDECL_OBJFIRST),"pack release");
method(engine,"bool open(const string &in path, const string &in key, bool create = false)",asFUNCTION(guarded<&pack_open>::call));
method(engine,"void close()",asFUNCTION(pack_close));
method(engine,"bool get_is_open() const property",asFUNCTION(pack_is_open));
method(engine,"string get_error() const property",asFUNCTION(guarded<&pack_error>::call));
method(engine,"bool exists(const string &in name) const",asFUNCTION(guarded<&pack_exists>::call));
method(engine,"array<string>@ list() const",asFUNCTION(guarded<&pack_list>::call));
method(engine,"int get_count() const property",asFUNCTION(guarded<&pack_count>::call));
method(engine,"int64 size(const string &in name) const",asFUNCTION(guarded<&pack_size>::call));
method(engine,"bool add_file(const string &in vfs_path, const string &in name)",asFUNCTION(guarded<&pack_add_file>::call));
method(engine,"bool add(const string &in name, const string &in data)",asFUNCTION(guarded<&pack_add>::call));
method(engine,"bool remove(const string &in name)",asFUNCTION(guarded<&pack_remove>::call));
method(engine,"string read(const string &in name)",asFUNCTION(guarded<&pack_read>::call));
method(engine,"bool extract(const string &in name, const string &in vfs_path)",asFUNCTION(guarded<&pack_extract>::call));
method(engine,"bool compact()",asFUNCTION(guarded<&pack_compact>::call));
method(engine,"bool flush()",asFUNCTION(guarded<&pack_flush>::call));
method(engine,"int64 get_wasted_bytes() const property",asFUNCTION(guarded<&pack_wasted_bytes>::call));
check(engine->RegisterGlobalFunction("bool mount_pack(pack@ p, const string &in at = \"/\", bool writable = false)",asFUNCTION(guarded<&mount_pack>::call),asCALL_CDECL),"mount_pack");
check(engine->RegisterGlobalFunction("void unmount_pack(pack@ p)",asFUNCTION(unmount_pack),asCALL_CDECL),"unmount_pack");
}
}
