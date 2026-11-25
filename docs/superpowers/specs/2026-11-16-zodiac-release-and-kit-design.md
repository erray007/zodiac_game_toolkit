# Zodiac Release Builds and Installable Kit — Design

Status: approved by the owner (2026-11-16). Plan 3 of the series in `docs/superpowers/specs/2026-11-14-bgdk-design.md` (the "foundation spec"). It implements the foundation spec's release build flow (its 8.4) and goal G4, and adds the kit's distribution packages.

## 1. Intent

Zodiac is distributed as an installable package. A game developer installs only Zodiac and can then build a release of their game: one executable containing exactly the modules the game uses, with no Visual Studio, GCC or any other toolchain on their machine. Installing must be easy for a blind beginner, and must also work without administrator rights (school or work computers). Double-clicking a `.zs` file runs it, as BGT and NVGT users expect.

## 2. Goals

- **R1.** `zodiac build` turns a project into a release executable on a machine that has only the Zodiac kit installed.
- **R2.** The release executable contains only the declared modules (static by default), with per-module `dynamic` shipping next to it (foundation spec 8.3, 8.4).
- **R3.** A release build behaves exactly like `zodiac run` on the same project (foundation anti-goal: no silent dev/release differences).
- **R4.** The kit ships as both a setup wizard (`zodiac-setup-<version>.exe`) and a portable zip, built from the same CI output.
- **R5.** Double-clicking a `.zs` file runs it; right-click offers a release build whose result is shown in an accessible window.
- **R6.** Release outputs and the kit are ready for code signing, with signing switched off until a certificate exists.

## 3. Non-goals

- Linux kit packages (`.tar.gz` + install script, later `.deb`): with the Linux host plan (plan 10). Windows→Linux game builds: plan 6.
- Embedding assets into the executable or a pack file: needs the virtual file system (core platform plan) and protection (plan 7). Plan 3 copies assets next to the executable.
- Script/asset encryption: plan 7.
- A kit auto-updater. Installing a newer setup over an older one upgrades in place.
- Buying or configuring a code-signing certificate (owner's decision: unsigned for now; apply to SignPath Foundation once the repository is public).

## 4. Anti-goals

- A release that needs anything installed on the player's machine beyond the game folder (no VC++ redistributable, no runtime DLLs other than the developer's `dynamic` modules).
- A setup wizard that a screen-reader user cannot complete, or that requires sighted help for any step.
- A "hello world" release that grows because of build machinery (foundation anti-goal on bloat).
- Behavior that differs between double-clicking a `.zs` file and `zodiac run` on the same project.

## 5. Constraints

- Windows x86_64, Windows 10 or later; toolchain llvm-mingw UCRT as pinned in `tools/toolchain.json` (foundation spec 8.8).
- The kit's linker is LLVM `lld` from the same pinned llvm-mingw release used to build the kit, so module libraries and linker always match.
- Release builds use full LTO (`cmake/ZodiacReleaseFlags.cmake`), so the kit's `lib/release` archives hold LLVM bitcode, not native objects, and `zodiac build` links every game through lld's LTO backend. A static module library linked by `zodiac build` must therefore hold native objects, or bitcode from the kit's exact LLVM version (the pinned llvm-mingw release); bitcode from any other LLVM version makes lld fail with "Invalid bitcode".
- Everything redistributed in the kit respects its license (lld: Apache-2.0 with LLVM exception; mingw-w64 runtime: ZPL/public domain; libc++/compiler-rt: Apache-2.0 with LLVM exception). The kit carries a `THIRD_PARTY_LICENSES` file.
- AngelScript bytecode is only valid against an identical registered interface, so release startup must register exactly the same modules, in the same order, with the same declarations, as the compile step.

## 6. Approaches considered

**Kit distribution (Windows)**

| Option | Summary | Trade-off |
|---|---|---|
| A. Setup wizard only | Installs, adds PATH, file association, uninstaller. | Easiest for beginners; needs admin unless per-user install is offered. |
| B. Portable zip only | Unzip anywhere and run. | No admin needed; PATH and associations are manual. |
| **C. Both (chosen)** | Same CI output packaged twice. | Covers beginners and locked-down machines; two packages to test. |

**`.zs` double-click without a `zodiac.toml`**

| Option | Summary |
|---|---|
| **A. Load every installed module (chosen first, superseded: see row B and kit usability spec item 8)** | One-file experiments (`speak("hello")`) just work, as in NVGT. `zodiac build` still required a project file (later changed: it builds a lone script with the core only). |
| B. Core only | Stricter; every experiment needs a project file. *Chosen later instead of A: `2026-11-16-zodiac-kit-usability-design.md` item 8.* |

**Code signing**

