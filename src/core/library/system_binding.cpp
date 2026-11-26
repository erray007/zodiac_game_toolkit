// The script functions about the process itself (contract in library.hpp, register_system): is_admin and restart_as_admin.
// With ZODIAC_TEST_HOOKS, test_hook_elevation stands in for the platform's answers (library.hpp).
#include <core/library/library.hpp>
#include <platform/elevation.hpp>
#include <angelscript.h>
#include <stdexcept>
#include <string>
#if defined(ZODIAC_TEST_HOOKS)
#include <atomic>
#endif

namespace zodiac {
namespace {
#if defined(ZODIAC_TEST_HOOKS)
std::atomic<ElevationHook> g_hook{ElevationHook::off};
std::atomic<unsigned> g_restart_requests{0};
#endif

bool elevated() noexcept
{
#if defined(ZODIAC_TEST_HOOKS)
if(g_hook.load(std::memory_order_relaxed)!=ElevationHook::off)return false;
#endif
return platform::is_elevated();
}

platform::Elevation restart_elevated() noexcept
{
#if defined(ZODIAC_TEST_HOOKS)
switch(g_hook.load(std::memory_order_relaxed)){
case ElevationHook::off:
break;
case ElevationHook::started:
g_restart_requests.fetch_add(1,std::memory_order_relaxed);
return platform::Elevation::started;
case ElevationHook::declined:
g_restart_requests.fetch_add(1,std::memory_order_relaxed);
return platform::Elevation::declined;
case ElevationHook::not_elevated:
case ElevationHook::failed:
g_restart_requests.fetch_add(1,std::memory_order_relaxed);
return platform::Elevation::failed;
}
#endif
return platform::restart_elevated();
}

bool is_admin() noexcept
{
return elevated();
}

void check(int result,const char* declaration)
{
if(result<0)throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
}

#if defined(ZODIAC_TEST_HOOKS)
void test_hook_elevation(ElevationHook hook) noexcept
{
g_hook.store(hook,std::memory_order_relaxed);
g_restart_requests.store(0,std::memory_order_relaxed);
}
unsigned test_hook_restart_requests() noexcept
{
return g_restart_requests.load(std::memory_order_relaxed);
}
#endif

bool SystemLibrary::restart_as_admin() noexcept
{
if(elevated())return true;
asIScriptContext* const active=asGetActiveContext();
// Only main()'s own context may end the game: not a global variable's initializer (run on a context of the engine's pool, by zodiac
// build too, so building a game never elevates the build), and not a script running on another thread.
if(!active||!main_context||active!=*main_context)return false;
// Nor a call nested in main() (a sort's comparator, any callback a native function runs on main's context with PushState): an abort
// there ends only the nested call, and whatever runs it decides what follows, so the game might go on beside its elevated copy.
if(active->IsNested())return false;
switch(restart_elevated()){
case platform::Elevation::already:
return true;
case platform::Elevation::started:
// The context stops when this call returns to it; run() turns the abort into a normal end.
restarting=true;
active->Abort();
return true;
case platform::Elevation::declined:
case platform::Elevation::failed:
return false;
}
return false;
}

void register_system(asIScriptEngine* engine,SystemLibrary& library)
{
check(engine->RegisterGlobalFunction("bool is_admin()",asFUNCTION(is_admin),asCALL_CDECL),"bool is_admin()");
check(engine->RegisterGlobalFunction("bool restart_as_admin()",asMETHOD(SystemLibrary,restart_as_admin),asCALL_THISCALL_ASGLOBAL,&library),"bool restart_as_admin()");
}
}
