# Zodiac Library and Packs (Plan 4b) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give scripts maths, random, time and files through a case-insensitive virtual file system, and let games and scripts keep their files in authenticated, encrypted packs that `zodiac build` makes and patches automatically.

**Architecture:**
- The script library lives in `src/core/library/` (maths, random, time); the VFS in `src/core/vfs/` (paths, mount table, folder source, `file` object and helpers); packs in `src/core/pack/` (crypto, format, storage, mount source, script `pack` object, release trailer and start-up mounts).
- `ScriptRuntime` owns one `vfs::Vfs`, registers every new binding, and hands the Vfs to `Host` for the plugin entries. `zodiac run`, `zodiac build`'s compile and the release stub mount it before any script code runs, so dev and release resolve the same paths.
- Release packing (`src/cli/release_packing.*`, `src/cli/pack_key.*`, `src/cli/literal_paths.*`) runs inside `write_release`: the pack is appended to the linked executable before signing, or written as `<game>.zpk`.

**Tech Stack:** C++23, clang (llvm-mingw on Windows, clang 18 + libc++ on Linux), AngelScript 2.39.0, Monocypher 4.0.2 (Poly1305, BLAKE2b, Argon2id, reference ChaCha20), doctest, Python e2e tests, CMake + Ninja.

**Spec:** `docs/superpowers/specs/2026-11-19-zodiac-library-and-packs-design.md`, with `docs/superpowers/specs/2026-11-16-zodiac-core-types-design.md` sections 5, 8.4-8.7 and 9 carried over unchanged.

## Global Constraints

- English only in code, comments and commit messages.
- No indentation where not needed: flush-left style like the existing C++ and CMake files; a header comment per file describing its contract. Python keeps 4-space indentation.
- Edit files with the Edit and Write tools only: no heredoc, no `sed -i`, no shell redirection into source files.
- Never touch the untracked `D:\bgdk\test.zs`.
- Build trees on C: (D: is nearly full): `C:/zbuild/debug` (`.tools/cmake/bin/cmake --preset windows-debug -B C:/zbuild/debug`) and `C:/zbuild/release` (`--preset windows-release -B C:/zbuild/release`).
- Commit often. Push only `feat/library-and-packs`.
- Release speed rule: the bench gate ratio is at most 1.05. When a change is compared against a baseline, the paired geomean is at most 1.00 and no op is above 1.02 beyond its A/A noise in every pass.
- Hello size gate: 5 MB (5242880 bytes), measured in Release only (`test_hello_size_gate`).
- Dev and release are identical: the same paths open the same bytes; only the source differs.
- Plugin ABI is append-only: no existing member of `plugin.h` moves, changes or disappears.
- clang only, C++23. Windows and Linux CI green after every pushed task.
- One-line diagnostics through `Diagnostics`; script errors are catchable script exceptions (`raise_script_exception` / `guarded<>` in `src/core/types/script_guard.hpp`), never C++ exceptions into AngelScript, never undefined behaviour on bad input.
- Thread rules: `file`, `pack`, `timer`, `random_generator` and `date_time` objects are not internally synchronized. The Vfs mount table, pack storage and every read through a mount are safe from several threads. The global random generator is `thread_local`.
- Third-party code needs a permissive licence recorded in `THIRD_PARTY_LICENSES`.
- Test commands:
  - Build with `.tools/cmake/bin/cmake --build C:/zbuild/debug` and run one test with `.tools/cmake/bin/ctest --test-dir C:/zbuild/debug -R <name> --output-on-failure`.
  - In a "verify they fail" step, the expected result is a compile error, a link error or a failing assertion.
  - The e2e suites run through ctest (`cli_e2e`, `build_e2e`, `kit_e2e`).
