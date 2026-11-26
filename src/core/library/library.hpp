// The script library of spec 8.5 to 8.7 (core-types design) beyond the types: maths, random numbers and time.
// Contract:
// - register_math registers abs, min, max and clamp for int, int64, float and double; floor, ceil, round(x),
//   round(x, digits), trunc, sqrt, pow, exp, log, log10, sin, cos, tan, asin, acos, atan, atan2, hypot and fmod on double;
//   and the constants PI, TAU and E. Every function is native asCALL_CDECL. Only integer abs (on the smallest value:
//   "integer overflow") and clamp (low greater than high: "clamp: low is greater than high") raise script exceptions.
//   round is half away from zero; round(x, digits) rounds the shortest round-trip decimal form of x (round.hpp).
// - register_random registers int random(int min, int max) (inclusive, bounds swapped when min > max), double random_float()
//   (in [0, 1)), double random_float(double min, double max) (in [min, max), bounds swapped when min > max; equal bounds give
//   min, a NaN bound gives NaN, an infinite bound with unequal bounds gives NaN; no overflow for any finite bounds, a rounding
//   to max gives the double just below it; Xoshiro256::between), bool random_bool(), all on the calling thread's generator
//   (random.hpp), and random_generator with void seed(uint64) and the same four as methods. random_generator is a counted
//   reference type (controller ruling: AngelScript allows &inout only on such types) with value semantics: random_generator g
//   is seeded from entropy, random_generator g(n) by seed(n), and copies (the copy factory, opAssign) copy the state; a
//   failed allocation raises "out of memory". It adds array<T>::random_choice() and random_choice(random_generator
//   &inout) (both const T&, raising "random_choice on an empty array"), so it must come after register_array and before
//   anything makes an array instance (register_string makes array<string>): the engine refuses new template methods then. Native
//   asCALL_CDECL and asCALL_CDECL_OBJFIRST functions; generator: xoshiro256** (random.hpp).
// - register_time registers int64 ticks() and int64 ticks_us() (milliseconds and microseconds since the process started, on
//   platform::monotonic_us, so they never decrease); timer, a POD value type that starts running, with the property getters
//   int64 elapsed (ms, rounded down), double elapsed_seconds and bool running, and restart() (0 and running), pause() and
//   resume() (both idempotent) and set(int64 ms) (sets elapsed, keeps the running state; saturates at +-2^62 us); and
//   date_time, a POD value type holding a DateTime (date_time.hpp): date_time() is 1970-01-01 00:00:00.000,
//   date_time(year, month, day, hour = 0, minute = 0, second = 0, millisecond = 0) raises "invalid date" for parts
//   DateTime::from_parts refuses, date_time::now() (local) and date_time::utc_now() in namespace date_time, the int
//   methods year() month() day() hour() minute() second() millisecond() weekday() (0 = Monday) day_of_year(),
//   to_string(format = "{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}") (DateTime::format), + - += -= with int64 milliseconds,
//   date_time - date_time as int64 milliseconds (results outside int64 raise "integer overflow"), opEquals and opCmp.
//   to_string uses string, so it comes after register_string.
//   Every date_time is a zone-less civil time: now() is the local wall-clock reading and utc_now() the UTC one, each stored
//   as if it were a time without zone (platform::utc_to_local_civil_ms; the zone is re-read within about a second of a
//   change). So now() - utc_now() is the local zone's offset from UTC in milliseconds, mixing the two in arithmetic or
//   comparisons mixes zones, and arithmetic on now() values is wall-clock arithmetic: adding 24 hours across a daylight
//   saving change keeps the wall-clock time (the real time elapsed was 23 or 25 hours). Measure durations with ticks() or
//   timer, never with now().
//   ticks() and timers keep counting while the system is suspended (platform::monotonic_us).
// - register_system(engine, library) (system_binding.cpp) registers bool is_admin() and bool restart_as_admin() (platform/
//   elevation.hpp has the platform rules and the accessibility note on the UAC prompt):
//   - is_admin(): platform::is_elevated(), read once per process.
//   - restart_as_admin(): true at once, doing nothing, when the process is elevated already (on POSIX: root, by the owner's ruling,
//     so a script's `if(!restart_as_admin())` means the same everywhere). Otherwise it only acts on main()'s own context (the
//     runtime's, library.main_context) at its top level: main(), or a script function main() calls directly, may restart the game.
//     A global variable's initializer (which zodiac build runs too), a script on another context or thread, and a call nested in
//     main()'s context by a native function (a sort's comparator, any callback run with PushState, which an abort would end alone)
//     get false and nothing is started.
//     It starts the game again with administrator rights (platform::restart_elevated: the same executable, arguments and starting
//     folder; in zodiac run, zodiac.exe runs the script again). When the user declines the UAC prompt, or the start fails, it returns
//     false and the game runs on. When the elevated copy started, this process ends normally: the call aborts main()'s context and
//     sets library.restarting, and ScriptRuntime::run then returns 0 with nothing reported, so the runtime shuts down as after any
//     main() (modules shut down, logs flushed, the exit code is 0); the script never sees the call return. On POSIX it is false
//     unless the process is root (no portable elevation).
//   Both are native asCALL_CDECL / asCALL_THISCALL_ASGLOBAL functions that cannot fail.
// - Each throws std::runtime_error naming the declaration when the engine refuses one, as register_exception_helpers does.
#pragma once

class asIScriptContext;
class asIScriptEngine;

namespace zodiac {
// The state restart_as_admin needs, owned by the ScriptRuntime (which outlives its engine's script calls).
struct SystemLibrary{
// The runtime's main() context: where it is kept, since the runtime makes it on its first run.
asIScriptContext* const* main_context=nullptr;
// Set when restart_as_admin started the elevated copy and aborted main().
bool restarting=false;
bool restart_as_admin() noexcept;
};
#if defined(ZODIAC_TEST_HOOKS)
// Test hook (a ZODIAC_TEST_HOOKS tree only; test_no_test_hooks proves the symbol absent elsewhere): any value but off makes is_admin
// and restart_as_admin see a process that is not elevated, and stands in for platform::restart_elevated: started, declined, or
// failed (also for not_elevated, as on POSIX), without starting anything. off restores the platform. Each call resets the count of
// restarts that reached the platform's place, which test_hook_restart_requests reads.
enum class ElevationHook{off,not_elevated,started,declined,failed};
void test_hook_elevation(ElevationHook hook) noexcept;
unsigned test_hook_restart_requests() noexcept;
#endif
void register_math(asIScriptEngine* engine);
void register_random(asIScriptEngine* engine);
void register_time(asIScriptEngine* engine);
void register_system(asIScriptEngine* engine,SystemLibrary& library);
}
