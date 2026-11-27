// zodiac::Host implements the plugin ABI table (zodiac/plugin.h) on top of an AngelScript engine.
//
// Lifetime: the Host must outlive every script call into functions registered through it, so shut the engine down before destroying the Host.
// Threads: registration and the setters run on the main thread before scripts execute. The call-time accessors in the table only read the call and immutable registration data, so different script contexts may use them concurrently.
// VFS entries (plugin.h, VFS rules): they reach the Vfs given to set_vfs (ZODIAC_E_UNAVAILABLE without one) from any thread. A handle
// is a slot of a table that only grows, in segments that never move, tagged with the slot's generation: every call checks the
// generation under the slot's lock, so a stale or forged handle is refused and calls on one handle are serialized. A read takes
// one uncontended compare-exchange and goes through the Reader straight into the plugin's buffer.
#pragma once
#include <zodiac/plugin.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class asIScriptEngine;
class asIScriptFunction;
class asITypeInfo;

namespace zodiac {
namespace vfs {
class Vfs;
}
class PluginFiles;
class Host{
public:
using LogHandler=std::function<void(zodiac_log_level,std::string_view)>;
explicit Host(asIScriptEngine* engine);
// Closes every handle a plugin left open.
~Host();
Host(const Host&)=delete;
Host& operator=(const Host&)=delete;
// The VFS the plugin entries read (null: they give ZODIAC_E_UNAVAILABLE). It must outlive every open handle and every call.
void set_vfs(vfs::Vfs* files) noexcept{vfs_=files;}
vfs::Vfs* vfs() const noexcept{return vfs_;}
zodiac_host* abi() noexcept{return &table_.abi;}
asIScriptEngine* engine() const noexcept{return engine_;}
const std::string& current_module() const noexcept{return current_module_;}
void set_current_module(std::string_view name){current_module_.assign(name);}
// The default handler discards. Called for registration errors and plugin log() calls, possibly from script threads.
void set_log_handler(LogHandler handler){log_handler_=std::move(handler);}
const LogHandler& log_handler() const noexcept{return log_handler_;}
// Number of registrations the host itself rejected (each also logged as an error). A plugin's own log() calls never count.
std::size_t registration_failures() const noexcept{return registration_failures_;}
void log(zodiac_log_level level,std::string_view message) const noexcept;
private:
friend struct HostAbi;
// What a script-visible value is, resolved once at registration so calls never inspect AngelScript type metadata.
enum class Kind:std::uint8_t{other,boolean,int8,int16,int32,int64,uint8,uint16,uint32,uint64,float32,float64,string,array,object};
struct Value{
Kind kind=Kind::other;
bool by_reference=false;   // primitive passed as &in/&out/&inout, or returned by reference
bool handle=false;         // object handle (@)
Kind element=Kind::other;  // array element kind
asITypeInfo* type=nullptr; // object type, used to create returned arrays
};
// One record per generic registration; its address is the AngelScript auxiliary pointer of the function.
struct Binding{
zodiac_generic_fn fn;
void* user_data;
const Host* host;
bool string_result; // returns string by value: the thunk constructs it before the call so ret_string only assigns
Value result;
std::vector<Value> params;
};
// Standard-layout wrapper so a zodiac_host* can be converted back to its Host.
struct Table{
zodiac_host abi;
Host* owner;
};
// Both never throw: a failed message allocation or a throwing log handler only drops the message.
int fail(int code,std::string_view declaration,std::string_view reason) const noexcept;
int fail_engine(int engine_code,std::string_view declaration) const noexcept;
enum class Target:std::uint8_t{function,method,factory};
int register_generic(Target target,const char* type_name,const char* declaration,zodiac_generic_fn fn,void* user_data);
int register_native(const char* declaration,void (*fn)(void));
int register_ref_type(const char* name,void (*add_ref)(void*),void (*release)(void*));
void describe(Binding& binding,const asIScriptFunction* function) const;
Value classify(int type_id,bool by_reference) const;
Table table_;
asIScriptEngine* engine_;
std::string current_module_;
LogHandler log_handler_;
mutable std::size_t registration_failures_=0;
std::deque<Binding> bindings_;
vfs::Vfs* vfs_=nullptr;
// The plugins' open files; made with the Host, so a call never has to create it.
std::unique_ptr<PluginFiles> files_;
};
}
