// The script functions about the process itself (contract in library.hpp, register_system): is_admin and restart_as_admin.
#include <core/library/library.hpp>
#include <platform/elevation.hpp>
#include <angelscript.h>
#include <stdexcept>
#include <string>

namespace zodiac {
namespace {
bool is_admin() noexcept
{
return platform::is_elevated();
}

void check(int result,const char* declaration)
{
if(result<0)throw std::runtime_error(std::string("cannot register \"")+declaration+"\" (angelscript error "+std::to_string(result)+")");
}
}

bool SystemLibrary::restart_as_admin() noexcept
{
if(platform::is_elevated())return true;
asIScriptContext* const active=asGetActiveContext();
// Only main()'s own context may end the game: not a global variable's initializer (run on a context of the engine's pool, by zodiac
// build too, so building a game never elevates the build), and not a script running on another thread.
if(!active||!main_context||active!=*main_context)return false;
switch(platform::restart_elevated()){
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
