// zodiac::cli::find_literal_paths: the file paths a script names as string literals in calls that read files (spec 8.4 of the
// library-and-packs design), for zodiac build's warning that a release cannot open them; check_literal_paths gives the warnings.
//
// find_literal_paths(source) tokenizes AngelScript source: // and /* */ comments are skipped; "..." and '...' strings (which are
// strings, not characters, in Zodiac's engine) have their escapes decoded as the compiler does (\" \' \\ \n \r \t \0, \x with up
// to two hex digits, \u with four and \U with eight, as UTF-8); """...""" heredoc strings are taken as written, without their
// first line when it holds only spaces and tabs and without their last when it does; adjacent string literals are one literal
// (the compiler joins them). An argument is literal only when it is exactly such a literal; a string with an escape the compiler
// would refuse or drop, and anything computed ("a" + b, a variable, a call), is no literal and is skipped. The calls matched
// (an identifier followed by '('; "member" means directly after '.'):
// - not a member: read_file, file_exists, file_size, file_modified and list_files (the first argument); copy_file and move_file
//   (the first argument, the source; the destination is written, never checked);
// - member .open(path, mode) of a file: a receiver declared in the source as a file ("file f;", "file@ f", a parameter or a class
//   member, anywhere in that source), or any receiver not declared as a pack whose second argument is a file mode literal
//   (files.hpp). Checked only for the read modes "r" and "rb", never for a write mode or a computed mode;
// - member .open(path, key[, create]) of a pack: only a receiver declared as a pack ("pack p;", "pack@ p"), so an open(text,
//   text) of any other object is never taken for one; checked unless a third argument is there and is not the literal false (a
//   pack being created is written);
// - member .add_file(path, name) of a declared pack: the first argument;
// - member .extract(name, path) of a declared pack: the entry name, with pack_entry set, when the receiver's nearest earlier .open
//   in the same source was a pack's open with a literal path and a literal key (pack and key then name them); otherwise nothing.
// What remains unseen (an advisory scan): packs and files held in arrays, returned by functions or declared in another script
// file than the call (the type is per source), and pack opens with a computed key, are not checked.
// mount_pack takes a pack handle, so the pack it mounts is checked through that pack's literal .open. Write targets (write_file,
// append_file, an open in a write mode, the destinations) are never reported; with uses they go to uses->written, and mount_pack's
// folders to uses->mounted. line counts from 1 ("\n" ends a line), and is the line where the literal's first piece starts. The
// results are in source order.
//
// check_literal_paths(project, files, assets, shipped, diagnostics): for each compiled script (ScriptRuntime::script_files, read
// again from disk; a file that cannot be read is skipped) and each literal path in it, parsed by the VFS rules (vfs::parse_path
// under [files] host_paths): skipped when it is invalid, a data:/ path or a host path; otherwise compared by fold_name ('\' as
// '/') with every declared asset (project-relative), every folder of one (list_files("sounds") names a folder), and every file the
// build itself ships at the top of the output folder (shipped: the executable, <game>.zpk, the module DLLs). An entry name with
// pack_entry is compared only with the entries of its pack: the pack's path must name a declared asset, which is opened read only
// with the literal key (any failure skips the check). A path is also fine when any compiled script writes it as a literal (a save
// read back), or lies below a literal folder some script mounts a pack at; one mount_pack at "/" or at a computed folder, in any
// script, turns every warning off, since that pack may hold any name. The suppressions ignore order and reachability (a write
// after the read counts too): a missed warning costs nothing, a false one teaches developers to ignore them.
// No match: the warning, which is never an error:
// `warning: <section>:<line>: "<path>" is not among the declared assets, so the release cannot open it` (path through one_line).
// Threads: main thread.
#pragma once
#include <core/diagnostics.hpp>
#include <core/project_file.hpp>
#include <core/script_runtime.hpp>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zodiac::cli {
struct LiteralPath{
int line=0;
std::string path;
// path is an entry name of the pack opened from pack with key (.extract).
bool pack_entry=false;
std::string pack;
std::string key;
};
// What else the scan found, for the suppressions of check_literal_paths (appended to, so one LiteralUses gathers every script).
struct LiteralUses{
// The literal paths written: the first argument of write_file and append_file, the destination of copy_file, move_file and
// .extract, a file's open in a write mode, a pack's open that creates it.
std::vector<std::string> written;
// The literal folders of mount_pack(p, at).
std::vector<std::string> mounted;
// A mount_pack with no literal folder (the default "/", or a computed one).
bool mounted_at_root=false;
};
std::vector<LiteralPath> find_literal_paths(std::string_view source,LiteralUses* uses=nullptr);
// assets: absolute paths under project.root (expand_assets'). shipped: file names directly in the output folder.
void check_literal_paths(const Project& project,std::span<const ScriptRuntime::ScriptFile> files,std::span<const std::filesystem::path> assets,std::span<const std::string> shipped,Diagnostics& diagnostics);
}
