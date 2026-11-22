// Script paths of the virtual file system (core/vfs/vfs.hpp): parsing into a mount and a normalized relative path, the name rules,
// and the case fold every case-insensitive lookup compares by.
//
// parse_path(path, host_paths):
// - '\' counts as '/', repeated separators collapse ("a//b" is "a/b"), "." segments are dropped and ".." removes the segment before
//   it; a ".." above the mount's root fails with `"<path>" leaves its mount`.
// - A leading "data:" (ASCII case-insensitive) names the per-user data mount, mount "data:"; anything else is relative to the root
//   mount, mount "", and a leading '/' only marks that root, on every platform.
// - A host path is a drive letter and ':' ("C:/x", "c:\x"), a leading "\\" or "//" ("//server/share/x"), or a "host:" prefix
//   (ASCII case-insensitive; "host:/etc/x" is how Linux paths are written). Without host_paths it fails with
//   `"<path>" is a host path; set [files] host_paths = true to use it`. With it the result is mount "host:", host the absolute
//   path (a relative or rooted one resolved against the working directory, the game folder), lexically normal and without a trailing
//   separator, and relative host's generic UTF-8 text.
// - Every named segment (host paths: every one below the root) must pass valid_name, else `"<path>" is not a valid file name`. A host
//   path that is not well-formed UTF-8 or whose root is a Win32 device namespace ("\\.\", "\\?\", "//./", "//?/", also after "host:")
//   gets the same text.
// - folder is true when the text names a folder by its form: nothing after the mount ("", "/", "data:/"), a trailing separator
//   ("sounds/"), or a last segment "." or "..". relative is then the folder's own path ("" for a mount's root). File operations
//   on such a path fail with is_directory; folder operations and mount points accept it.
// Each <path> in a message is the path as given, through one_line. Path-rule failures are never wrapped in another message.
#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zodiac::vfs {
struct VfsPath{
// "" for the root mount, "data:" for the per-user data mount, "host:" for an accepted host path.
std::string mount;
// '/'-joined segments without a leading or trailing '/', spelled as given; empty for the mount's folder itself.
std::string relative;
// Only for "host:": the absolute host path, lexically normal.
std::filesystem::path host;
bool folder=false;
};
std::expected<VfsPath,std::string> parse_path(std::string_view path,bool host_paths);
// True for a name one segment may have: not empty, not "." or "..", well-formed UTF-8, no control character (below 0x20, or 0x7F), none of <>:"|?*/\,
// not ending in '.' or ' ', and a stem (the text before the first '.', trailing spaces ignored) that is no Windows device name,
// compared ASCII case-insensitively: CON PRN AUX NUL CONIN$ CONOUT$ CLOCK$, COM0-COM9, LPT0-LPT9 and COM¹ COM² COM³ LPT¹ LPT² LPT³.
// The same on every platform, so a name from player input never opens a device on Windows and never behaves differently on Linux.
// Names beginning with temp_prefix (ASCII case-insensitively) are reserved for the Vfs's temporary files, so no script, listing or
// lookup ever sees one.
bool valid_name(std::string_view name) noexcept;
// The start of the name of every temporary file the Vfs writes next to a file it replaces (write_file, copies).
inline constexpr std::string_view temp_prefix=".zodiac-tmp-";
// True when name begins with temp_prefix, ASCII case-insensitively (hidden from scripts).
bool is_temp_name(std::string_view name) noexcept;
// The name the Vfs gives a temporary file: temp_prefix, 16 lowercase hexadecimal digits (the creation time in seconds since 1970
// UTC, as 8 digits, then 8 random ones) and ".tmp". Only names of exactly this form are ever removed as a crash's leftovers.
std::string make_temp_name(std::int64_t unix_seconds,std::uint32_t random);
// The creation time (seconds since 1970 UTC) of a name of make_temp_name's exact form; nullopt for any other name.
std::optional<std::int64_t> temp_name_time(std::string_view name) noexcept;
// The temporary files this process is writing, by name (a name is unique: its time and random digits): the Vfs holds each from
// before it is made until it is renamed or removed, and a scan never removes a held one. Thread-safe.
void hold_temp(const std::string& name);
void release_temp(const std::string& name) noexcept;
bool temp_held(std::string_view name);
// The case fold of UTF-8 text: unicode::to_upper (Unicode simple uppercase, as NTFS's upcase table) per code point. Bytes that are
// not well-formed UTF-8 are kept as they are, and nothing is normalized (NFC and NFD spellings stay different).
std::string fold_name(std::string_view utf8);
// Appends fold_name(utf8) to out.
void append_fold(std::string& out,std::string_view utf8);
}
