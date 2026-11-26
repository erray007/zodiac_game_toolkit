// Administrator rights of the running process, behind the script functions is_admin and restart_as_admin (src/core/library/
// system_binding.cpp), implemented in src/platform/<os>/elevation.cpp.
// - is_elevated(): Windows: the process token is elevated (TokenElevation), or its enabled groups hold the Administrators group (an
//   administrator with UAC off, whose token is the full one). POSIX: the effective user is root (geteuid() == 0). Read once and kept: a
//   process's elevation never changes.
// - restart_elevated():
//   - Windows: already when is_elevated(). Otherwise starts this executable again with administrator rights: ShellExecuteExW with the
//     "runas" verb on the running executable (GetModuleFileNameW), with this process's arguments (CommandLineToArgvW of the command line,
//     everything after the program name, quoted again by append_argument, so each argument arrives as it was given) and the working
//     directory the process started in (taken before main, so zodiac run's and the release's change to the game folder does not count;
//     relative paths on the command line keep their meaning). So zodiac run game.zs (zodiac.exe running a script in dev mode) runs the
//     script again, elevated, and a release starts the release again. started: the elevated copy is running; declined: the user said no
//     at the UAC prompt (ERROR_CANCELLED); failed: anything else (no error dialog is shown, SEE_MASK_FLAG_NO_UI). The UAC prompt names
//     its owner as the process's own foreground window (only when user32 is already loaded, so no game imports user32 for this) or its
//     console window. An elevated process cannot share its parent's console: started from a console, the copy opens a console of its own.
//   - POSIX: already when euid is 0, failed otherwise: there is no portable way to elevate (sudo, pkexec and doas all differ and all
//     need a terminal or a desktop agent), so a game asks its player to start it as root.
// - Accessibility (for the user documentation, Task 14): UAC's prompt appears on the secure desktop, where only an installed screen
//   reader runs. NVDA reads it only when NVDA is installed, not from its portable copy; Narrator always does. A blind player using a
//   portable NVDA hears nothing at the prompt (Alt+Y accepts it, Escape declines).
// Threads: both may be called from any thread.
#pragma once

namespace zodiac::platform {
bool is_elevated() noexcept;
enum class Elevation{already,started,declined,failed};
Elevation restart_elevated() noexcept;
}
