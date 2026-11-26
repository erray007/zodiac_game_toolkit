#include <core/script_runtime.hpp>
#include <core/diagnostics.hpp>
#include <core/path_utf8.hpp>
#include <core/project_file.hpp>
#include <core/text.hpp>
#include <core/library/library.hpp>
#include <core/types/array.hpp>
#include <core/types/dictionary.hpp>
#include <core/types/string_binding.hpp>
#include <core/vfs/files.hpp>
#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace zodiac {
namespace {
namespace fs=std::filesystem;
constexpr const char* kModuleName="main";
fs::path from_utf8(std::string_view text)
{
return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
void append_number(std::string& text,int value)
{
char digits[16];
const auto end=std::to_chars(digits,digits+sizeof digits,value).ptr;
text.append(digits,end);
}
// Reads a whole file in one allocation. The error is the system's reason.
std::expected<std::string,std::string> read_file(const fs::path& path)
{
std::error_code error;
const std::uintmax_t size=fs::file_size(path,error);
if(error)return std::unexpected(error_text(error));
errno=0;
std::ifstream in(path,std::ios::binary);
if(!in.is_open()){
const int code=errno;
return std::unexpected(code?error_text(std::error_code(code,std::generic_category())):std::string("the file cannot be opened"));
}
std::string text;
text.resize_and_overwrite(static_cast<std::size_t>(size),[&in](char* buffer,std::size_t count){
in.read(buffer,static_cast<std::streamsize>(count));
return static_cast<std::size_t>(in.gcount());
});
if(in.bad())return std::unexpected(std::string("the file cannot be read"));
return text;
}
std::string_view without_bom(std::string_view text) noexcept
{
if(text.starts_with("\xEF\xBB\xBF"))text.remove_prefix(3);
return text;
}
asIScriptEngine* create_engine()
{
// Must precede engine creation so script threads can be used later; paired with asUnprepareMultithread in the destructor.
asPrepareMultithread();
asIScriptEngine* engine=asCreateScriptEngine();
if(!engine){
asUnprepareMultithread();
throw std::runtime_error("cannot create the AngelScript engine");
}
return engine;
}
// Collects SaveByteCode output in a string.
class StringWriteStream final:public asIBinaryStream{
public:
explicit StringWriteStream(std::string& out) noexcept:out_(out){}
int Write(const void* ptr,asUINT size) override
{
if(!size)return 0;
try{
out_.append(static_cast<const char*>(ptr),size);
return 0;
}
catch(...){
return -1;
}
}
int Read(void*,asUINT) override{return -1;}
private:
std::string& out_;
};
// Feeds LoadByteCode from a view. A read past the end zero-fills the destination and fails, which makes the load fail instead of reading outside the view.
class StringReadStream final:public asIBinaryStream{
public:
explicit StringReadStream(std::string_view bytes) noexcept:rest_(bytes){}
int Read(void* ptr,asUINT size) override
{
if(size>rest_.size()){
std::memset(ptr,0,size);
rest_={};
return -1;
}
if(size){
std::memcpy(ptr,rest_.data(),size);
rest_.remove_prefix(size);
}
return 0;
}
int Write(const void*,asUINT) override{return -1;}
private:
std::string_view rest_;
};
// Appends "namespace::Object::name" of a script function.
void append_function_name(std::string& text,const asIScriptFunction& function)
{
const char* space=function.GetNamespace();
if(space&&*space)text.append(space).append("::");
if(const char* object=function.GetObjectName())text.append(object).append("::");
text.append(function.GetName());
}
}

struct RuntimeCallbacks{
static void message(const asSMessageInfo* info,void* runtime)
{
static_cast<ScriptRuntime*>(runtime)->on_message(*info);
}
static int include(const char* include,const char* from,CScriptBuilder* builder,void* runtime)
{
try{
return static_cast<ScriptRuntime*>(runtime)->on_include(include,from,*builder);
}
catch(...){
return -1;
}
}
};

ScriptRuntime::ScriptRuntime(Diagnostics& diagnostics,ModuleSearchPaths search_paths)
:diagnostics_(diagnostics),search_paths_(std::move(search_paths)),engine_(create_engine()),host_(engine_)
{
engine_->SetMessageCallback(asFUNCTION(RuntimeCallbacks::message),this,asCALL_CDECL);
// Zodiac's array first: string's split and join use array<string>; the dictionary after both, since keys() returns
// array<string>. The sample dictionary add-on is not registered (it keys on std::string; Zodiac's dictionary replaces it).
// The random numbers right after the array: they add random_choice to array<T>, which the engine only allows before the
// first instance (string's array<string>) exists. Then the rest of the library (the sample maths add-on is not registered
// either; register_math replaces it), and the time library, whose date_time.to_string returns a string. The files come after
// time: file_modified returns a date_time, and the lists array<string>.
register_array(engine_);
register_random(engine_);
register_string(engine_);
register_dictionary(engine_);
register_math(engine_);
register_time(engine_);
register_files(engine_,vfs_);
register_pack(engine_,vfs_);
register_system(engine_,system_);
register_exception_helpers(engine_);
host_.set_log_handler([this](zodiac_log_level level,std::string_view message){diagnostics_.report(level,message);});
vfs_.set_log([this](zodiac_log_level level,std::string_view message){diagnostics_.report(level,message);});
}

ScriptRuntime::~ScriptRuntime()
{
// Release every script-held object before shutdown_module runs (plugin.h lifetime rules): the context, then the script module
// with its global variables, then a full garbage collection for objects kept alive only by reference cycles.
if(context_)context_->Release();
context_=nullptr;
main_=nullptr;
if(asIScriptModule* module=engine_->GetModule(kModuleName,asGM_ONLY_IF_EXISTS))module->Discard();
engine_->GarbageCollect(asGC_FULL_CYCLE);
registry_.shutdown_all();
engine_->ShutDownAndRelease();
asUnprepareMultithread();
}

void ScriptRuntime::on_message(const asSMessageInfo& info) noexcept
{
// Compiler information messages ("Compiling void main()") are noise for screen-reader users, so only errors and warnings are reported.
if(info.type==asMSGTYPE_INFORMATION)return;
try{
const bool error=info.type==asMSGTYPE_ERROR;
const zodiac_log_level level=error?ZODIAC_LOG_ERROR:ZODIAC_LOG_WARNING;
const std::string_view kind=error?"error":"warning";
const std::string_view section=info.section?info.section:"";
const std::string_view message=info.message?info.message:"";
std::string line;
line.reserve(section.size()+message.size()+32);
if(!section.empty()){
line.append(section);
if(info.row>0){
line.push_back(':');
append_number(line,info.row);
if(info.col>0){
line.push_back(':');
append_number(line,info.col);
}
}
line.append(": ");
}
line.append(kind).append(": ").append(message);
if(held_messages_)held_messages_->emplace_back(level,std::move(line));
else diagnostics_.report(level,line);
}
catch(...){
}
}

std::string ScriptRuntime::section_name(const fs::path& file) const
{
const fs::path relative=file.lexically_relative(root_);
const bool inside=!relative.empty()&&*relative.begin()!=fs::path("..");
const std::u8string text=(inside?relative:file).generic_u8string();
return std::string(reinterpret_cast<const char*>(text.data()),text.size());
}

int ScriptRuntime::add_section(const fs::path& file,const std::string& section,CScriptBuilder& builder,std::string_view requested_by,std::string_view requested_name)
{
// Already part of the module (CScriptBuilder would skip it too, but only after the file was read).
if(!included_.insert(section).second)return 0;
const auto code=read_file(file);
if(!code){
std::string line;
line.reserve(requested_by.size()+requested_name.size()+code.error().size()+40);
line.append(requested_by).append(": error: cannot read ");
if(requested_name.empty())line.append("script");
else line.append("included file \"").append(requested_name).push_back('"');
line.append(": ").append(code.error());
diagnostics_.report(ZODIAC_LOG_ERROR,line);
return -1;
}
const std::string_view text=without_bom(*code);
// Before the section is added: its #include directives are followed inside AddSectionFromMemory, so the list is depth first.
script_files_.push_back({file,section});
return builder.AddSectionFromMemory(section.c_str(),text.data(),static_cast<unsigned>(text.size()),0)<0?-1:0;
}

int ScriptRuntime::on_include(const char* include,const char* from,CScriptBuilder& builder)
{
const std::string_view requested(include);
const fs::path file=((root_/from_utf8(from)).parent_path()/from_utf8(requested)).lexically_normal();
return add_section(file,section_name(file),builder,from,requested);
}

int ScriptRuntime::load_modules(const Project& project,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check)
{
std::vector<std::string> names;
names.reserve(project.static_modules.size()+project.dynamic_modules.size());
names.insert(names.end(),project.static_modules.begin(),project.static_modules.end());
for(const std::string& name:project.dynamic_modules)if(std::ranges::find(names,name)==names.end())names.push_back(name);
if(names.empty())return check?check(registry_.ordered()):0;
ModuleSearchPaths search=search_paths_;
#if !defined(ZODIAC_RELEASE)
// Dev only: a release loads dynamic modules from the executable's folder (its search path) and nowhere else, so plugins/ is never searched there.
fs::path plugins=project.root/"plugins";
if(std::ranges::find(search.dirs,plugins)==search.dirs.end())search.dirs.push_back(std::move(plugins));
#endif
if(auto loaded=registry_.load(names,search,linked);!loaded){
diagnostics_.report(ZODIAC_LOG_ERROR,loaded.error());
return 2;
}
if(const int checked=check?check(registry_.ordered()):0)return checked;
if(auto registered=registry_.register_all(host_);!registered){
diagnostics_.report(ZODIAC_LOG_ERROR,registered.error());
return 2;
}
return 0;
}

int ScriptRuntime::prepare(const Project& project,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check)
{
main_=nullptr;
debug_info_stripped_=false;
script_files_.clear();
if(context_)context_->Unprepare();
root_=project.root;
if(const int modules=load_modules(project,linked,check))return modules;
main_section_=section_name(project.main_script);
included_.clear();
return 0;
}

int ScriptRuntime::build(const Project& project,const ModuleCheck& check,bool* compile_failed)
{
if(compile_failed)*compile_failed=false;
if(const int prepared=prepare(project,{},check))return prepared;
CScriptBuilder builder;
builder.SetIncludeCallback(&RuntimeCallbacks::include,this);
if(builder.StartNewModule(engine_,kModuleName)<0){
diagnostics_.report(ZODIAC_LOG_ERROR,"error: cannot create the script module");
return 1;
}
if(add_section(project.main_script,main_section_,builder,main_section_,{})<0){
if(compile_failed)*compile_failed=true;
return 1;
}
if(const int built=builder.BuildModule();built<0){
// A global initializer that failed at run time is no compile error; the engine has described it.
if(compile_failed)*compile_failed=built!=asINIT_GLOBAL_VARS_FAILED;
return 1;
}
return find_main();
}

std::expected<std::string,std::string> ScriptRuntime::save_bytecode(bool strip_debug_info) const
{
if(!main_)return std::unexpected(std::string("error: no script has been built"));
const asIScriptModule* module=engine_->GetModule(kModuleName,asGM_ONLY_IF_EXISTS);
std::string bytecode;
StringWriteStream stream(bytecode);
if(!module||module->SaveByteCode(&stream,strip_debug_info)<0)return std::unexpected(std::string("error: cannot save the compiled scripts"));
return bytecode;
}

int ScriptRuntime::load_bytecode(const Project& project,std::string_view bytecode,std::span<const zodiac_module_info* const> linked,const ModuleCheck& check)
{
if(const int prepared=prepare(project,linked,check))return prepared;
asIScriptModule* module=engine_->GetModule(kModuleName,asGM_ALWAYS_CREATE);
if(!module){
diagnostics_.report(ZODIAC_LOG_ERROR,"error: cannot create the script module");
return 1;
}
StringReadStream stream(bytecode);
std::vector<std::pair<zodiac_log_level,std::string>> held;
bool stripped=false;
int loaded=0;
{
// Points the message callback at held for exactly the LoadByteCode call, and back at diagnostics on every exit (an
// exception from the engine or a module included), so no later message can land in the destroyed vector.
struct HeldMessagesScope{
std::vector<std::pair<zodiac_log_level,std::string>>*& target;
~HeldMessagesScope(){target=nullptr;}
};
const HeldMessagesScope scope{held_messages_};
held_messages_=&held;
loaded=module->LoadByteCode(&stream,&stripped);
}
if(loaded==asINIT_GLOBAL_VARS_FAILED){
// The interface matched; a global variable initializer failed, which the engine has described like build() would.
for(const auto& [level,line]:held)diagnostics_.report(level,line);
return 1;
}
if(loaded<0){
// The reader's own messages ("LoadByteCode failed...", missing registered functions) mean nothing to a player; the cause is always a mismatched or incomplete build.
diagnostics_.report(ZODIAC_LOG_ERROR,"error: the game data does not match the modules it was built with");
// A module mismatch (exit 2), unlike a global initializer failure (a script error, exit 1).
return 2;
}
for(const auto& [level,line]:held)diagnostics_.report(level,line);
debug_info_stripped_=stripped;
return find_main();
}

int ScriptRuntime::find_main()
{
asIScriptModule* module=engine_->GetModule(kModuleName);
asIScriptFunction* entry=module->GetFunctionByDecl("int main()");
if(!entry)entry=module->GetFunctionByDecl("void main()");
if(entry){
main_=entry;
return 0;
}
std::string line;
for(asUINT i=0;i<module->GetFunctionCount();++i){
const asIScriptFunction* function=module->GetFunctionByIndex(i);
if(std::string_view(function->GetName())!="main"||*function->GetNamespace())continue;
const char* section=nullptr;
int row=0;
int col=0;
function->GetDeclaredAt(&section,&row,&col);
line.append(section?section:main_section_);
if(row>0){
line.push_back(':');
append_number(line,row);
line.push_back(':');
append_number(line,col);
}
line.append(": error: main() must be declared as \"void main()\" or \"int main()\"");
break;
}
if(line.empty())line.append(main_section_).append(": error: no main() function");
diagnostics_.report(ZODIAC_LOG_ERROR,line);
return 1;
}

void ScriptRuntime::report_exception()
{
const char* section=nullptr;
int col=0;
const int row=context_->GetExceptionLineNumber(&col,&section);
const char* exception=context_->GetExceptionString();
std::string line;
line.reserve(128);
line.append(section?section:main_section_);
if(row>0){
line.push_back(':');
append_number(line,row);
}
line.append(": runtime error: ").append(exception&&*exception?exception:"unknown exception");
if(row<=0&&debug_info_stripped_)line.append(" (line unknown)");
diagnostics_.report(ZODIAC_LOG_ERROR,line);
const asUINT frames=context_->GetCallstackSize();
for(asUINT i=0;i<frames;++i){
const asIScriptFunction* function=context_->GetFunction(i);
if(!function)continue;
line.assign("  at ");
append_function_name(line,*function);
const char* frame_section=nullptr;
const int frame_row=context_->GetLineNumber(i,nullptr,&frame_section);
if(frame_section){
line.append(" (").append(frame_section);
if(frame_row>0){
line.push_back(':');
append_number(line,frame_row);
}
line.push_back(')');
}
diagnostics_.report(ZODIAC_LOG_ERROR,line);
}
}

int ScriptRuntime::run()
{
if(!main_){
diagnostics_.report(ZODIAC_LOG_ERROR,"error: no script has been built");
return 1;
}
if(!context_)context_=engine_->CreateContext();
if(!context_||context_->Prepare(main_)<0){
diagnostics_.report(ZODIAC_LOG_ERROR,"error: cannot prepare main() for execution");
return 1;
}
const int state=context_->Execute();
int result=1;
if(state==asEXECUTION_FINISHED)result=main_->GetReturnTypeId()==asTYPEID_INT32?static_cast<int>(context_->GetReturnDWord()):0;
// restart_as_admin started the elevated copy and stopped main(): this game ends normally, its copy carries on.
else if(state==asEXECUTION_ABORTED&&system_.restarting)result=0;
else if(state==asEXECUTION_EXCEPTION)report_exception();
else{
std::string line=main_section_;
line.append(": runtime error: main() did not finish (execution state ");
append_number(line,state);
line.push_back(')');
diagnostics_.report(ZODIAC_LOG_ERROR,line);
}
context_->Unprepare();
return result;
}

int enter_root(const fs::path& root,Diagnostics& diagnostics)
{
std::error_code error;
fs::current_path(root,error);
if(!error)return 0;
diagnostics.report(ZODIAC_LOG_ERROR,"error: cannot use \""+path_to_utf8(root)+"\" as the working directory: "+error_text(error));
return 2;
}

int exit_code_of(int result,Diagnostics& diagnostics)
{
if(result>=0&&result<=255)return result;
std::string line="warning: main() returned ";
append_number(line,result);
line.append("; exit codes must be 0-255, using 1");
diagnostics.report(ZODIAC_LOG_WARNING,line);
return 1;
}
}