| Option | Summary |
|---|---|
| **A. Unsigned for now, signing-ready (chosen)** | Free; SmartScreen warns on first run. |
| B. SignPath Foundation | Free for open source; repository must be public. Planned later. |
| C. Paid certificate | Warning disappears at once; yearly cost. |

## 7. Decisions made with the project owner

- Zodiac is distributed as an installable kit; the developer needs nothing else to build games.
- Both a setup wizard and a portable zip.
- Double-clicking a `.zs` file runs it; with no `zodiac.toml` nearby every installed module is loaded. *Superseded by `2026-11-16-zodiac-kit-usability-design.md` item 8: without a `zodiac.toml` only the core is loaded.*
- The console window closes when the program ends, error or not. Developers who want to read errors run from a terminal or use `verbose` and the log file.
- Unsigned for now, designed so signing is one setting away; `zodiac build` can sign games with the developer's own certificate.

## 8. Design

### 8.1 Kit layout

Identical for the wizard and the zip:

```
Zodiac/
  bin/zodiac.exe, bin/zodiacw.exe
  bin/ld.lld.exe
  lib/release/          release stub, zodiac_core and AngelScript as static libraries, plus the CRT/libc++/compiler-rt subset needed to link
  modules/              shared modules (*.dll) for dev mode and dynamic shipping
  modules/static/       static module libraries (*.a)
  include/zodiac/       plugin.h, plugin.hpp for plugin authors
  docs/                 manual (en, tr) and reference, once they exist
  examples/
  LICENSE, THIRD_PARTY_LICENSES
```

`zodiac.exe` finds everything relative to its own location, so the zip works from any folder, including paths with spaces and non-ASCII characters.

### 8.2 `zodiac build`

`zodiac build [project_dir] [--output <dir>]` (default output `<project>/release/`):

1. Load and validate `zodiac.toml` (as `run` does). Missing file: error `zodiac.toml not found; create a project with zodiac new` (exit 2).
2. Resolve declared modules and dependencies into the registration order `run` uses.
3. Register them in a fresh engine from the kit's shared modules, compile the scripts, and save bytecode (`asIScriptModule::SaveByteCode`). **Debug info is kept by default (my call)** so release errors carry file and line exactly like `run`; `[build] strip_debug_info = true` strips it for smaller, harder-to-read bytecode, and errors then say `line unknown`. Plan 7 (protection) revisits this. Compile errors are reported exactly as `run` reports them.
4. Write a game payload: format version, kit version, the ordered module list with each module's version, `[core] exclude`, diagnostics settings, and the bytecode.
5. Link with the kit's `ld.lld`: release stub + `zodiac_core` + AngelScript + every `static` module library + the CRT subset, into `<output>/<name>.exe` (GUI subsystem; attaches to the parent console per foundation spec 8.10).
6. Embed the payload (method left to the builder, see 10).
7. Copy every `dynamic` module DLL next to the executable.
8. Copy the project's declared assets (`[build] assets`, glob patterns relative to the project) into `<output>/`, keeping their relative paths.
9. If signing is configured (8.6), sign the executable and dynamic modules.

Every step reports progress at `verbose` 3 and errors at level 1; `build` never leaves a half-written executable at the output path (write to a temporary name, rename at the end).

### 8.2a Build result window (decided by the owner)

- `zodiac build --no-window` writes progress, errors and the result to the console only. Terminals, scripts and CI use this flag.
- Without the flag, `zodiac build` opens a result window when it finishes (the right-click "Build with Zodiac" entry uses this form):
  - title `Zodiac build - <game name>`;
  - a read-only multi-line edit field holding the whole output, focused when the window opens, caret on the first line; the first line is the summary (`Build succeeded: <path to exe>` or `Build failed: <N> errors`), followed by every message with file and line;
  - buttons: Copy all, Open output folder (only after success), Close (also Esc).
  - Standard Win32 controls only (edit, buttons), each labeled, full keyboard operation, so NVDA and JAWS can read the output line by line.
- **My call:** without the flag the output also goes to the console when there is one, so nothing is lost. When no interactive desktop is available (a service session, or Linux without a display later), `build` behaves as if `--no-window` were given instead of hanging.
- **My call:** the right-click entry starts `zodiacw.exe build "<file>"`, a GUI-subsystem twin of `zodiac.exe` from the same sources, so no console window flashes up behind the result window. `zodiacw.exe` is in `bin/` next to `zodiac.exe`.
- The window belongs to the kit's build tool only; games never get it.

### 8.2b Application manifest: the Segment Heap (decided by the owner, 2026-11-25, Task 5c fix round 1)

