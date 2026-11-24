# Zodiac Core Types and Library — Design

Status: approved by the owner (2026-11-16). First half of plan 4 (core platform) in the series of `docs/superpowers/specs/2026-11-14-bgdk-design.md`, following the naming rules of `docs/superpowers/specs/2026-11-16-zodiac-script-api-design.md` (8.1, style C). The second half of plan 4 (frame loop, `wait()`, coroutines, workers, threads, `verbose`, crash handler, diagnostics change) is designed there and is not part of this document.

## 1. Intent

Give game scripts the everyday tools a BGT or NVGT developer reaches for first — text, lists, key/value maps, maths, random numbers, timers, dates and files — with Zodiac's own types instead of AngelScript's sample add-ons, so that text behaves correctly for Turkish and every other language, common operations are as fast as possible, and the later `bgt_compat` module can bind BGT's names (`string_split`, `string_to_number`, `file_exists`, ...) directly to the same native functions.

## 2. Goals

- **T1.** A `string` type whose `length()` and indices count characters, with byte access next to it.
- **T2.** A hash-based `dictionary` that is faster than the sample add-on's tree.
- **T3.** An `array<T>` that keeps the proven garbage-collector integration of the sample add-on and adds native fast paths for primitive element types.
- **T4.** A maths, random, time and file library that covers everything Blood Bath uses (survey: `string_split` 74×, `string_replace` 68×, `string_to_number` 65×, `string_contains` 42×, `string_left` 19×, file open/read/write, `file_exists`, `directory_create`, `file_copy`, `timer`, `random`, `round`, `get_date`/`get_time`, `dictionary` serialize).
- **T5.** Measured, enforced speed: a benchmark suite compares the new types with the sample add-ons and fails when ours are slower.

## 3. Non-goals

- BGT names (`string_split` etc.) and BGT's file and serialization formats: the `bgt_compat` module (plan 9) binds those to these functions.
- Binary buffers, packet builders and network byte order: the network plan.
- Script and asset packs, encryption: plan 7 (the virtual file system here is built so packs can be mounted later).
- Locale-aware sorting and collation beyond the case conversion in 8.1.

## 4. Anti-goals

- A `string` that silently splits a character: no index, `substr` or `length` may ever cut a UTF-8 sequence in half.
- Types that are slower than the sample add-ons they replace for any measured operation.
- A crash for bad script input: out-of-range indices, invalid UTF-8, missing files and bad conversions are catchable script exceptions or documented return values, never undefined behaviour.

## 5. Constraints

- C++23, clang only; style without indentation; one-line diagnostics; the plugin ABI v1 is unchanged (host entries such as `arg_string`, `ret_string`, `ret_array`, `array_*` keep their contracts and are re-implemented on the new types).
- Dev and release identical (plan 3 identity rule).
- Licensing: the array is derived from AngelScript's `scriptarray` add-on (zlib); its notice is kept in the source file and in `THIRD_PARTY_LICENSES`.
- Thread rules of the Script API spec: these types are not internally synchronized; workers receive copies (deep copy must be supported by all three types).

## 6. Approaches considered

**String semantics**

| Option | Summary |
|---|---|
| A. Bytes (BGT) | Fast; `"şehir".length()` is 6; indices can split letters. |
| B. Characters | `"şehir".length()` is 5; correct for menus and text input. |
| **C. Characters plus byte access (chosen)** | B, plus `byte_length()` and byte-level access for files and network data. |

**How to build the types**

| Option | Summary |
|---|---|
| 1. Methods on the sample `std::string` | Least work; every character operation rescans the text. |
| **2. Own types (chosen)** | Own `string` and hash `dictionary` from scratch; own `array<T>` forked from `scriptarray` with native fast paths. |
| 3. Two string types | Flexible but confusing. |

The owner chose 2 and asked for as many own types as possible; after discussing speed, the array is a fork of the proven add-on rather than a rewrite.

## 7. Decisions made with the owner

