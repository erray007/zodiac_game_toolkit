// Administrator rights of the running process, behind the script functions is_admin and restart_as_admin (src/core/library/
// system_binding.cpp), implemented in src/platform/<os>/elevation.cpp.
// - is_elevated(): Windows: the process token is elevated (TokenElevation), or its enabled groups hold the Administrators group (an
//   administrator with UAC off, whose token is the full one). POSIX: the effective user is root (geteuid() == 0). Read once and kept: a
//   process's elevation never changes.
// - restart_elevated():
//   - Windows: already when is_elevated(). Otherwise starts this executable again with administrator rights: ShellExecuteExW with the
//     "runas" verb on the running executable (GetModuleFileNameW), with this process's arguments (restart_parameters) and the working
//     directory the process started in (taken before main, so zodiac run's and the release's change to the game folder does not count;
//     relative paths on the command line keep their meaning). So zodiac run game.zs (zodiac.exe running a script in dev mode) runs the
//     script again, elevated, and a release starts the release again. started: the elevated copy is running; declined: the user said no
//     at the UAC prompt (ERROR_CANCELLED); failed: anything else (no error dialog is shown, SEE_MASK_FLAG_NO_UI). The UAC prompt names
//     its owner as the process's own foreground window (only when user32 is already loaded, so no game imports user32 for this) or its
//     console window. An elevated process cannot share its parent's console: started from a console, the copy opens a console of its own.
//     Before the launch the process takes the restart guard (below) and keeps it on started, until release_held_restart_guard or
//     its exit; on declined and failed it gives the guard back at once.
//   - POSIX: already when euid is 0 (the owner's ruling: true for an elevated process on every platform), failed otherwise: there is
//     no portable way to elevate (sudo, pkexec and doas all differ and all need a terminal or a desktop agent), so a game asks its
//     player to start it as root.
// - The restart guard (Windows; nothing on POSIX): the elevated copy runs while the process that started it is still shutting down,
//   whose open data:/ files commit only then (and whose writable pack is still locked). So the old process creates and owns the named
//   mutex restart_guard_name(<its executable path>) before the launch; the system releases it as abandoned when the process exits,
//   after everything was written. wait_for_restart(), called at start-up by zodiac run and by every release before anything mounts or
//   opens data:/, opens that mutex for its own executable: none there (every ordinary start) costs one failed OpenMutexW; otherwise
//   it waits until the owner is gone (released or abandoned both count as done) for at most kRestartWaitMs, and after that timeout
//   carries on anyway (a hung old process must not keep the game from starting; its unsaved data is then its own). The mutex lives
//   in the session's Local\ namespace, so the elevated copy in the same session finds it, with a security descriptor granting
//   SYNCHRONIZE to everyone (an administrator who elevates with other credentials is another user).
//   release_held_restart_guard() gives back a guard a started restart keeps, at once (nothing when there is none, and on POSIX):
//   zodiac run and the release call it as soon as the game has shut down (its runtime gone, so its files are committed and its
//   packs flushed and unlocked), so the copy does not wait while the old process shows its result window or exits.
//   The guard is named after the executable: in dev mode every `zodiac run` uses the one of zodiac.exe's own path, so an unrelated
//   game started with `zodiac run` while another game run by the same zodiac.exe restarts elevated waits for that restart too
//   (at most kRestartWaitMs).
// - restart_parameters (Windows): every argument the process received after the program name, from the C runtime's __wargv (the
//   arguments main or wmain actually got; the UCRT and CommandLineToArgvW read "" inside quotes differently), each quoted again by
//   append_argument, so each arrives in the copy exactly as given. A process whose runtime has no wide argv (a narrow main) falls
//   back to CommandLineToArgvW(GetCommandLineW()).
// - Notes for the user documentation (Task 14):
//   - UAC's prompt appears on the secure desktop, where only an installed screen reader runs. NVDA reads it only when NVDA is
//     installed, not from its portable copy; Narrator always does. A blind player using a portable NVDA hears nothing at the prompt
//     (Alt+Y accepts it, Escape declines).
//   - A game started from a mapped network drive or a subst drive cannot restart elevated: the elevated token does not see the
//     user's drive mappings, so the launch fails and restart_as_admin returns false.
// Threads: every function may be called from any thread.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace zodiac::platform {
bool is_elevated() noexcept;
enum class Elevation{already,started,declined,failed};
Elevation restart_elevated() noexcept;
// Waits for the restart guard of this executable, as described above. Returns at once when there is none (and on POSIX).
void wait_for_restart() noexcept;
// Releases the restart guard a started restart_elevated keeps, as described above.
void release_held_restart_guard() noexcept;
inline constexpr unsigned kRestartWaitMs=10000;
// "Local\zodiac-restart-" and 16 lowercase hex digits: FNV-1a (64 bits) over the path's UTF-16 (or UTF-32) units with ASCII letters
// folded to upper case, so one executable spelt in two cases has one guard. Deterministic, portable (the tests check it everywhere).
template<class Char>
std::basic_string<Char> restart_guard_name(std::basic_string_view<Char> executable)
{
std::uint64_t hash=0xCBF29CE484222325u;
for(const Char c:executable){
std::uint32_t unit=static_cast<std::uint32_t>(static_cast<std::make_unsigned_t<Char>>(c));
if(unit>='a'&&unit<='z')unit-='a'-'A';
// Two bytes per unit (four for a code point above U+FFFF in UTF-32), low first, so UTF-16 and UTF-32 agree on the BMP.
for(int shift=0;shift<32&&(shift<16||unit>0xFFFFu);shift+=8){
hash^=(unit>>shift)&0xFFu;
hash*=0x100000001B3u;
}
}
constexpr std::string_view prefix="Local\\zodiac-restart-";
constexpr char digits[]="0123456789abcdef";
std::basic_string<Char> name;
name.reserve(prefix.size()+16);
for(const char c:prefix)name.push_back(static_cast<Char>(c));
for(int shift=60;shift>=0;shift-=4)name.push_back(static_cast<Char>(digits[(hash>>shift)&0xFu]));
return name;
}
#if defined(_WIN32)
// The restart guard's parts, for restart_elevated, wait_for_restart and their tests. hold_restart_guard creates and owns the mutex
// name (nullptr when it cannot); release_restart_guard releases and closes it. wait_restart_guard is wait_for_restart for a name.
enum class GuardWait{none,released,abandoned,timed_out};
void* hold_restart_guard(const std::wstring& name) noexcept;
void release_restart_guard(void* guard) noexcept;
GuardWait wait_restart_guard(const std::wstring& name,unsigned timeout_ms) noexcept;
// The arguments for the elevated copy (above); false when they cannot be read.
bool restart_parameters(std::wstring& line);
#endif
}
