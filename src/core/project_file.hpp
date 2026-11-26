// zodiac::load_project reads and validates <dir>/zodiac.toml (load_project_file any project file): the game name and main script, the modules a game uses, core parts to exclude, diagnostics output, build settings (assets, debug info, signing), file access ([files] host_paths), the release pack ([pack] enabled, external, writable, exclude), the executable's settings ([release] run_as_admin) and worker scripts.
// toml++ is only included by project_file.cpp, so it never leaks into other translation units. Every error is one plain-text line; the ones about a project file's content start with that file's name ("zodiac.toml").
// Unknown keys are ignored in every section (a misspelt [pack] writeable leaves writable false); only unknown [core] exclude values are refused.
#pragma once
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zodiac {
enum class OutputMode{automatic,always,never};
struct DiagnosticsConfig{
OutputMode console=OutputMode::automatic;
// Absolute (project root / configured file), or empty when no log file is configured.
std::filesystem::path file;
OutputMode dialog=OutputMode::automatic;
bool operator==(const DiagnosticsConfig&) const=default;
};
struct BuildConfig{
// Glob patterns relative to the project root with '/' separators (see glob_match), in file order. load_project refuses a
// pattern with a backslash, an absolute pattern (leading '/' or a drive letter) and one with a ".." segment, since none could ever match.
std::vector<std::string> assets;
bool strip_debug_info=false;
// Program and arguments run on the built executable; empty when signing is not configured. When not empty, at least one element contains {file}.
std::vector<std::string> sign_command;
};
struct FilesConfig{
// [files] host_paths: scripts may open absolute host paths (core/vfs/path.hpp). Off unless the project enables it.
bool host_paths=false;
};
// [pack] (spec 8.4 of the library-and-packs design): how a release build stores the declared assets.
struct PackConfig{
// false keeps plan 3's copying of every asset next to the executable.
bool enabled=true;
// The pack goes next to the executable as <game>.zpk instead of being appended to it.
bool external=false;
// The release mounts its pack writable. A running executable is locked by the system, so true forces external to true (silently).
bool writable=false;
// Assets kept as plain copied files: glob patterns under the same rules (and refusals) as BuildConfig::assets, in file order.
std::vector<std::string> exclude;
};
// [release]: settings of the built executable itself.
struct ReleaseConfig{
// zodiac build links the requireAdministrator manifest (kit lib/release/zodiac_manifest_admin.obj) in place of the asInvoker one.
bool run_as_admin=false;
};
struct Project{
// Absolute, normalized project directory.
std::filesystem::path root;
// The project file it was read from (absolute, normalized, inside root); empty for a lone script's project, which has none.
std::filesystem::path file;
// [game] name, or the name of the root folder when absent.
std::string name;
// Absolute, normalized, inside root, and an existing regular file.
std::filesystem::path main_script;
// Names matching [a-z0-9_]+, without duplicates, in file order. "core" is dropped.
std::vector<std::string> static_modules;
std::vector<std::string> dynamic_modules;
// Entries of [core] exclude: "window" or "keyboard", without duplicates.
std::vector<std::string> core_exclude;
DiagnosticsConfig diagnostics;
BuildConfig build;
FilesConfig files;
PackConfig pack;
ReleaseConfig release;
// [workers] scripts: absolute, normalized, inside root, existing .zs regular files, without duplicates, in file order.
std::vector<std::filesystem::path> workers;
};
// Reads <dir>/zodiac.toml. A missing file: `zodiac.toml: error: no zodiac.toml in "<dir>"`; an unreadable one: `zodiac.toml: error: cannot read "<file>"`.
std::expected<Project,std::string> load_project(const std::filesystem::path& dir);
// Reads the project file config_file, whatever its name or extension (zodiac run/build --config); root is its folder (absolute, normalized), and every
// relative path in it resolves against that folder. A missing, unreadable or non-regular file: `error: config file "<config_file as given>" does not exist`.
// Every other error is load_project's, starting with the file's own name instead of "zodiac.toml" ("ayar.txt:3:1: error: ...").
std::expected<Project,std::string> load_project_file(const std::filesystem::path& config_file);
// Makes script_file (resolved against the working directory) the project's main script. Refused, leaving project unchanged, when it is not inside
// project.root: `error: script "<absolute script>" is outside the project folder "<root>"`. The file itself is not checked: the caller verifies it is a regular file.
std::expected<void,std::string> set_main_script(Project& project,const std::filesystem::path& script_file);
// The text "zodiac new" writes as zodiac.toml, with [game] name and main and [modules] static and dynamic filled in (TOML basic strings: backslash,
// quote and control characters escaped, UTF-8 kept). main_relative uses '/' separators. load_project reads it back to the same name, main and lists.
std::string project_file_text(std::string_view name,std::string_view main_relative,std::span<const std::string> static_modules,std::span<const std::string> dynamic_modules);
// The project a script file belongs to: the nearest folder holding a regular file zodiac.toml, starting at the file's own folder and walking up to
// the filesystem root. The result is absolute and normalized (no trailing separator); nullopt when no folder qualifies. Only checks for the file, never reads it.
std::optional<std::filesystem::path> find_project_root(const std::filesystem::path& script_file);
// The project for running one script file. With a project root (find_project_root): load_project(root) with main_script replaced by the given file (absolute, normalized, inside root by construction).
// Without one: root is the file's folder, main_script the file, name the file's stem, file empty, no modules at all (only the core: installed modules
// may register the same names, so none is chosen for the developer), everything else default.
// A project file error is returned unchanged. The file itself is not checked: the caller verifies it is a regular file.
std::expected<Project,std::string> load_project_for_script(const std::filesystem::path& script_file);
// Every regular file under project.root matching any of project.build.assets, as absolute paths, sorted and without duplicates.
// zodiac.toml in the root, the project file (project.file) and everything under <root>/release and <root>/.zodiac (the build's own state, the pack key among it) are never returned; those paths are compared as
// the file system compares names (same_path_text: case-insensitively on Windows). A pattern matching nothing is not an error; a missing or unreadable directory is: the error is one line, `zodiac.toml: error: cannot read directory "<path>": <reason>`.
// A link whose target no longer exists is skipped (there is nothing to copy); any other entry whose type cannot be read is an error: `zodiac.toml: error: cannot read "<entry path>": <reason>`.
std::expected<std::vector<std::filesystem::path>,std::string> expand_assets(const Project& project);
// True when a release build puts the asset relative (project-relative, '/' separators, as expand_assets' patterns see it) into its
// pack: [pack] enabled, no [pack] exclude pattern matches it, and it is no ".zpk" file (ASCII case-insensitively; a pack shipped as
// an asset stays a plain file, so a script opens it with its own key). Shared by zodiac build's asset split and zodiac run's
// release pack check, so both agree.
bool packed_in_release(const PackConfig& pack,std::string_view relative) noexcept;
}