- String semantics C.
- Own `string` and `dictionary` written from scratch; `array<T>` forked from `scriptarray` and optimized.
- A benchmark suite enforces that the new types are not slower.
- Plan 4's first half is split into 4a (types) and 4b (library).

## 8. Design

### 8.1 `string` (plan 4a)

Representation (builder's details within these rules):
- UTF-8 bytes, always NUL-terminated so the plugin ABI can hand out `const char*` without copying.
- Small-string optimization: short strings (at least 22 bytes) live inline with no allocation.
- Cached metadata: byte length, character count, and an "all ASCII" flag. For ASCII strings every character operation is a byte operation. For non-ASCII strings a sparse index (character → byte offset every N characters, built lazily on the first indexed access and dropped on mutation) makes `s[i]` and `substr` close to O(1).
- Invalid UTF-8 (bytes read from a file or the network) is kept as is; each invalid byte counts as one character, so `length()` and indices stay consistent and no data is lost.

Script API (style C; indices and counts are characters unless the name says `byte`):

| Group | Members |
|---|---|
| Size | `length()`, `byte_length()`, `empty()` |
| Access | `s[i]` (one-character string, read-only), `const uint &code_at(int) const` (code point), `const uint8 &byte_at(int) const` (byte) |
| Search | `find(needle, start=0)`, `rfind(needle, start=-1)`, `contains(needle)`, `starts_with`, `ends_with`, `count(needle)` |
| Parts | `substr(start, count=-1)`, `left(count)`, `right(count)`, `split(delimiter, keep_empty=true)` → `array<string>`, `array<string>.join(separator)` |
| Change (returns new string) | `replace(old, new, all=true)`, `trim()`, `trim_left()`, `trim_right()`, `lower()`, `upper()`, `lower(locale)`, `upper(locale)`, `repeat(n)`, `reverse()` (by character), `insert(index, text)`, `erase(start, count)` |
| Numbers | `to_number()` (double; throws on invalid), `to_int()`, `is_number()`, `is_digits()`, `string(value)` constructors for every numeric type and bool, `format(fmt, ...)` with `{}` placeholders |
| Other | `hash()` (64-bit, stable across runs and platforms), comparison operators (byte order = code point order), `+` and `+=` with strings, numbers and bools (BGT-style concatenation) |

`code_at` and `byte_at` return const references so the engine calls them through its fast call path (asBC_Thiscall1, as the add-on's `opIndex`); scripts read them as values, and a funcdef or delegate bound to them must use the reference signature.

- **Character = Unicode code point (my call).** Grapheme clusters (letter plus combining marks, emoji sequences) are not merged: they need UAX #29 tables and would slow every operation. Text typed on Windows and macOS keyboards is precomposed, so Turkish letters are single code points. `s.normalize()` (NFC) is provided for text from other sources. The anti-goal "never split a character" means never split a UTF-8 sequence.
- **Integers and ranges (my call, BGT-friendly):** lengths, indices and counts are `int`; "not found" is `-1`. `s[i]`, `code_at`, `byte_at` and `insert`/`erase` with a start outside `[0, length()]` throw `index out of range`. Counts that run past the end are clamped (`left(100)` on a 5-character string returns all 5; `substr(3, 100)` returns the rest). A negative count other than the `-1` default throws. The array uses the same rules.
- `lower()`/`upper()` use Unicode's locale-independent simple case mapping. `lower("tr")`/`upper("tr")` (also `"az"`) apply the Turkish dotted/dotless I rules (`I`→`ı`, `İ`→`i`, `i`→`İ`, `ı`→`I`).
- Errors: an index outside `[0, length())` throws `index out of range`; `to_number()` on a non-number throws `"<text>" is not a number` (use `is_number()` to check first).

### 8.2 `array<T>` (plan 4a)

- Forked from AngelScript 2.39.0's `scriptarray` into `src/core/types/array.*`, keeping its type-info, garbage-collector and copy handling; the zlib notice stays at the top of the file.
- Style-C names: `length()`, `empty()`, `resize(n)`, `reserve(n)`, `clear()`, `insert_at(i, v)`, `insert_last(v)`, `remove_at(i)`, `remove_last()`, `remove_range(start, count)`, `find(v, start=0)`, `find_by_ref(handle)`, `contains(v)`, `sort()`, `sort(compare_function)`, `sort_descending()`, `reverse()`, `slice(start, count=-1)`; initializer lists `{1, 2, 3}`; index out of range throws.
- Fast paths: for `bool`, all integer and floating-point element types and `string`, `sort`, `find`, `contains` and equality run natively in C++ without calling back into the script engine.

### 8.3 `dictionary` (plan 4a)

- Hash table with string keys (open addressing, the string's cached keyed table hash), iteration in insertion order (deterministic saves and listings).
- Values of any script type, stored like the sample add-on (primitive kept inline, objects and handles by reference counting), with garbage-collector support for cycles.
- API: `set(key, value)`, `get(key, out value)` → bool, `d[key]` (value proxy with conversions), `exists(key)`, `remove(key)` → bool, `keys()` → `array<string>`, `length()`, `empty()`, `clear()`, `serialize()` → string and `deserialize(string)` → bool in Zodiac's own versioned text format (only primitives, strings and nested dictionaries/arrays of those; other values are refused with an exception naming the key).

### 8.4 Benchmarks (plan 4a)

- `tests/bench/` target (not part of the normal test run) measuring, for the new types and the sample add-ons side by side: construction, concatenation, `length`, indexed access over ASCII and Turkish text, `find`, `split`, `replace`, array `insert_last`/`find`/`sort` for `int` and `string`, dictionary insert/lookup/iterate at 10, 1 000 and 100 000 keys.
- CI runs it on the release preset and fails when a new-type operation is more than 5 % slower than its add-on counterpart. **Noise handling (my call, redesigned with the owner's approval on 2026-11-24, Task 5b fix round 4):** shared CI runners are noisy, so each operation runs new and add-on versions interleaved in the same process for at least 15 rounds and compares the medians of the ratio. Each side is calibrated to an equally long timed window per round (`bench_types --calibrate time`, the gate's mode, window `--window-ms`, 1 ms; `--calibrate count` gives both sides equal reps instead, for diagnosis only), because a side whose window is a hundred times shorter is measured mostly over its fixed costs. The Windows job runs 3 bench processes and gates on the median of their ratios per operation, with no retry: one process's code and heap placement alone moves a ratio by several percent. It also compares Zodiac with main's Zodiac on the same runner (main's bench in alternating processes, cached per main commit): no operation may be more than 10 % slower than on main and their geometric mean no more than 1 %, limits set from same-source rebuilds (A/A' and no-op relinks), which move single operations by up to 9 % while their geometric mean stays within 1 %. Reason: the first design re-measured a failing operation once, which let placement luck decide (Task 5b's study of 60 runs) and could not catch a slowdown of our own that the add-on happened to share. Metadata (character count, ASCII flag, hash) may be computed lazily to keep construction and `+=` at add-on speed. If an operation provably cannot meet the gate, that is recorded as a ruling and reported to the owner, not silently waived. Operations that do not exist in the add-on (character indexing on Turkish text) are compared with the equivalent add-on code a script would have to write.
- The sample add-ons remain available only to the benchmark target; games never get them.

### 8.5 Maths and random (plan 4b)

- `abs`, `min`, `max`, `clamp`, `floor`, `ceil`, `round(x)`, `round(x, digits)`, `trunc`, `sqrt`, `pow`, `exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `hypot`, `fmod`, constants `PI`, `TAU`, `E`. Rounding is half away from zero (BGT's behaviour).
- `random(min, max)` (int, inclusive), `random_float()` (0 ≤ x < 1), `random_float(min, max)`, `random_bool()`, `random_choice(array)`; a `random_generator` value type (`seed(n)`, same methods) for reproducible sequences. Generator: xoshiro256**, seeded from the operating system's entropy source at start-up.

### 8.6 Time (plan 4b)

- `ticks()` → int64 milliseconds from a monotonic clock; `ticks_us()` microseconds.
- `timer`: `elapsed` (int64 ms), `elapsed_seconds` (double), `restart()`, `pause()`, `resume()`, `running`, `set(ms)`.
- `date_time` value: `date_time::now()`, `date_time::utc_now()`, `year`, `month`, `day`, `hour`, `minute`, `second`, `millisecond`, `weekday` (0 = Monday), `day_of_year`, `to_string(format="{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}")`, arithmetic with milliseconds, comparison.

### 8.7 Files through the virtual file system (plan 4b)

- A virtual file system in core with mounts. At start-up: the project root (dev) / the executable's folder (release) is mounted read-write at `/`; the per-user data folder (`%APPDATA%\<game name>` on Windows, `$XDG_DATA_HOME/<game name>` on Linux) is mounted at `data:/` for saves. Plan 7 adds read-only pack mounts in front of `/`.
- **Case-insensitive paths on every platform (my call):** BGT games were written on Windows and the Blood Bath server must run on Linux, so `Sounds/Shot.ogg` finds `sounds/shot.ogg` everywhere. Lookup tries the exact name first; on a miss it scans the folder once (cached per folder, invalidated by Zodiac's own writes) for a case-insensitive match. In dev mode a match that differs only in case logs a warning with the real name. Two names that differ only in case in one folder are an error when looked up case-insensitively.
- Paths in scripts use `/`; relative paths are relative to `/`; `..` may not leave a mount; absolute host paths are refused unless the project enables `[files] host_paths = true`.
- `file` object: `open(path, mode)` → bool (`"r"`, `"w"`, `"a"`, `"r+"`, binary with `"b"`; text mode is UTF-8 with `\n` line ends and an optional BOM skipped on read), `read()` (rest), `read(count)` (bytes), `read_line()`, `write(text)`, `size`, `position`, `seek(position)`, `eof`, `close()`, `is_open`, `error` (last error text).
- Helpers: `file_exists`, `directory_exists`, `read_file(path)` → string (throws on failure), `write_file(path, text)` → bool, `append_file`, `delete_file`, `copy_file`, `move_file`, `file_size`, `file_modified` (date_time), `create_directory(path, recursive=true)`, `delete_directory(path, recursive=false)`, `list_files(path, pattern="*")`, `list_directories(path)`.
- Failure rule: operations whose failure is normal (open, exists, write_file, delete) return bool and set the error text; reading a whole file that must exist (`read_file`) and misuse (reading a closed file) throw.

### 8.8 Existing add-on helpers (my call)

The exception helper `getExceptionInfo()` from the sample `scripthelper` add-on stays registered (existing scripts and fixtures keep working) and gets the style-C name `exception_info()` bound to the same function. New helpers only get style-C names.

## 9. Success criteria

- `"şehir".length()==5`, `"şehir".byte_length()==6`, `"şehir"[1]=="e"`, `"İSTANBUL".lower("tr")=="istanbul"`, and no operation ever returns half a character (property tests over random strings mixing ASCII, Turkish, CJK, emoji and invalid bytes).
- Every benchmark operation of the new types is at most 5 % slower than its add-on counterpart (target: faster), enforced in CI.
- The plugin ABI tests and all existing script fixtures pass unchanged on the new types.
- A Blood Bath–style script using the string, file, timer and random operations listed in T4 runs in dev and release with identical output.

## 10. Left to the builder

- Exact memory layout of `string` (inline capacity, index granularity N), the hash function (any fast, stable 64-bit hash), dictionary probing scheme and load factor.
- The serialization text format details (versioned header required).
- Unicode case mapping tables (generated from the Unicode Character Database at a pinned version, committed).
- Benchmark harness and its repetition count.
- Order of tasks inside 4a and 4b.