- `zodiac.exe`, `zodiacw.exe` and every game `zodiac build` makes carry one application manifest, made from the template `src/platform/windows/application.manifest.in`. It declares `<heapType xmlns="http://schemas.microsoft.com/SMI/2020/WindowsSettings">SegmentHeap</heapType>` and the execution level, `<requestedExecutionLevel level="asInvoker" uiAccess="false"/>`: declaring a level keeps Windows' installer detection (its name heuristics: setup, update, patch, install) off every game.
- The execution level is a build-time parameter. CMake's `zodiac_manifest_object(<target> <level>)` configures the template per level and compiles it into a resource object holding one RT_MANIFEST resource 1. The kit stages `lib/release/zodiac_manifest.obj` (asInvoker), which `zodiac build` links into every game, and `lib/release/zodiac_manifest_admin.obj` (requireAdministrator). Task 9's `[release] run_as_admin = true` makes `zodiac build` link the second object instead of the first (in `build_command.cpp`, where the link inputs are listed), never both; no other resource script may emit a manifest.
- Windows 10 version 2004 (build 19041) and later then run the program on the Segment Heap. Older Windows (7, 8.1 and Windows 10 before 2004) ignores the element without an error and runs the program on the NT heap, exactly as before.
- Reason: on the NT heap, a process with millions of live blocks can get one request size (32 or 64 bytes in the measurements) stuck on a path about 50 times slower than the others for the whole session; the Segment Heap showed no such size in 60 processes, and per operation it was as fast or faster (0.93-0.98 of the NT heap's time). Zodiac's own block caches (String's heap blocks, ScriptArray objects) stay. mimalloc was measured too and is not shipped.
- Any later manifest entry (DPI awareness, the UTF-8 code page, supportedOS) goes into the same file, beside heapType.
- Verified by `test_segment_heap` and kit-clean-machine's run-time probe (`tests/fixtures/tools/heap_kind_probe.cpp`), which read the process heap's signature of zodiac.exe and of a built game; on Windows older than build 19041 the probe only reports.

### 8.3 Static module registration

- Building a module with `ZODIAC_MODULE_STATIC` makes `ZODIAC_DEFINE_MODULE` emit a uniquely named registration record (`zodiac_static_module_<name>`) pointing at the module's `zodiac_module_info`, placed in a dedicated linker section.
- A static archive member is only linked when something references it, so `zodiac build` passes `-u zodiac_static_module_<name>` for each static module; no compiler or generated C code is needed.
- At startup the release stub walks the section to find the linked records, then registers modules **in the order stored in the payload**, not section order. A static or dynamic module missing at startup, or a version different from the payload's, is a startup error at level 1 (exit 2), never undefined behavior.
- Dynamic modules are loaded from the executable's folder only (never the current directory or PATH).
- The same mechanism must be implementable for ELF and Mach-O (foundation spec 8.9 rule 7); plan 3 implements and tests COFF (Windows). ELF follows in plan 6.

### 8.4 Dev/release identity

- `run` and the release stub share one start-up path in core (resolve modules, register, load or compile, run); only the source of modules (shared libraries vs. linked records) and of the script (source vs. bytecode) differ.
- CI test: each script fixture that runs under `zodiac run` is also built with `zodiac build` and run; exit code and diagnostic output must match.

### 8.4a What "identical" means (my call)

- Script behavior (results, exit codes, exceptions and their messages, file and line) is identical.
- The only permitted differences are the ones the Script API spec names: `verbose` default (2 dev, 1 release), dev-only checks (race checker, yield watchdog), and the release-only automatic `<game>.log`. They only affect which diagnostics are written, never script behavior.
- The CI identity test runs both with the same explicit `--verbose` and compares exit code and diagnostic lines.
- Dev-only checks are compiled into a dev configuration of core (`zodiac.exe`) and excluded from the release configuration in `lib/release/`; the kit therefore ships two builds of `zodiac_core` from the same sources. Until plan 4 adds those checks, both configurations are the same code.

### 8.4b Paths (my call)

- **Project root:** the folder containing `zodiac.toml`; with no project file, the folder of the given `.zs` file. In a release build, the executable's folder.
- `#include` paths are relative to the including file (as now).
- Assets, worker scripts and `plugins/` are relative to the project root. Release copies assets keeping their relative paths, so the same relative path works in both.
- At start-up the working directory is set to the project root (dev) or the executable's folder (release), so BGT-style relative file access behaves the same.

### 8.4c Worker scripts (my call)

Workers (Script API spec 8.3) are declared in the project file so `zodiac build` can compile them, the same way modules are declared explicitly:

```
[workers]
scripts = ["bots.zs", "weapons.zs"]
```

Starting an undeclared worker is an error in dev mode too, so dev and release fail the same way. Without a project file, any `.zs` file can be started as a worker in dev mode. Plan 3 defines the payload so it can carry several compiled scripts; plan 4 implements workers.

### 8.4d Plugins and licenses in release builds (my call)

- Third-party plugins link `dynamic` by default: the DLL is copied from `<project>/plugins/` next to the executable.
- `static` linking of a plugin is allowed only for a plugin built with the kit's pinned toolchain: its `.a` (in `<project>/plugins/static/`) carries a toolchain stamp, and `zodiac build` refuses a missing or different stamp with a message saying to use `dynamic`.
- A module whose manifest does not allow `static` (e.g. LGPL) and is listed as `static` is refused (exit 2), naming the module and its license. Allowed but license-relevant static links produce a warning.

### 8.5 `zodiac run` with a `.zs` file

- `zodiac run` accepts a project directory (as now) or a `.zs` file.
- For a file: search the file's folder and its parents for `zodiac.toml`. Found: use that project's modules and settings, but run the given file as the main script. Not found: run the file with every installed module loaded, in the kit's fixed dependency order; this is logged at `verbose` 3. *Superseded by `2026-11-16-zodiac-kit-usability-design.md` item 8: not found means the core only, no modules.*
- Double-click runs exactly this command, so double-click and terminal behave identically. The console closes when the program ends.

### 8.6 Signing

- `zodiac.toml`:

```
[build.sign]
command = ["signtool", "sign", "/fd", "SHA256", "/a", "{file}"]
```

- `zodiac build` runs the command once per output executable and DLL, substituting `{file}`; a non-zero exit fails the build. Not configured: no signing, no warning.
- Kit CI has a signing step for `zodiac.exe`, `ld.lld.exe`, module DLLs and the setup executable that runs only when the signing secret is present.

### 8.7 Setup wizard

- Built with Inno Setup in CI (pinned version and checksum, like other tools).
- Install mode: the wizard asks "for all users" (admin, `Program Files\Zodiac`) or "only for me" (no admin, per-user folder).
- Options, each a checkbox (all checked by default):
  - add `zodiac` to PATH (system or user, matching the install mode);
  - associate `.zs` files: open runs `zodiac.exe run "%1"`;
  - add "Build with Zodiac" to the `.zs` right-click menu (runs `zodiacw.exe build` on the project the file belongs to and shows the result window, 8.2a);
  - Start menu shortcuts to the manual and examples.
- Uninstall removes exactly what was added (files, PATH entry, associations, menus, shortcuts) and nothing else.
- Upgrading: installing a newer version over an older one replaces the kit in place and keeps the chosen options.
- Accessibility: only standard Inno Setup pages and controls (no custom-drawn pages, no images carrying information), every control labeled, full keyboard operation.

### 8.8 Portable zip

`zodiac-<version>-windows-x86_64.zip`, the same layout as 8.1. No PATH, association or registry changes. The README tells how to add PATH manually.

### 8.9 CI

- Builds the kit layout, the zip and the setup wizard on every push to `main` and for tags; uploads them as artifacts. Tags `v*` publish a GitHub release with both packages and checksums.
- Clean-machine test: in a fresh Windows job without the dev toolchain on PATH, unzip the kit, `zodiac new`, `zodiac build`, run the produced executable, check the exit code and output.
- Size gate: a core-only "hello world" release must be at most 5 MB (raised to 5 MB by the owner, 2026-11-17), measured on Release builds; CI fails above it. (The foundation spec's full criterion with `audio` and `screen_reader` takes over in plan 5.)
- Silent install and uninstall of the setup wizard (`/VERYSILENT`) in CI, checking PATH, association and file removal.

## 9. Success criteria

- On a Windows machine with only the Zodiac kit (from the zip or the wizard), `zodiac new hello`, `zodiac build hello` produces `hello/release/hello.exe`, which runs on another clean Windows machine with nothing installed.
- The release executable contains no code from undeclared modules (verifiable by symbol inspection); a `dynamic` module ships as a DLL next to it and is loaded from there.
- Every script fixture gives the same exit code and diagnostics under `run` and as a release build.
- Double-clicking a `.zs` file runs it; with no project file, a script calling a function from an installed module runs. *Superseded by `2026-11-16-zodiac-kit-usability-design.md` item 8: with no project file a core-only script runs, and a module call fails to compile with the note line.*
- The owner completes the setup wizard and the uninstall with NVDA alone.
- A core-only hello world release is at most 5 MB (raised to 5 MB by the owner, 2026-11-17).

## 9a. Order (my call)

Plan 3 is implemented before the Script API runtime (plan 4). The payload format carries a version; plan 4 extends it (worker scripts are already representable) without breaking plan 3 builds of the same kit version. Releases are tied to the kit version that built them.

## 10. Left to the builder

- Payload embedding method (appended data vs. PE resource) and its compatibility with Authenticode signing (an appended payload must stay valid after signing).
- Exact CRT/libc++/compiler-rt subset and link command line.
- Section name and record layout for static registration, within 8.3.
- Inno Setup script details, per-user install folder, and the registry keys used.
- Exact `[build]` keys beyond `assets` and `sign`.