- Owner rule: after each task's review is clean, stop and ask the owner before starting the next one.
- Exact texts (each `<path>` is the script's path as given, through `one_line`):
  - `"<path>" leaves its mount`; `"<path>" is a host path; set [files] host_paths = true to use it`; `"<path>" is not a valid file name`; `"<path>" matches both "<a>" and "<b>"; rename one of them`; `"<path>" matches more than one name; rename one of them` (an ambiguity whose source cannot name the matches).
  - `cannot read "<path>": <reason>` (reading, `r`, `r+` on a missing file, the from side of copy and move); `cannot write "<path>": <reason>` (`w` and `a`, `r+` otherwise, `file.write`, `file.sync`, the commit in `file.close`, `write_file`, `append_file`, the to side of copy and move, `mount_pack` with writable on a pack that cannot be changed, naming the pack's path); `cannot delete "<path>": <reason>` (`delete_file`, `delete_directory`, the source delete of a move made by copy and delete); `cannot create "<path>": <reason>` (`create_directory`). Reasons `file not found`, `access denied`, `damaged data`, `is a directory`, `directory not empty`, `disk full`.
  - `"<path>" is in a read-only pack; save files in data:/ instead`.
  - `"<path>" is in use by another program` (a writable pack open while another process holds the pack open for writing).
  - `wrong key or damaged pack`; `integer overflow`; `file is not open`; `file is not open for writing`; `"<mode>" is not a file mode`; `random_choice on an empty array`; `clamp: low is greater than high`; `invalid date`.
  - `[release] run_as_admin must be a boolean` (a project file error, after `zodiac.toml: error: ` like the others).
  - `error: .zodiac/release.manifest is damaged; restore it from your backup or delete it` (zodiac build, an earlier manifest whose first three lines are wrong); `error: "<game>.zpk" is in use by another program` (a release start whose writable pack another copy of the game holds); `error: the game data is damaged (pack: unsupported pack (newer format))`; `error: cannot mount the game data: out of memory`.

## Review Focus

1. **Turkish and other non-ASCII names in case-insensitive lookup.** A disk file `IŞIK.ogg` must be found as `ışık.ogg` and `işik.ogg` but not as `İŞİK.ogg`, on Windows and Linux alike. The fold is Unicode simple uppercase per code point, as NTFS's upcase table does, so Linux answers like Windows. NFD names are not merged with NFC ones. The tests are in Task 4.
2. **BGT-style backslashes and Windows device names in script paths.** `sounds\shot.ogg` opens `sounds/shot.ogg`. `con`, `aux.txt`, `com1.log`, `nul`, and names ending in `.` or space are refused with `"<path>" is not a valid file name` on every platform, so a save named after player input never opens a device on Windows and never behaves differently on Linux. The tests are in Task 4.
3. **Reads at chunk edges and large offsets.** These cover entries of 0, 1, 65535, 65536, 65537 and 3×65536+7 bytes, a read starting at 65535 for 3 bytes, a seek to exactly 65536, a read spanning three chunks, and index offsets of 2^32 and more. A disk file of more than 4 GiB is also read through `file`. The tests are in Tasks 5 and 7.
4. **A pack changed while it is mounted or read.** The pack object adds entries to a mounted pack while other threads read through the mount. A second `pack` object opens the same file. `compact()` replaces the file while a `file` object still reads the old one. Each gives correct bytes and no corruption. The tests are in Task 8.
5. **A signed release executable.** Signing appends a certificate table after the pack, so the trailer is not at the end of the file. The game must still find its pack, and the signature must cover the pack. The tests are in Task 10.

---

### Task 1: Maths

**Files:**
- Create: `src/core/library/library.hpp`, `src/core/library/round.hpp`, `src/core/library/round.cpp`, `src/core/library/math_binding.cpp`
- Modify: `src/core/CMakeLists.txt` (sources), `src/core/script_runtime.cpp` (register after the dictionary), `tests/script_fixture.hpp` (`create_engine` registers the same), `cmake/Dependencies.cmake` (add `scriptmath/scriptmath.cpp` to `angelscript_sample_addons`), `tests/check_no_sample_addons.cmake` (also refuse `RegisterScriptMath`), `tests/bench/bench_types.cpp`, `tests/CMakeLists.txt`
- Test: `tests/test_math.cpp`

**Interfaces:**
- Produces in `library.hpp` (namespace `zodiac`): `void register_math(asIScriptEngine*);` `void register_random(asIScriptEngine*);` `void register_time(asIScriptEngine*);` (the last two are defined in Tasks 2 and 3). Each throws `std::runtime_error` naming the declaration when the engine refuses one, as `register_exception_helpers` does.
- Produces in `round.hpp`: `double round_half_away(double x) noexcept;` `double round_digits(double x,int digits) noexcept;` and `double round_digits_exact(double x,int digits) noexcept;` (the exact decimal path alone, exported for the property test).
- Script API: `abs`, `min`, `max` and `clamp` for `int`, `int64`, `float` and `double`. `floor`, `ceil`, `round(x)`, `round(x, digits)`, `trunc`, `sqrt`, `pow`, `exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `hypot` and `fmod` on `double`. `const double PI, TAU, E`. Everything is native `asCALL_CDECL`. Only `clamp` and integer `abs` can throw.

- [ ] **Step 1: Write the failing tests** in `tests/test_math.cpp`:
  - C++: `round_half_away(0.5)==1`, `round_half_away(-2.5)==-3`, `round_half_away(2.4999999999999996)==2`.
  - `round_digits(2.675,2)==2.68`, `round_digits(1.005,2)==1.01`, `round_digits(-0.125,2)==-0.13`, `round_digits(1234.5,-2)==1200`, `round_digits(0.1+0.2,15)==0.3`, `round_digits(1e300,2)==1e300`. NaN and ±inf come back unchanged; `digits>=17` returns `x`.
  - Property, 1 000 000 values with a fixed seed: `round_digits(x,d)==round_digits_exact(x,d)`.
  - Script: `abs(-3)==3`, `abs(int(-2147483648))` throws `integer overflow`, `clamp(5,1,3)==3`, `clamp(1.0,3.0,2.0)` throws `clamp: low is greater than high`, `min(2.5,1.0)==1.0`, `floor(-1.5)==-2`, `trunc(-1.5)==-1`, `abs(PI-3.141592653589793)<1e-15`, `abs(TAU-2*PI)<1e-15`, `E==exp(1.0)`, `hypot(3,4)==5`, `fmod(7,3)==1`.
- [ ] **Step 2: Run to verify they fail.** Run `.tools/cmake/bin/cmake --build C:/zbuild/debug --target test_math`. The expected result is a compile or link failure.
- [ ] **Step 3: Implement `round_digits`.**
  - The exact path takes the shortest round-trip form of `x` (`std::to_chars(double)`), rounds its decimal digits half away from zero at `digits` places, and parses the result with `std::from_chars`. Binary representation error therefore never shows.
  - The fast path is `std::round(x*p)/p` with `p=10^digits` from a table. Use it only when `|x*p| < 2^52` and the fractional part of `|x*p|` is farther than 2^-20 from 0.5. Everything else takes the exact path.
- [ ] **Step 4: Register the maths.** Then add bench op pairs to `bench_types`: `math_sqrt`, `math_sin`, `math_floor`, `math_pow` against the scriptmath add-on, and `math_abs_int`, `math_min_double`, `math_clamp_double`, `math_round`, `math_hypot`, `math_round_digits` against bench-only native functions on the add-on engine. Those natives are plain C++ (`std::abs`, `std::min`, `std::clamp`, `std::round`, `std::hypot`, and `std::round(x*p)/p` for `round_digits`).
- [ ] **Step 5: Verify.**
  - Run `ctest --test-dir C:/zbuild/debug -R "test_math|test_angelscript_has_no_sample_addons" --output-on-failure`. Expect PASS.
  - Run the full suite once.
  - On `C:/zbuild/release`, run `bench_types --op math_...` for every new op and `check_bench.py`. Expect every ratio ≤ 1.05.
  - If `math_round_digits` cannot meet the gate after optimization, STOP and report the numbers. That is a ruling (spec 8.1), not a waiver.
- [ ] **Step 6: Commit** with `git commit -m "Add maths functions with half-away-from-zero and decimal rounding"`.

### Task 2: Random numbers

**Files:**
- Create: `src/core/library/random.hpp`, `src/core/library/random.cpp`, `src/core/library/random_binding.cpp`
- Modify: `src/core/CMakeLists.txt`, `src/core/script_runtime.cpp`, `tests/script_fixture.hpp`, `tests/bench/bench_types.cpp`, `tests/CMakeLists.txt`
- Test: `tests/test_random.cpp`

**Interfaces:**
- Consumes: `platform::fill_entropy` (`src/platform/entropy.hpp`).
- Produces `class zodiac::Xoshiro256` (32 bytes of state, trivially copyable) with these members:
  - `void seed(std::uint64_t)`: SplitMix64 expansion, where state word i is the i-th SplitMix64 output starting from the seed.
  - `void seed_from_entropy() noexcept`.
  - `std::uint64_t next() noexcept`.
  - `std::int64_t range(std::int64_t min,std::int64_t max) noexcept`: inclusive, swaps `min>max`, Lemire's unbiased multiply-shift.
  - `double unit() noexcept`: `(next()>>11)*0x1p-53`.
  - `bool coin() noexcept`: the top bit.
- Produces `Xoshiro256& thread_generator() noexcept;`. It is `thread_local` and seeded from entropy on its first use in each thread.
- Script API:
  - Global functions: `int random(int min, int max)`, `double random_float()`, `double random_float(double min, double max)`, `bool random_bool()`.
  - `random_generator` is a value type (`asOBJ_VALUE|asOBJ_POD|asOBJ_APP_CLASS_ALLINTS`). A default-constructed one is seeded from entropy. It has `void seed(uint64)` and the same four methods.
  - **Controller ruling, deviating from core 8.5's `random_choice(array)` spelling:** AngelScript 2.39.0 cannot register a global template function, so `random_choice` is a method of `array<T>`. There are two overloads, `const T& random_choice() const` and `const T& random_choice(random_generator &inout) const`.
    - `register_random` adds both with `RegisterObjectMethod("array<T>", ...)` after it has registered `random_generator`. `array.cpp` is not changed.
    - An empty array throws `random_choice on an empty array`.

- [ ] **Step 1: Write the failing tests**:
  - Golden values for `seed(42)`: `next()` returns `0x15780b2e0c2ec716`, `0x6104d9866d113a7e`, `0xae17533239e499a1`, `0xecb8ad4703b360a1`.
  - After `seed(42)`, `range(1,100)` five times gives `9, 38, 69, 93, 100`.
  - After `seed(42)`, `unit()` gives `0.08386297105988216`, `0.3789802506626686`, `0.6800434110281394`.
  - `range(5,1)` stays in [1,5]. `range(INT64_MIN,INT64_MAX)` does not hang. `random(3,3)==3`. `random_float()` lies in [0,1).
  - Chi-square test of `range(0,9)` over 100 000 draws, below the 99.9 % bound.
  - Script: two generators seeded 42 give the same sequence. With `array<int> a = {1,2,3};`, both `a.random_choice()` and `a.random_choice(g)` return one of the elements. With `array<int> e;`, `e.random_choice()` throws the exact text.
  - Two threads' `thread_generator()` objects are distinct and both seeded.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement and register.** Add bench pairs `random_int`, `random_float`, `random_float_range`, `random_bool`, `random_choice` and `generator_random_int`. Their counterpart is a bench-only native binding of `std::mt19937_64` with `std::uniform_int_distribution` and `std::uniform_real_distribution`, the plain C++ a binding would use.
- [ ] **Step 4: Verify.** Run `ctest --test-dir C:/zbuild/debug -R test_random`, then the full suite once. On `C:/zbuild/release`, every new bench op must be ≤ 1.05.
- [ ] **Step 5: Commit** with `git commit -m "Add xoshiro256** random numbers seeded from the entropy source"`.

### Task 3: Time

**Files:**
- Create: `src/platform/clock.hpp`, `src/platform/windows/clock.cpp`, `src/platform/posix/clock.cpp`, `src/core/library/date_time.hpp`, `src/core/library/date_time.cpp`, `src/core/library/time_binding.cpp`
- Modify: `src/core/CMakeLists.txt` (platform and library sources), `src/core/script_runtime.cpp`, `tests/script_fixture.hpp`, `tests/CMakeLists.txt`
- Test: `tests/test_time.cpp`

**Interfaces:**
- Produces in `platform/clock.hpp` (all thread-safe):
  - `std::int64_t monotonic_us() noexcept;`: QueryPerformanceCounter or `CLOCK_MONOTONIC`, never going back.
  - `std::int64_t utc_now_ms() noexcept;`
  - `std::int64_t utc_to_local_civil_ms(std::int64_t utc_ms) noexcept;`: local wall-clock time as milliseconds since 1970-01-01T00:00 read as if it were UTC, using `localtime_r` or `SystemTimeToTzSpecificLocalTime`.
- Produces `struct zodiac::DateTime{std::int64_t civil_ms;}`:
  - **My call:** it is a zone-less civil time, like BGT's dates.
  - `static std::optional<DateTime> from_parts(int year,int month,int day,int hour,int minute,int second,int millisecond)`.
  - `year()`, `month()`, `day()`, `hour()`, `minute()`, `second()`, `millisecond()`, `weekday()` (0 = Monday) and `day_of_year()` (1-366), using Hinnant's days-from-civil algorithm.
  - `String format(std::string_view fmt) const`.
- Script API:
  - `int64 ticks()`: ms since process start, from `monotonic_us`. `int64 ticks_us()`.
  - `timer` is a value type and starts running:
    - `elapsed` (int64 ms) and `elapsed_seconds` (double) are `property` getters.
    - `restart()` sets it to 0 and running.
    - `pause()` and `resume()` are idempotent.
    - `running` is a getter.
    - `set(int64 ms)` sets `elapsed` and keeps the running state.
  - `date_time` is a value type:
    - `date_time::now()` (local) and `date_time::utc_now()` (registered in namespace `date_time`).
    - **My call:** the constructor `date_time(int year, int month, int day, int hour = 0, int minute = 0, int second = 0, int millisecond = 0)` throws `invalid date`. Tests and saves need a fixed date.
    - Getters for every part.
    - `to_string(const string &in format = "{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}")` supports the tokens `{yyyy} {yy} {MM} {M} {dd} {d} {HH} {H} {mm} {ss} {fff}`. `{{` and `}}` escape a brace, and an unknown token stays literal.
    - `date_time + int64 ms`, `- int64`, `+=` and `-=`.
    - `date_time - date_time` gives `int64` ms.
    - `opCmp` and `opEquals`.

- [ ] **Step 1: Write the failing tests**:
  - In script, `date_time(2024,2,29)+int64(86400000)*365` is 2025-02-28 (int64, because `86400000*365` overflows `int`). `date_time(2024,12,31).day_of_year()==366`. `date_time(2026,11,19).weekday()==3`. `date_time(2023,2,29)` throws `invalid date`.
  - `date_time(2026,1,2,3,4,5,6).to_string("{yyyy}/{M}/{d} {H}:{mm}:{ss}.{fff} {x} {{")=="2026/1/2 3:04:05.006 {x} {"`.
  - Ordering, and `date_time(2026,1,2)-date_time(2026,1,1)==86400000`.
  - `ticks()` never decreases over 10 000 calls. `ticks_us()/1000` stays within 2 ms of `ticks()`.
  - `timer`: paused for 30 ms, `elapsed` does not grow. `set(5000)` makes `elapsed` at least 5000. `restart()` makes it below 20.
  - `abs((date_time::now()-date_time::utc_now()) - (utc_to_local_civil_ms(t)-t)) < 1000`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement and register.**
- [ ] **Step 4: Verify.** Run `ctest -R test_time`, then the full suite once.
- [ ] **Step 5: Commit** with `git commit -m "Add ticks, timer and date_time"`.

### Task 4: VFS core (paths, mounts, case-insensitive folders, start-up mounts)

**Files:**
- Create: `src/platform/file.hpp`, `src/platform/windows/file.cpp`, `src/platform/posix/file.cpp`, `src/core/vfs/path.hpp`, `src/core/vfs/path.cpp`, `src/core/vfs/vfs.hpp`, `src/core/vfs/vfs.cpp`, `src/core/vfs/folder_source.hpp`, `src/core/vfs/folder_source.cpp`, `src/core/vfs/host_source.hpp`, `src/core/vfs/host_source.cpp`, `src/core/vfs/startup.hpp`, `src/core/vfs/startup.cpp`
- Modify: `src/core/project_file.hpp`/`.cpp` (`[files] host_paths`), `src/core/script_runtime.hpp`/`.cpp` (owns `vfs::Vfs vfs_`, `vfs()` accessor, log handler), `src/cli/main.cpp` (`run_game`: `mount_project` with dev mode after `ScriptRuntime` and before `build`), `src/cli/build_command.cpp` (compile mounts like run, without dev mode), `src/release/stub.cpp` (`run_game`: `mount_game_folders` for the executable's folder after `ScriptRuntime` and before `load_bytecode`), `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/e2e/test_build.py`, `tests/e2e/test_cli.py`, `tests/e2e/test_kit.py` (every subprocess gets `APPDATA`/`XDG_DATA_HOME` pointing into the test's temp folder)
- Test: `tests/test_vfs.cpp`, `tests/test_project_file.cpp`, `tests/test_release_stub.cpp`

**Interfaces:**
- Produces `platform/file.hpp` (namespace `zodiac::platform`):
  - `class File` (move-only):
    - `static std::expected<File,std::error_code> open(const std::filesystem::path&,FileAccess)` with `FileAccess{read,read_write,create_truncate,create_or_open}`. Windows opens with `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE`.
    - `std::int64_t read_at(std::uint64_t offset,std::span<std::byte>) const noexcept`: positional (`pread` / `ReadFile` with `OVERLAPPED`), bytes read, 0 at end, negative `-errno`-style on error. It is safe from several threads on one `File`.
    - `bool write_at(std::uint64_t,std::span<const std::byte>) noexcept`, `std::uint64_t size() const`, `bool truncate(std::uint64_t)`, `bool sync()`.
  - `bool replace_file(const path& from,const path& to,std::error_code&)`: on Windows `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS|POSIX_SEMANTICS)`, falling back to `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`; on POSIX `rename`.
  - `std::filesystem::path user_data_folder()`: `%APPDATA%`, else the roaming known folder, on Windows; `$XDG_DATA_HOME`, else `$HOME/.local/share`, elsewhere. Empty when none.
- Produces `vfs/path.hpp` (namespace `zodiac::vfs`):
  - `struct VfsPath{std::string mount; std::string relative; std::filesystem::path host;}`.
    - `mount` is `""` for `/`, `"data:"` for `data:/`, and `"host:"` for an accepted host path.
    - `relative` is a normalized `/`-joined form with no leading slash, empty for a mount's folder itself.
    - `host` is set only for `"host:"`: the absolute host path, lexically normalized.
  - `std::expected<VfsPath,std::string> parse_path(std::string_view path,bool host_paths)`:
    - `\` counts as `/`, and repeated separators collapse (`a//b` is `a/b`).
    - `.` segments are dropped. A `..` above the mount root gives `"<path>" leaves its mount`.
    - `""`, `"/"`, `"data:/"` and a trailing `/` (`"sounds/"`) name the folder itself. They are valid wherever a folder is expected (`list_files`, `directory_exists`, `create_directory`, mount points). A file operation on them fails with `is_directory`.
    - **My call:** a host path is a drive letter followed by `:`, a leading `\\` or `//`, or a `host:` prefix. A leading `/` always means the VFS root, also on Linux, where host paths are written as `host:/...`. A host path is refused unless `host_paths` is set.
    - A component is refused with `"<path>" is not a valid file name` when:
      - it ends in `.` or space, or holds a control character or one of `<>:"|?*`;
      - or its stem before the first dot is a device name (ASCII case-insensitive): `CON PRN AUX NUL CONIN$ CONOUT$ CLOCK$`, `COM0-9` and `LPT0-9`, and the superscript forms `COM¹ COM² COM³ LPT¹ LPT² LPT³`.
    - Path-rule failures (leaves its mount, host path, not a valid file name, ambiguous) are thrown or reported with their own Global texts. They are never wrapped as `cannot read "<path>": <reason>`, whose reasons are only those listed in the Global Constraints: `file not found`, `access denied`, `damaged data`, `is a directory` and `directory not empty`.
  - `std::string fold_name(std::string_view utf8)`: per code point `unicode::to_upper`. Invalid bytes are kept, and nothing is normalized.
- Produces `vfs/vfs.hpp`:
  - `enum class FileError:int{not_found=1,access_denied,damaged,is_directory,not_a_directory,read_only_pack,bad_path,ambiguous,io,not_empty}` and `std::string_view reason_text(FileError)`. The texts are `file not found`, `access denied`, `damaged data`, `is a directory`, `not a directory`, `in a read-only pack`, `invalid path`, `ambiguous name`, `input/output error`, `directory not empty`.
  - `struct EntryInfo{EntryKind kind; std::uint64_t size; std::int64_t modified_utc_ms; std::string name;}` with `enum class EntryKind{none,file,directory,deleted}`. `deleted` is a deletion marker, used from Task 8 on.
  - `class Reader`: `virtual std::int64_t read_at(std::uint64_t offset,std::span<std::byte>)` (bytes, 0 at end, `-int(FileError)` on failure) and `virtual std::uint64_t size() const`. A reader is used by one thread at a time; different readers may run concurrently.
  - `class WriteTarget`: `write_at`, `size`, `truncate`, and `virtual std::expected<void,FileError> commit()`.
  - `class Source`: `stat(relative)`, `open_read(relative)` returning `std::expected<std::shared_ptr<Reader>,FileError>`, `open_write(relative,WriteMode)` with `WriteMode{truncate,append,update}`, `list(relative,std::vector<EntryInfo>&)`, `remove(relative)`, `make_directory(relative)`, `remove_directory(relative,bool recursive)`, `bool read_only() const`.
  - `class Vfs`:
    - Mounting: `mount(std::string_view at,std::shared_ptr<Source>,bool writable,const void* owner)` returns bool, `unmount(const void* owner)`, and `mount_folder(std::string_view at,const std::filesystem::path& host,bool writable)`.
    - File operations: `stat(std::string_view path)`, `open_read(std::string_view path)`, `open_write(std::string_view path,WriteMode)`, `remove(path)`, `list(path,ListKind{files,directories},std::string_view pattern)` returning sorted unique names, `create_directory(path,bool recursive)`, `delete_directory(path,bool recursive)`.
    - Settings: `set_host_paths(bool)`, `set_dev_mode(bool)`, `set_log(std::function<void(zodiac_log_level,std::string_view)>)`, `set_release_pack_check(std::function<bool(std::string_view relative)>)` (Task 10 sets it).
    - Locking: the mount table is copy-on-write behind a `std::shared_mutex`. A lookup copies a `shared_ptr<const Table>` under the shared lock and resolves without holding it.
- `FolderSource`:
  - Exact name first.
  - On a miss, it reads a per-folder cache of `fold_name → real names`. The cache is guarded by a mutex, invalidated by Zodiac's own writes in that folder, and rescanned when the folder's modification time has changed.
  - More than one match gives `ambiguous` with the message text above.
  - In dev mode every successful lookup also checks the spelling of each component against the cache, and warns once per path: `warning: "<path>" was found as "<real path>"; use the real name, other systems may not find it`.
- `HostSource` (`host_source.hpp`): a writable `Source` without a case cache. The Vfs routes every `"host:"` `VfsPath` to it, and it opens `VfsPath::host` exactly as given. It is used only when `host_paths` is set.
- `vfs/startup.hpp` holds one shared helper and the dev wrapper. Task 10's `mount_release` also calls the helper.
  - `void mount_game_folders(Vfs&,const std::filesystem::path& root,std::string_view game_name,bool host_paths,bool dev_mode)` mounts `root` read-write at `/`, sets `host_paths` and dev mode, and mounts `user_data_folder()/<game name>` at `data:/`. **My call:** the data folder is created on the first write, not at start-up.
  - `void mount_project(Vfs&,const Project&,bool dev_mode)` calls it with the project root, `[files] host_paths` and the game name.
  - From this task on, the release stub calls `mount_game_folders(vfs, executable folder, payload.game_name, false, false)`, so a built game has `/` and `data:/` and identity holds. Task 9 passes the payload's `host_paths` instead of `false`, and Task 10 adds only the pack mount.
- `Project` gains `struct FilesConfig{bool host_paths=false;} files;`. `[files] host_paths` must be a boolean: `[files] host_paths must be a boolean`.

- [ ] **Step 1: Write the failing tests** in `tests/test_vfs.cpp`, with temp folders mounted explicitly:
  - `parse_path` cases:
    - `"a/./b/../c.txt"` gives `"a/c.txt"`.
    - `"../x"` and `"data:/../x"` give `"<path>" leaves its mount`.
    - `"C:/x"`, `"c:\\x"`, `"//srv/x"` and `"host:/etc/x"` are refused unless `host_paths` is set.
    - `"/sounds/a.ogg"` gives mount `""`.
    - `"data:/saves/s1.txt"` gives mount `"data:"`.
    - `""`, `"/"`, `"data:/"` and `"sounds/"` parse as folders, and `"a//b"` gives `"a/b"`.
    - With `host_paths` set, `"host:<temp>/x.txt"` (and on Windows `"<drive>:/<temp>/x.txt"`) gives mount `"host:"`. The Vfs reads and writes it through `HostSource`.
  - Review Focus 2:
    - `"sounds\\shot.ogg"` reads `sounds/shot.ogg`.
    - These give `"<path>" is not a valid file name`: `"con"`, `"aux.txt"`, `"COM1.log"`, `"nul"`, `"lpt9"`, `"CONIN$"`, `"conout$.txt"`, `"CLOCK$"`, `"COM¹.txt"`, `"lpt³"`, `"a."`, `"a "` and `"a|b"`.
    - `"console.txt"` and `"com10.txt"` are allowed.
    - The results are the same on both platforms.
  - Case-insensitive lookup:
    - `Sounds/Shot.ogg` finds `sounds/shot.ogg`.
    - In dev mode that logs the exact warning once and never in release mode.
    - On Linux, `a.txt` and `A.txt` with a lookup of `A.TXT` give the ambiguous error. Exact `a.txt` still opens.
    - A file created by another process after the cache was built is found by its other-case name, through the modification-time rescan.
  - Review Focus 1:
    - With `IŞIK.ogg` on disk, `ışık.ogg` and `işik.ogg` are found and `İŞİK.ogg` is not found.
    - With `şehir` stored as NFD, an NFC `şehir` lookup is not found.
    - The results are identical on Windows and Linux.
  - Concurrency: 8 threads doing 10 000 `stat` and `open_read` calls while the main thread mounts and unmounts a second folder 1 000 times. No failure, and the stable file is always found.
  - `mount_project` maps `data:/x` to `<data>/<game>/x`, and a write creates the folder.
  - `tests/test_project_file.cpp`: `[files] host_paths = true` is read, and a string value gives the exact error.
  - The release stub's mounts are proven in Task 5, which has the script helpers to read through them. Here, `test_release_stub` must stay green.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** the platform file layer, the path parser, the Vfs, `FolderSource`, `HostSource`, `mount_game_folders` and `mount_project`. Wire up `ScriptRuntime`, `zodiac run` (dev mode on), `zodiac build`'s compile (dev mode off) and the release stub.
- [ ] **Step 4: Verify.** Run `ctest -R "test_vfs|test_project_file"`, then the full suite once including `cli_e2e`, `build_e2e` and `kit_e2e`. Nothing outside the temp folders may be written. Push and confirm CI is green on both platforms.
- [ ] **Step 5: Commit** with `git commit -m "Add the virtual file system with case-insensitive folders and start-up mounts"`.

### Task 5: Script `file` object and helpers

**Files:**
- Create: `src/core/vfs/files.hpp`, `src/core/vfs/file_binding.cpp`
- Modify: `src/core/script_runtime.cpp` (`register_files(engine_,vfs_)` after time), `tests/script_fixture.hpp` (a `zodiac::vfs::Vfs vfs` member mounted on a temp folder, registered in the constructor body), `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/test_files.cpp`, `tests/test_release_stub.cpp`

**Interfaces:**
- Consumes: `vfs::Vfs`, `Reader`, `WriteTarget` and `FileError` from Task 4, and `DateTime` from Task 3.
- Produces `void zodiac::register_files(asIScriptEngine*,vfs::Vfs&);`. Global functions bind with `asCALL_THISCALL_ASGLOBAL` on the Vfs.
- Script API (the failure rule of core spec 8.7):
  - `file` is a reference type with a factory. It has:
    - `bool open(const string &in path, const string &in mode)`, with the modes `r w a r+` and a `b` suffix (`rb`, `wb`, `ab`, `r+b` or `rb+`).
    - Reading: `string read()`, `string read(int64 count)` (count bytes from the file, then text conversion), `string read_line()` (without the line end, `""` at end).
    - Writing: `bool write(const string &in)`; `bool sync()` flushes what was written so far to the device (FlushFileBuffers; fdatasync, fsync on macOS), true at once for a file opened `r`, and throws `file is not open` when closed.
    - Position: getters `int64 size`, `int64 position`, `bool eof`; `bool seek(int64)`.
    - State: `void close()`, getters `bool is_open` and `string error`.
  - Text mode reads `\r\n` as `\n` and skips a UTF-8 BOM at position 0. Writing never converts, so the bytes are identical on both platforms.
  - Reading or writing a closed file throws `file is not open`. Writing a file opened `r` throws `file is not open for writing`. Data that fails authentication throws `cannot read "<path>": damaged data`.
  - Helpers:
    - `file_exists`, `directory_exists`.
    - `string read_file(path)` throws `cannot read "<path>": <reason>`.
    - `write_file`, `append_file`, `delete_file`, `copy_file(from,to)` and `move_file(from,to)` all overwrite. A move between sources is a copy then a delete.
    - `bool write_file(const string &in path, const string &in data, bool sync = false)`, `bool copy_file(const string &in from, const string &in to, bool sync = false)` and `bool move_file(const string &in from, const string &in to, bool sync = false)`: the replaced file is the old one or the complete new one (a temporary file renamed over it). The default survives a crash of the game; `sync = true` flushes the new file to the device before the rename (and, on POSIX, its folder after it). A move's sync concerns only the copy it falls back to; a rename moves no data. Failures use `cannot write "<path>": <reason>`.
    - `int64 file_size(path)` returns -1 when missing. `date_time file_modified(path)` throws like `read_file` and gives local civil time.
    - `create_directory(path, bool recursive = true)`, `delete_directory(path, bool recursive = false)`.
    - `array<string>@ list_files(path, const string &in pattern = "*")` matches `*` and `?` case-insensitively by `fold_name`. `array<string>@ list_directories(path)`. Both return names, sorted by fold, then bytes.
  - **My call:** `string last_file_error()` gives the calling thread's last helper error (`thread_local`). The bool helpers set it on failure and clear it on success.

- [ ] **Step 1: Write the failing tests** (script level through `ScriptFixture`):
  - Every mode round-trips. `"a\r\nb"` read in text mode gives `"a\nb"` and in binary mode is unchanged. A BOM is skipped in text mode only.
  - `read_line` over `"x\ny"` gives `"x"`, then `"y"`, then `""`, and then `eof` is true.
  - `read_file("missing.txt")` throws `cannot read "missing.txt": file not found`.
  - `file_size("missing")==-1`. After a failed `write_file("con", "x")`, `last_file_error()=="\"con\" is not a valid file name"`.
  - `list_files("", "*.TXT")` returns `{"a.txt","B.txt"}` sorted.
  - `copy_file` and `move_file` across `/` and `data:/` work.
  - Misuse texts are exact.
  - `test_release_stub.cpp`, `start mounts the executable folder and data:/`: with `APPDATA`/`XDG_DATA_HOME` set to a temp folder, a payload script reads `a.txt` placed next to the fake executable. It writes `data:/s.txt`, and the test finds that file under `<temp>/<game>/s.txt`.
  - Review Focus 3, disk part: a test helper makes a sparse 5 GiB file (`ftruncate`; on Windows `FSCTL_SET_SPARSE` plus `SetEndOfFile`) and writes `"END"` at offset 5368709120. Then `size==5368709123`, and `seek(5368709120)` followed by `read(3)` gives `"END"`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement and register.**
- [ ] **Step 4: Verify.** Run `ctest -R "test_files|test_release_stub"`, then the full suite once.
- [ ] **Step 5: Commit** with `git commit -m "Add the script file object and file helpers"`.

### Task 6: Crypto (ChaCha20-Poly1305, KDF, hash)

**Files:**
- Create: `src/core/pack/crypto.hpp`, `src/core/pack/crypto.cpp`, `src/core/pack/chacha20_simd.cpp` (only when Step 4 keeps it), `tests/bench/bench_files.cpp` (with the `crypto_chunk` op; Task 13 adds the read ops), `tests/fixtures/crypto/rfc8439.txt` (the RFC vectors, transcribed)
- Modify: `cmake/Dependencies.cmake` (`FetchContent_Declare(monocypher URL https://monocypher.org/download/monocypher-4.0.2.tar.gz URL_HASH SHA256=<pinned from the verified download>)`; `monocypher.c` is compiled into both core targets with `-w`), `src/core/CMakeLists.txt`, `tests/bench/CMakeLists.txt`, `THIRD_PARTY_LICENSES` (Monocypher, BSD-2-Clause text, linked into zodiac.exe and every game), `tests/CMakeLists.txt`
- Test: `tests/test_crypto.cpp`

**Interfaces:**
- Produces (namespace `zodiac::crypto`):
  - Types: `using Key=std::array<std::uint8_t,32>;` `using Nonce=std::array<std::uint8_t,12>;` `using Tag=std::array<std::uint8_t,16>;`
  - `void seal(const Key&,const Nonce&,std::span<const std::byte> aad,std::span<const std::byte> plain,std::span<std::byte> cipher,Tag&) noexcept;`
  - `bool open(const Key&,const Nonce&,std::span<const std::byte> aad,std::span<const std::byte> cipher,const Tag&,std::span<std::byte> plain) noexcept;`: RFC 8439 2.8. It compares in constant time and writes nothing readable on failure (the output is zeroed).
  - `Key derive_password_key(std::string_view password,std::span<const std::uint8_t,16> salt,std::uint32_t kib,std::uint32_t passes);`: Argon2id with one lane. The defaults are `kDefaultKdfKib=8192` and `kDefaultKdfPasses=3`.
  - `Key derive_file_key(const Key& master,std::span<const std::uint8_t,16> salt) noexcept;`: keyed BLAKE2b-256 of `"zodiac pack key v1"` followed by the salt.
  - `std::array<std::uint8_t,32> hash(std::span<const std::byte>) noexcept;`: BLAKE2b-256.
  - `void random_bytes(std::span<std::byte>);`: `platform::fill_entropy`. It throws `std::runtime_error("the entropy source failed")`.
- **My call:** Monocypher 4.0.2 provides Poly1305, BLAKE2b, Argon2id and the reference ChaCha20 (`crypto_chacha20_ietf`). The AEAD is assembled as RFC 8439 specifies: the Poly1305 key is block 0 and the data starts at counter 1.

- [ ] **Step 1: Write the failing tests**:
  - Every RFC 8439 vector: 2.3.2 (block), 2.4.2 (encryption), 2.5.2 (Poly1305), 2.6.2 (Poly1305 key generation), 2.8.2 (AEAD), and A.1 to A.5.
  - Flipping each bit of cipher, tag or aad makes `open` false with the output zeroed.
  - SIMD against reference, with both compiled in. The SIMD keystream and `seal` output must equal the reference (Monocypher) output on every RFC vector, and on 64 random buffers with a fixed seed. The buffer lengths are 0 to 1024 and 65536, at counters 0, 1 and 0xFFFFFFFE. The test is compiled only while Step 4 keeps the SIMD file.
  - `derive_password_key` is deterministic for a fixed salt and differs for another salt.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement on Monocypher.**
- [ ] **Step 4: Measure the vectorized ChaCha20.**
  - Write an SSE2 4-block path and an AVX2 8-block path with `__attribute__((target(...)))`, dispatched once by `__builtin_cpu_supports`.
  - Measure with `bench_files --op crypto_chunk`: one 64 KiB `seal` plus `open`, against Monocypher's scalar keystream.
  - Keep the SIMD file only when it is at least 10 % faster on both CI hosts. Otherwise delete it. Either way, record the numbers in the commit message.
  - Add a report op, `kdf_open`: the time of `derive_password_key` with the defaults, printed by `bench_files` on release. It is not a unit test. Task 13 records it against the spec's "well under 100 ms", which here means at most 50 ms.
- [ ] **Step 5: Verify.** Run `ctest -R test_crypto` on debug and release, then the full suite once. `bench_files --op kdf_open` on release prints at most 50 ms. `test_angelscript_has_no_sample_addons` still passes. Push and confirm CI is green.
- [ ] **Step 6: Commit** with `git commit -m "Add ChaCha20-Poly1305, Argon2id and BLAKE2b for packs"`.

### Task 7: Pack format, storage and the script `pack` object

**Files:**
- Create: `src/core/pack/pack_format.hpp`, `src/core/pack/pack_format.cpp`, `src/core/pack/pack_storage.hpp`, `src/core/pack/pack_storage.cpp`, `src/core/pack/pack_binding.cpp`
- Modify: `src/core/vfs/files.hpp` (`register_pack`), `src/core/script_runtime.cpp` (`register_pack(engine_,vfs_)` after files), `tests/script_fixture.hpp`, `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/test_pack.cpp`

**Interfaces:**
- Consumes: Task 6 crypto, Task 4 `platform::File`, `replace_file`, `vfs::Vfs` (to resolve VFS paths to host files) and `parse_path` (for entry names).
- Binary layout (my call, spec 8.2; all integers little-endian):
  - **Header, 64 bytes:**
    - Magic `"ZDPACK\x1a\n"`, then `u32 version=1`.
    - `u32 flags`: bit 0 is compression, reserved. Any set bit gives `unsupported pack (newer format)`.
    - `u8 chunk_log2` (16, which is 64 KiB), `u8 kdf` (0 is a raw 256-bit key, 1 is Argon2id), `u16 0`.
    - `u32 kdf_kib`, `u32 kdf_passes`, a 16-byte random salt, then 20 zero bytes.
    - An opener refuses `kdf_kib>65536` (64 MiB), `kdf_passes>8` or `chunk_log2` outside 12..24 as damaged, so a crafted mod pack cannot stall the game.
  - **Chunks:**
    - Each entry is split into chunks of `2^chunk_log2` bytes. Each chunk is stored as ciphertext followed by its 16-byte tag.
    - The nonce is `u32 chunk_index ‖ u64 entry_id`.
    - **My call:** entry ids are random non-zero u64 values, unique within the index, not counters. Entries lost in a crash can then never reuse a nonce.
  - **Index:**
    - It is one AEAD record with a random 12-byte nonce, and the header is its AAD.
    - The plaintext is `u64 generation`, `u64 wasted_bytes`, `u32 meta_len`, the meta bytes (`key=value\n` lines; Tasks 10 and 11 use `build`, `base_build` and `patch`), and `u32 count`.
    - Then each entry: `u16 name_len`, the name, `u8 flags` (bit 0 is a deletion marker), `u64 id`, `u64 size`, `u64 offset`, `i64 modified_utc_ms`.
    - After decryption every field is validated: names pass `parse_path`, there are no fold duplicates, and offset plus chunk bytes stay inside the data region. A failure gives damaged.
  - **Footer, 40 bytes:** `u64 index_offset`, `u64 index_length`, the 12-byte index nonce, `u32 crc32` of the preceding 28 bytes, and magic `"ZDPKEND1"`.
  - **Recovery:** this happens only when the window's last 40 bytes are not a complete footer. That means the magic is missing, the CRC fails, or `index_offset+index_length` does not end exactly where the footer starts: a truncated file or an interrupted append.
    - The opener then scans backwards, inside the window, for the footer magic, and takes the first complete footer whose index authenticates.
    - A complete footer whose index fails authentication is never skipped: open fails with `wrong key or damaged pack`, as the spec requires for a modified pack.
    - The first write after a recovery truncates the file to the recovered footer.
- Produces `void zodiac::register_pack(asIScriptEngine*,vfs::Vfs&);`, declared in `src/core/vfs/files.hpp` next to `register_files`.
- Produces `class zodiac::pack::PackStorage` (thread-safe: a `std::shared_mutex`, readers share it and changes take it exclusively):
  - Creating and opening: `static std::expected<std::shared_ptr<PackStorage>,std::string> open(const std::filesystem::path& host,KeySource,bool create,const PackOptions& = {},std::optional<PackLocation> window = {})`.
    - `KeySource` is either a password `std::string` or a raw `crypto::Key`.
    - `PackOptions{std::uint32_t kdf_kib=kDefaultKdfKib; std::uint32_t kdf_passes=kDefaultKdfPasses; std::uint8_t chunk_log2=16;}`.
    - `struct PackLocation{std::uint64_t offset; std::uint64_t size;};` is defined in `pack_storage.hpp`. Task 10 uses it for packs embedded in an executable.
    - A window makes the pack read-only, and every offset is relative to `offset`. The header is at `offset`, the footer ends at `offset+size`, and recovery never reads outside the window. Without a window, the window is the whole file.
    - A new pack immediately gets its header, an empty index and a footer.
    - A process-wide registry keyed by the canonical host path plus the window offset returns the already open storage. The registry checks the derived file key in constant time and gives `wrong key or damaged pack` on a mismatch.
  - Changing entries:
    - `add(std::string_view name,std::span<const std::byte>,std::int64_t modified_utc_ms)` and `add_deletion_marker(name)`. Each appends chunks after the last footer; a replacement adds the old bytes to the wasted bytes.
    - `remove(name)`.
  - Lookup and reading:
    - `stat(name)` returns `vfs::EntryInfo`, `list()`, `count()`, `wasted_bytes()`.
    - `open_reader(name)` returns a `vfs::Reader`. It snapshots, at open, the storage's current `shared_ptr<platform::File>`, the entry record (id, size, offset), the chunk size and the file key, and caches one decrypted chunk. It never reads the storage's current state again, so `compact()` or a later replacement cannot change what it reads.
  - Saving:
    - `flush()` appends a new index (generation+1) and a new footer, then syncs. The superseded index and footer are added to `wasted_bytes`.
    - `compact()` writes `<file>.compact.tmp` in the same folder with the same salt, new entry ids and a new index. It then calls `replace_file` and swaps in the new `File`. Readers already open keep their snapshot of the old one.
  - `metadata()` and `set_metadata(std::string)`.
  - `close()` flushes and stops lookups. Destruction does the same, logging a failure. Only the last holder triggers it.
- Script API: `pack` is a reference type with a factory.
  - Opening and state:
    - `bool open(const string &in path, const string &in key, bool create = false)`. `create` creates the pack when it is missing; without it, a missing file fails with `file not found`.
    - `void close()`, getters `bool is_open` and `string error`.
    - `pack.close()` (and the object's destruction) flushes, drops only this object's `shared_ptr<PackStorage>`, and removes the mounts this object made (Task 8). The shared storage stays open for other `pack` objects, mounts and readers until its last reference goes, and that last holder calls `PackStorage::close()`.
  - Listing and lookup: `bool exists(name)`, `array<string>@ list()` (sorted by bytes, markers left out), getter `int count`, `int64 size(name)` (-1 when missing).
  - Writing:
    - `bool add_file(const string &in vfs_path, const string &in name)` and `bool add(const string &in name, const string &in data)`. An invalid name gives `"<name>" is not a valid entry name`.
    - `bool remove(name)`.
  - Reading:
    - `string read(name)` throws `cannot read "<name>" from the pack: file not found` or `...: damaged data`.
    - `bool extract(name, const string &in vfs_path)`.
  - Upkeep: `bool compact()`, `bool flush()`, getter `int64 wasted_bytes`.
  - **My call:** the pack file must resolve to a folder mount. One inside a pack gives `"<path>" is inside a pack; a pack file must be a plain file`. Task 10 keeps `*.zpk` assets plain, so dev and release agree.

- [ ] **Step 1: Write the failing tests** in `tests/test_pack.cpp`. The C++ tests use `PackOptions{kdf_kib=64,kdf_passes=1}` for speed. Script tests use the defaults, at most three opens per test.
  - Round trip of names with Turkish letters. Case-insensitive `exists("SES/A.OGG")`.
  - `list()` order. `size`, `count`, `extract` and `add_file` through VFS paths.
  - Review Focus 3, pack part (these tests pass `chunk_log2=16` explicitly, so a later default change keeps them at the chunk edges):
    - Entries of 0, 1, 65535, 65536, 65537 and 196615 bytes read back exactly.
    - A reader at 65535 for 3 bytes, a read at exactly 65536, and one spanning chunks 0 to 2 all give the right bytes.
    - An encoded index entry with `offset=0x1'0000'0010` and `size=0x2'0000'0000` round-trips through `pack_format`'s encoder and decoder without writing that much data.
  - Security (spec 9):
    - A wrong key fails at open with `wrong key or damaged pack`.
    - Flip each byte of a flushed two-entry pack, one at a time over the whole file. Either the open fails with `wrong key or damaged pack`, or reading an entry throws `damaged data`. It never returns other bytes, and never opens at an older generation: a complete but modified footer or index is refused, not recovered from.
    - The same plaintext in two packs with the same password gives different ciphertext.
    - Known plaintext: two entries each start with the same real 27-byte OGG page header (`"OggS"` plus 23 header bytes), followed by different data, over three chunks.
      - The keystream recovered from entry 1's first 27 bytes (ciphertext XOR header) is XORed with every other 27-byte window of entry 1's chunks and of entry 2's chunks.
      - None of those windows gives the plaintext.
      - Entry 2's first 27 ciphertext bytes differ from entry 1's.
    - Header fields `kdf_kib=65537`, `kdf_passes=9` and `chunk_log2=30` are refused as damaged without deriving a key. `kdf_kib=65536` with `kdf_passes=8` opens.
  - Window: a pack copied into a larger file at offset 4096, with 100 trailing bytes after it, opens with `PackLocation{4096,size}` and reads every entry.
    - The registry treats the same path at offsets 0 and 4096 as two storages.
    - The 100 trailing bytes end with a copy of a later pack's footer, which would recover to different entries. A window one byte short of the real end makes the last footer incomplete. The pack then opens at its own previous flush inside the window, and the copied footer outside the window is never used.
  - Deletion markers: `add_deletion_marker("b")` makes `stat("b").kind==EntryKind::deleted`. `list()` and the script `list()` and `exists` leave it out, and the marker survives flush and reopen.
  - Metadata: `set_metadata("build=ab\n")` survives flush and reopen exactly.
  - Shared registry: two `PackStorage::open` calls on one path return the same pointer; a wrong key on the second gives `wrong key or damaged pack`.
  - Reader snapshot: a reader opened before `compact()` reads correct bytes after it.
  - Writing:
    - Replace and remove raise `wasted_bytes`, and so does each `flush()`, by the superseded index and footer. `compact()` brings it back to 0, keeps every entry, and leaves no `.compact.tmp` behind.
    - Crash truncation (spec 8.3): add `a`, flush, add `b`.
      - Copy A is taken before `b`'s flush. Copy B is taken after `flush()` of `b`.
      - Copy A is truncated at the first footer's end, at +1, and in the middle of `b`'s chunk.
      - Copy B is truncated at the start of the new index and at −1 from its end.
      - Each opens with exactly `{a}`, and `a` reads correctly. A truncation inside the first footer fails as damaged.
  - Script: `pack p; p.open("m.zpk","k",true); p.add("x","1"); p.close();`, then a second `pack q` opens it and reads `"1"`. With `pack r` also open on the file, `q.close()` leaves `r.read("x")=="1"`. `read("missing")` throws the exact text.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** `pack_format`, then `PackStorage` and the registry, then the binding.
- [ ] **Step 4: Verify.** Run `ctest -R test_pack`, then the full suite once. Push and confirm CI is green.
- [ ] **Step 5: Commit** with `git commit -m "Add encrypted packs and the script pack object"`.

### Task 8: Mounting packs

**Files:**
- Create: `src/core/pack/pack_source.hpp`, `src/core/pack/pack_source.cpp`
- Modify: `src/core/vfs/vfs.hpp`/`.cpp` (newest-first resolution, deletion markers, write routing), `src/core/pack/pack_binding.cpp` (`mount_pack`, `unmount_pack`; `close()` removes this object's mounts), `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`
- Test: `tests/test_pack_mount.cpp`

**Interfaces:**
- Consumes: `PackStorage` (Task 7) and `Vfs::mount`/`unmount` (Task 4).
- Produces:
  - `std::shared_ptr<vfs::Source> make_pack_source(std::shared_ptr<pack::PackStorage>,bool writable);`
  - Script `bool mount_pack(pack@ p, const string &in at = "/", bool writable = false)`. A failure sets `p.error` to `pack is not open`, `already mounted there` or a path error.
  - Script `void unmount_pack(pack@ p)`.
- Resolution (spec 8.3):
  - Mounts at one point are searched newest first, and every mount is in front of that point's folder.
  - The first mount that has the name wins. A `deleted` marker hides the name in every older mount and in the folder at that point: `stat` gives none, reads give `file not found`, listings leave it out.
  - Listings merge all sources by fold, using the newest spelling.
- Writes:
  - When a writable pack mount covers the path, the newest writable pack at that point receives the write, new files included.
  - Otherwise, when a read-only pack provides the name, the write fails with `read_only_pack`, and the message is `"<path>" is in a read-only pack; save files in data:/ instead`.
  - Otherwise the write goes to the folder.
  - A pack write target buffers in memory and calls `PackStorage::add` on `commit()`, when the file object closes.
- Routing reads only the `writable` flag given to `Vfs::mount`. `make_pack_source`'s flag sets `Source::read_only()` to its opposite, and `mount_pack` passes the same value to both.
- Lifetimes:
  - The mount table, readers and file objects hold the `shared_ptr<PackStorage>`. Each mount's owner is the `pack` script object that made it.
  - `unmount_pack(p)` and `p.close()` remove `p`'s mounts at once, so lookups through them stop.
  - Other objects' mounts and open readers keep working. The storage is freed with its last reference.

- [ ] **Step 1: Write the failing tests**:
  - Newest first with two packs at `/` holding `a.txt`.
  - A pack mounted at `data:/maps` serves `data:/maps/x` and `data:/MAPS/X`, and `data:/maps/../x` resolves in the folder.
  - A write to a path only a read-only pack has fails with the exact text. A writable mount receives new and existing names, which persist after flush and reopen. Unrelated paths still land in the folder.
  - A marker in the newer pack hides the older pack's file and the folder's file. A third, newer pack holding the name shows it again.
  - Unmount while a read is open (spec 9): after `unmount_pack(p)` and `p.close()`, with no other holder, the file object keeps reading correct bytes. A new open fails with `file not found`, and a `weak_ptr` to the storage expires after the file closes.
  - Concurrent reads (spec 9): 8 threads each make 2 000 random reads (random offset, 1 to 200 000 bytes) of three entries through the mount and compare them with the source bytes. One extra thread opens and closes readers in a loop. It must finish within 30 s on debug.
  - Review Focus 4:
    - (a) While 4 threads read existing entries through a mount, the main thread adds 200 new entries to the same `PackStorage` and flushes every 50. Every read is correct, and every new entry is visible through the mount after its `add`.
    - (b) A second `pack` object opened on the same file shares the storage: an entry added by one is visible to the other's `exists`.
      - With `p` mounted at `/`, `q.close()` leaves `p`'s mount serving files.
      - `p.close()` removes the mount, so `file_exists` through it becomes false.
      - The file then opens cleanly in a fresh `pack`.
    - (c) `compact()` runs while a `file` object reads a mounted entry. The reader finishes with correct bytes, a new open reads from the compacted file, and on Windows the replace succeeds while the old handle is open.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Run `ctest -R "test_pack_mount|test_vfs|test_files"`, then the full suite once. Push and confirm CI is green.
- [ ] **Step 5: Commit** with `git commit -m "Mount packs into the VFS with newest-first lookup and write routing"`.

### Task 9: Pack and file settings in the project and payload

**Files:**
- Modify: `src/core/project_file.hpp`/`.cpp` (`[pack]`; `expand_assets` skips `<root>/.zodiac/`), `src/core/payload.hpp`/`.cpp` (wire format version 2), `src/cli/build_command.cpp` (`payload_of` sets `host_paths` from `[files]`; `pack_mode`, `pack_writable` and `pack_key` keep their defaults until Task 10), `src/release/stub.cpp` (passes `payload.host_paths` to `mount_game_folders`), `.gitignore` (`.zodiac/`, so a stray fixture build never commits a key)
- Test: `tests/test_project_file.cpp`, `tests/test_payload.cpp`, `tests/test_release_stub.cpp` (its payload helpers build v2 payloads)

**Interfaces:**
- Produces: `struct PackConfig{bool enabled=true; bool external=false; bool writable=false; std::vector<std::string> exclude;};` as `Project::pack`.
  - `exclude` follows `assets`' pattern rules and errors, with `[pack]` in the text.
  - `enabled`, `external` and `writable` must be booleans: `[pack] <key> must be a boolean`.
  - `writable=true` forces `external` to true silently, as spec 8.4 says.
  - Unknown keys: I checked `parse_project`, and the existing loader ignores unknown keys in every section; only unknown values of `[core] exclude` are refused. `[pack]` and `[files]` treat unknown keys the same way and ignore them, and a test pins that.
- Produces: `GamePayload` adds four fields with defaults, so payloads written before Task 10 stay valid.
  - `enum class PackMode:uint8_t{none=0,embedded=1,external=2} pack_mode=PackMode::none`.
  - `bool pack_writable=false`.
  - `bool host_paths=false`.
  - `std::string pack_key`: empty, or exactly 32 bytes.
- The wire format inserts, after `u32 dialog`: `u32 pack_mode | u32 pack_writable | u32 host_paths | str pack_key`. `format_version` becomes 2, and only 2 is accepted.

- [ ] **Step 1: Write the failing tests**:
  - Defaults are read. `exclude = ["docs/**"]` is read. `writable = true` makes `external` true.
  - Bad types and patterns give the exact errors. An unknown `[pack] writeable = true` is ignored, so `writable` stays false.
  - `expand_assets` with `assets=["**/*"]` never returns `.zodiac/pack.key`.
  - Payload v2 round-trips every new field. Version 1 is refused with `error: the game data is damaged (unsupported version 1)`.
  - A 31-byte key, and a `pack_mode` of 3, are refused as damaged.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Run `ctest -R "test_project_file|test_payload|test_release_stub"`, then the full suite once.
- [ ] **Step 5: Commit** with `git commit -m "Add [pack] and [files] settings and carry them in the payload"`.

### Task 10: Release packing in `zodiac build` and the release start-up mount

**Files:**
- Create: `src/core/pack/release_pack.hpp`, `src/core/pack/release_pack.cpp`, `src/cli/pack_key.hpp`, `src/cli/pack_key.cpp`, `src/cli/release_packing.hpp`, `src/cli/release_packing.cpp`, `src/cli/literal_paths.hpp`, `src/cli/literal_paths.cpp`, `tests/fixtures/scripts/blood_bath_style/` (`zodiac.toml` with `static = ["example"]` and `assets = ["data/**", "sounds/**"]`, `main.zs`, and `data/`, `sounds/` with Turkish file names)
- Modify:
  - `src/cli/build_command.cpp` and `build_command.hpp`:
    - In `write_release`: the asset split and pack writing, appending to `<name>.exe.tmp` before signing, and `<name>.zpk.tmp` renamed with the DLL copies.
    - `payload_of` now fills `pack_mode`, `pack_writable` and `pack_key`.
    - The literal-path warnings run after the compile.
    - Update the contract comment's steps 4 to 7.
  - `src/release/stub.cpp`: `mount_release` replaces Task 4's `mount_game_folders` call, still before `load_bytecode`.
  - `src/core/CMakeLists.txt` (`release_pack.*`).
  - `src/core/script_runtime.hpp`/`.cpp`: `script_files()`.
  - `src/core/vfs/startup.cpp` and `src/cli/main.cpp`: the dev release-pack check.
  - `src/cli/CMakeLists.txt`, `tests/CMakeLists.txt`.
  - `tests/e2e/test_build.py`:
    - `test_build_assets_copied` becomes `test_build_assets_packed` plus `test_build_pack_disabled_copies_assets`.
    - `test_identity_with_run` copies each fixture into the temp folder before running and building.
    - `BUILDABLE_FIXTURES` gains `blood_bath_style`.
- Test: `tests/test_release_pack.cpp`, `tests/test_literal_paths.cpp`, `tests/test_release_stub.cpp`, `tests/e2e/test_build.py`

**Interfaces:**
- Produces in `release_pack.hpp` (namespace `zodiac::pack`):
  - `std::uint64_t image_end(const platform::File&)`. For a PE file whose security data directory is non-empty and ends at the file's end, it returns that directory's file offset. Otherwise it returns the file size.
  - Trailer, 32 bytes: `u64 pack_offset | u64 pack_size | u32 payload_crc | u32 crc32(first 20 bytes) | "ZDTRAIL1"`. `std::string trailer_bytes(std::uint64_t offset,std::uint64_t size,std::uint32_t payload_crc)`.
  - `std::expected<PackLocation,std::string> find_embedded_pack(const platform::File&,std::uint32_t payload_crc)`. `PackLocation` is Task 7's type, and its result is passed as the `window` argument of `PackStorage::open`.
  - **My call:** the payload stays located by its linked symbols. The trailer locates the pack and carries the payload's CRC-32, so a pack appended to another build's executable is refused.
  - `crypto::Key release_pack_key(const GamePayload&)`. It is the one function the protection plan replaces.
  - `int mount_release(vfs::Vfs&,const GamePayload&,const std::filesystem::path& executable,Diagnostics&)`:
    - Calls Task 4's `mount_game_folders(vfs, executable folder, payload.game_name, payload.host_paths, false)`.
    - Opens the build pack (embedded through the trailer, or `<game>.zpk` next to the executable) and mounts it at `/`, writable when `pack_writable`. Task 11 adds the patch mounts after this step.
    - Returns 0, or 2 after `error: the game data is damaged (pack: wrong key or damaged pack)` or `error: the game data is damaged (pack "<game>.zpk" is missing)`.
- Produces in `pack_key.hpp`: `std::expected<crypto::Key,std::string> load_or_create_pack_key(const std::filesystem::path& root)`.
  - The file is `.zodiac/pack.key`: 64 lowercase hex digits and `\n`.
  - When the key is first made, `.zodiac/.gitignore` gets `pack.key` if missing, and stdout gets `created .zodiac/pack.key; back it up, later builds and patches need it` before the summary.
  - A damaged file gives `error: .zodiac/pack.key is damaged (expected 64 hex digits); restore it from your backup`.
- Produces in `release_packing.hpp`:
  - `struct AssetSplit{std::vector<fs::path> packed,plain;}`. `AssetSplit split_assets(const Project&,const std::vector<fs::path>& assets)`: `plain` is `[pack] exclude` matches, `*.zpk`, or everything when `enabled=false`.
  - `std::expected<void,std::string> write_release_pack(const Project&,const crypto::Key&,const AssetSplit&,const fs::path& pack_file,std::string& build_id)`.
    - Entry names are the project-relative paths, and each entry's modified time is its source file's, so `file_modified` matches dev.
    - It writes metadata `build=<32 hex>`.
    - It writes `.zodiac/release.manifest`: line 1 `zodiac-release-manifest 1`, `build <id>`, `next_patch <n>` (kept from an earlier manifest, else 1), then `<blake2b hex> <size> <path>` per packed asset, sorted.
  - When there are no packed assets: no pack, no key, `pack_mode=none`. The hello stays one plain executable.
- Produces in `literal_paths.hpp`: `struct LiteralPath{int line; std::string path;};` and `std::vector<LiteralPath> find_literal_paths(std::string_view source)`.
  - It covers read-style calls only. It matches a string literal that is:
    - the first argument of `read_file`, `file_exists`, `file_size`, `file_modified` or `list_files`;
    - the source (first argument) of `copy_file` and `move_file`;
    - the path of a `.open(` whose mode literal is a read mode (`r`, `rb`);
    - the path of a pack's `.open(path, key...)`, told apart by its key argument not being a mode literal;
    - the source path of `.add_file(` and of `mount_pack`'s pack.
  - Write targets are never checked: `write_file`, `append_file`, `.open(` in a write mode, and destinations.
  - `.extract(name, path)` is handled differently: `LiteralPath` gets `bool pack_entry=true` for its entry name, and the build checks that name only against the assets of the pack the script opened by literal path, when one can be paired in the same file. Otherwise it is skipped, and the destination is never checked.
  - Comments are skipped; escapes and `"""` heredoc strings are handled.
  - `ScriptRuntime::script_files()` returns the compiled files, main first, then in include order.
- Build warning (my call on "project file"):
  - `warning: <section>:<line>: "<path>" is not among the declared assets, so the release cannot open it`.
  - It fires when no declared asset matches by VFS rules (fold, `\` as `/`) and the path names no file the build itself ships (the executable, `<game>.zpk`, module DLLs).
  - It is skipped for `data:/`, host paths and computed paths, and it is never an error.
- Dev warning: `mount_project` with dev mode calls `set_release_pack_check(relative)`. The check is true when the path matches `assets`, does not match `exclude`, does not end in `.zpk`, and `enabled && !writable`. Each such path warns once on its first write: `warning: "<path>" is written in a folder here, but the release puts it in a read-only pack, where writing fails; save files in data:/ instead`.

- [ ] **Step 1: Write the failing tests**:
  - `test_release_pack.cpp`:
    - A trailer round-trips.
    - A wrong `payload_crc` is refused, and a short file is refused without reading outside it.
    - Review Focus 5: a generated minimal PE image, plus a pack, plus a trailer, plus a fake `WIN_CERTIFICATE` table (8-aligned) set in the security data directory. `find_embedded_pack` finds the pack, and `PackStorage::open` with that window reads every entry. The same image with a certificate directory that does not end at the file end uses the file end.
  - `test_release_stub.cpp`:
    - `start runs a game whose pack is embedded`: a fake executable file with 4096 bytes of filler, then the pack, then the trailer. The pack is opened through its `PackLocation` window at offset 4096 and reads correctly. Bytes after the window are covered by the PE certificate test above.
    - `start reads an external pack` and `start refuses a missing external pack` (exact text).
    - `start mounts the pack before global initializers run`: `string g=read_file("data/a.txt");`.
    - A writable release pack keeps a script's write across two starts.
  - `test_literal_paths.cpp`:
    - The read-style calls above, one per line, are found. `f.open("a.txt","r")` gives line 3.
    - Ignored: `write_file("log.txt", ...)`, `f.open("save.txt","w")`, `copy_file`'s destination, commented-out calls and computed paths.
    - `p.extract("maps/1.txt", "data:/1.txt")` yields only `maps/1.txt`, with `pack_entry=true`.
  - e2e (Windows):
    - `test_build_assets_packed`: the output holds only `game.exe`, and the game reads `sounds/a.ogg` with the bytes from the project. `.zodiac/pack.key` and `.zodiac/.gitignore` exist and the stdout line is exact. A second build reuses the key, so the file is unchanged.
    - `test_build_pack_external`: `game.exe` plus `game.zpk`. A missing `game.zpk` gives exit 2 with the exact text.
    - `test_build_pack_exclude_keeps_plain_files`.
    - `test_build_pack_disabled_copies_assets`: plan 3's layout.
    - `test_build_writable_pack_is_external`.
    - `test_build_signed_pack_still_found`: the sign command is a Python script that appends a valid empty-certificate table and sets the security directory, and the game runs.
    - `test_build_warns_for_undeclared_literal_path`.
    - `test_run_warns_writing_a_packed_path`.
    - `test_hello_has_no_pack`: no `.zodiac` folder, and the size gate is unchanged.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.** The order in `write_release` is:
  1. Compile and link `exe.tmp`.
  2. Embedded mode: append the pack and trailer to `exe.tmp`. External mode: write `<name>.zpk.tmp`.
  3. Sign.
  4. Rename the executable, the DLLs and the `.zpk`.
  5. Copy `plain`.

  Every temporary file goes on failure, as plan 3's step 6 rule says.
- [ ] **Step 4: Verify.** Run `ctest -R "test_release_pack|test_literal_paths|test_release_stub"`, then the full suite on debug including `build_e2e` and `kit_e2e`. Push and confirm CI is green.
- [ ] **Step 5: Commit** with `git commit -m "Pack release assets into the executable or an external pack"`.

### Task 11: Patch packs

**Files:**
- Modify: `src/cli/main.cpp` (`--patch`, build only: `error: --patch applies to build only; run zodiac help`; help line), `src/cli/build_command.hpp`/`.cpp` (`BuildOptions::patch`), `src/cli/release_packing.hpp`/`.cpp` (`write_patch`, the manifest's `deleted` lines), `src/core/pack/release_pack.hpp`/`.cpp` (`mount_patches`, called by `mount_release`)
- Test: `tests/test_release_pack.cpp`, `tests/test_release_stub.cpp`, `tests/e2e/test_build.py`

**Interfaces:**
- Produces `struct PatchResult{int number; std::size_t changed; std::size_t deleted; bool written;};` and `std::expected<PatchResult,std::string> write_patch(const Project&,const crypto::Key&,const fs::path& output)`:
  - **My call:** no compile and no link. A patch carries asset files only, and stdout says `patches carry asset files only; script changes need a full build`.
  - It reads `.zodiac/release.manifest`. Without one: `error: no release manifest in .zodiac; build a full release first`. It needs `[pack] enabled`, else `error: --patch needs [pack] enabled`.
  - A manifest path that is still declared but no longer packed (it moved to `[pack] exclude` or became plain) fails the patch with `error: "<path>" moved out of the pack; that needs a full release build`. A patch cannot turn a packed file plain.
  - It writes `<output>/patches/patch_<n>.zpk` with the following content:
    - Every packed asset whose hash or size differs from the manifest, or that is new.
    - A deletion marker for each manifest path that is no longer declared at all (it is not in `expand_assets`).
    - Every path in the manifest's `deleted` list, again: as an entry when it is declared and packed again (also with bytes identical to the release's), otherwise as a marker. An older patch's marker can then never hide a restored file. A re-emitted path counts as a change only when its state (marker or entry) differs from the state the manifest recorded for the newest patch.
    - Metadata `base_build=<manifest build id>` and `patch=<n>`.
  - On success it increments `next_patch` in the manifest and writes a `deleted <state> <path>` line (`marker` or `entry`) for every path that has ever been given a marker. Those lines are kept for the life of the release. A full release build writes a new manifest without them.
  - **My call:** patches are cumulative against the last full release, not against the previous patch.
  - When nothing differs, nothing is written, and stdout says `no files changed since the last release build; no patch written` (exit 0).
- Produces `void mount_patches(vfs::Vfs&,const crypto::Key&,std::string_view build_id,const fs::path& exe_dir,const fs::path& data_dir,Diagnostics&)`:
  - It collects every `patch_<n>.zpk` (decimal `n`, no leading zeros) from `<data_dir>/patches/` and `<exe_dir>/patches/`, ordered by `n` ascending, with `data:/` after the executable's folder at equal `n`.
  - Each is mounted read-only at `/`. Other names are ignored with a warning: `warning: ignoring "<path>": patch packs are named patch_<n>.zpk`.
  - A wrong key or damaged pack is logged as `error: cannot use patch "<path>": wrong key or damaged pack`, and the game continues.
  - **My call:** a `base_build` that differs from the build pack's `build` is logged as `warning: patch "<path>" belongs to another release of the game; it is not used`, so an old patch can never override a newer full release.

- [ ] **Step 1: Write the failing tests** (spec 9):
  - Patch order: `patch_1` and `patch_2` both hold `a.txt`, and the newest wins. `patch_10` mounts after `patch_9`.
  - A deletion: `patch_1` marks `b.txt`, so `file_exists("b.txt")` is false, `read_file` throws `cannot read "b.txt": file not found`, and `list_files` leaves it out. Then `patch_2` adds `b.txt` again and it is visible.
  - A patch with a foreign key is refused with the exact logged line, and the game reads the build pack's bytes and exits 0.
  - A patch for another build is refused with the exact warning.
  - C++ (`test_release_pack.cpp`): `write_patch` on a project where one asset changed, one is new and one was removed. Opening the patch with `PackStorage` lists exactly the changed and new names plus one marker, and the metadata holds `base_build` and `patch=1`. The manifest's `next_patch` becomes 2.
  - e2e:
    - `test_build_patch_contains_only_changes`: the patched game, with `patches/patch_1.zpk` next to it, prints the changed asset's new bytes and an unchanged asset's old bytes.
    - `test_build_patch_needs_manifest`.
    - `test_build_patch_nothing_changed`.
    - `test_build_patch_deletes_and_readds`:
      - A full release ships `b.txt`.
      - `b.txt` is removed from the project and `--patch` writes `patch_1` with a marker; the game cannot see `b.txt`.
      - The identical `b.txt` is restored and `--patch` writes `patch_2`, holding `b.txt` even though its bytes equal the release's.
      - With both patches next to the game, it reads `b.txt` with the release's bytes.
    - `test_build_patch_refuses_a_file_moved_to_exclude`: exit 2 with the exact text, and no patch is written.
    - `test_build_patch_usage_errors`: `zodiac run --patch` gives the build-only text, and a project with `[pack] enabled = false` gives `--patch needs [pack] enabled`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Run `ctest -R "test_release_pack|test_release_stub"`, then the full suite including `build_e2e`. Push and confirm CI is green.
- [ ] **Step 5: Commit** with `git commit -m "Add patch packs with deletion markers"`.

### Task 12: Plugin VFS entries

**Files:**
- Modify:
  - `include/zodiac/plugin.h`:
    - Codes: `ZODIAC_E_NOT_FOUND -6`, `ZODIAC_E_IO -7`, `ZODIAC_E_DAMAGED -8`, `ZODIAC_E_BAD_PATH -9`, `ZODIAC_E_UNAVAILABLE -10` (a host without a VFS).
    - Seek origins: `ZODIAC_SEEK_SET 0`, `ZODIAC_SEEK_CUR 1`, `ZODIAC_SEEK_END 2`.
    - `typedef struct zodiac_vfs_file zodiac_vfs_file;`.
    - The thread rules for these entries go in the header comment.
  - `src/core/host.hpp`/`.cpp` (`Host::set_vfs(vfs::Vfs*)`), `src/core/script_runtime.cpp` (`host_.set_vfs(&vfs_)`), `tests/test_plugin_header.cpp` and `tests/test_plugin_header_c.c` (offsets), `tests/CMakeLists.txt` (fixture module `vfs_reader`)
- Create: `tests/fixtures/modules/vfs_reader.cpp`, `tests/test_plugin_vfs.cpp`

**Interfaces:**
- Appended to `zodiac_host` after `log`, in this order (host function indexes 32 to 37; `sizeof(zodiac_host)==host_function(38)`):
  - `int (*vfs_open)(zodiac_host*,const char* path,zodiac_vfs_file** handle);`
  - `int64_t (*vfs_read)(zodiac_host*,zodiac_vfs_file*,void* buffer,size_t size);`: bytes read, 0 at end of file, or a negative code.
  - `int64_t (*vfs_seek)(zodiac_host*,zodiac_vfs_file*,int64_t offset,int origin);`: the new position or a negative code.
  - `int64_t (*vfs_size)(zodiac_host*,zodiac_vfs_file*);`
  - `void (*vfs_close)(zodiac_host*,zodiac_vfs_file*);`
  - `int (*vfs_exists)(zodiac_host*,const char* path);`: 1, 0, or a negative code.
- A handle wraps a `vfs::Reader` and its position.
- The mapping of `FileError` to codes is: `not_found→NOT_FOUND`, `damaged→DAMAGED`, `bad_path`, `ambiguous` and `is_directory→BAD_PATH`, everything else `IO`.
- Paths follow the script rules, and the calls see exactly what scripts see. Calls are allowed from any thread; one handle is used by one thread at a time.
- The fixture `vfs_reader` registers `string vfs_read_all(const string &in path)` and `int vfs_threads(const string &in path,int threads)`. Both check `struct_size` first. The second returns 0 when every thread read identical bytes through its own handle.

- [ ] **Step 1: Write the failing tests**:
  - The new offsets and size in both header tests.
  - `test_plugin_vfs.cpp`, through a real `ScriptRuntime` with the fixture module and a mounted pack (spec 9):
    - `vfs_read_all("Data/A.TXT")` returns the packed bytes.
    - A missing path gives `NOT_FOUND`, and `"../x"` gives `BAD_PATH`.
    - A damaged chunk gives `DAMAGED`, with no bytes of it delivered.
    - `vfs_threads(path, 8)` returns 0.
    - A fixture module built against an older `struct_size` (the `small_info`-style CMake switch) still loads and registers.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify.** Run `ctest -R "test_plugin"`, then the full suite once. Push and confirm CI is green.
- [ ] **Step 5: Commit** with `git commit -m "Let plugins read files through the VFS"`.

### Task 13: Identity, pack read benchmark and the size gate

**Files:**
- Modify: `tests/fixtures/scripts/blood_bath_style/main.zs` (complete), `tests/bench/bench_files.cpp` (read ops), `tools/check_bench.py` (`--limit`, default 1.05), `tools/test_check_bench.py`, `.github/workflows/ci.yml` (a `bench_files` step on both OSes, with one re-run before failing, as `bench_types` has), `tests/e2e/test_build.py`
- Test: `tests/e2e/test_build.py`, `tools/test_check_bench.py`

**Interfaces:**
- `blood_bath_style` (core spec 9) prints every result with `example_print`. It covers:
  - `split`, `replace`, `to_number`, `contains`, `left`.
  - `read_file` of a packed asset by its exact path; `file` with `read_line` over a map file with a Turkish name; `file_exists` and `create_directory` in `data:/`.
  - Writing, reading back and deleting a save in `data:/`.
  - A `timer` (only `elapsed>=0` is printed).
  - `random_generator` seeded 7.
  - `round(2.675,2)`.
  - `date_time(2026,11,19).to_string()`.
  - `dictionary` `serialize`.
- `bench_files`: a 32 MiB random file on disk, and the same bytes in a pack, both mounted, both read through `Vfs::open_read` and warmed first.
  - `pack_read_sequential` reads the whole file in 64 KiB reads.
  - `pack_read_random_seek` makes 2 000 reads of 4 KiB at seeded random offsets.
  - The output uses `bench_types`' JSON lines, where `new_ns` is the pack and `addon_ns` the disk.

- [ ] **Step 1: Write the failing tests**:
  - `test_check_bench.py`: `--limit 3.0` passes a ratio of 2.9 and fails 3.1. The default stays 1.05.
  - `test_blood_bath_style_identical_in_every_pack_mode`: build with the default embedded pack, with `[pack] external = true` and with `[pack] enabled = false`. Each run's stdout, stderr and exit code equal `zodiac run`'s exactly, each with its own empty `APPDATA`.
  - `test_cased_read_same_output`: a small project reads a packed `sounds/a.ogg` as `Sounds/A.OGG` and prints its bytes.
    - This one case compares stdout and exit code only, between `zodiac run` and the release in each pack mode. The reason is that `zodiac run`'s dev-mode case warning (Task 4) goes to stderr and a release never prints it.
    - The test also asserts that the dev stderr is exactly that one warning and the release stderr is empty.
    - Every other identity case stays exact.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
  - Complete `blood_bath_style/main.zs`, the read ops and `--limit`.
  - Add the `bench_files` CI step on both OSes as report-only (`continue-on-error: true`) with an artifact upload of its JSON and stderr.
  - Commit with `git commit -m "Report pack reads against disk in CI and prove dev and packed releases identical"`, push, and collect the artifacts.
- [ ] **Step 4: Measure and rule.** On `C:/zbuild/release`, make three `bench_files` runs on Windows, and take three from the Linux CI artifacts.
  - **Ruling to record:** the limit per op is the highest median ratio measured on either OS × 1.25, rounded up to 0.1.
  - Tighten the CI step:
    - Remove `continue-on-error`.
    - Add `--limit <ruling>` with a comment.
    - Re-run once before failing, as `bench_types` does.
  - Record the ruling in this plan's Outcome section with the raw numbers, and record `kdf_open` (Task 6) there too.
  - Also record the chunk size measurement: 16, 64 and 256 KiB on `pack_read_random_seek`. Change `PackOptions::chunk_log2`'s default only if another size is more than 10 % faster on both OSes.
- [ ] **Step 5: Verify.**
  - Run the full suite on `C:/zbuild/debug` and `C:/zbuild/release`.
  - `test_hello_size_gate` passes in Release (≤ 5242880 bytes); note the size.
  - `bench_types` is green.
  - Paired runs of `bench_types` against `main` at `c380dd9` give a geomean ≤ 1.00 and no op above 1.02 beyond A/A noise in every pass.
  - Push and confirm CI is green on Windows, Linux and the kit jobs.
- [ ] **Step 6: Commit** the second commit with `git commit -m "Gate pack reads against disk at the measured limit"`.

**Outcome:** (filled in by Task 13: the bench_files limit ruling with its numbers, the chunk size, the hello size, and whether the SIMD ChaCha20 was kept)
