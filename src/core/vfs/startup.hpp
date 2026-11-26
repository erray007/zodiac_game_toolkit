// The start-up mounts every game gets, shared by zodiac run, zodiac build's compile and the release stub (and Task 10's mount_release).
//
// mount_game_folders(vfs, root, game_name, host_paths, dev_mode): sets host paths and dev mode, mounts root read-write at "/" and
// <user_data_folder()>/<game_name> read-write at "data:/". The data folder is created by the first write into data:/ (a file or a
// folder), never at start-up, so a game that saves nothing leaves nothing behind; until then data:/ reads as an empty folder.
// When there is no per-user data folder, or the game name cannot be a folder name (valid_name), data:/ is not mounted and the log
// gets one warning: `warning: data:/ is not available: <reason>`.
// mount_project(vfs, project, dev_mode) calls it with the project root, [files] host_paths and the game name. In dev mode (zodiac run)
// it also sets the Vfs's release pack check (vfs.hpp), unless [pack] is disabled or writable or there are no [build] assets: a
// path of the root mount is named when, everything compared by fold_name as VFS lookups compare names (glob_match over the folded
// pattern and path, so "Sounds/a.ogg" counts for "sounds/**"), one [build] assets pattern matches it, no [pack] exclude pattern
// does, it does not end in ".zpk", and it is none of the files expand_assets never ships (zodiac.toml, the project file,
// release/ and .zodiac/ in the root). zodiac build itself matches the disk's spelling (packed_in_release); the check is advisory. The release would put such a file into its read-only pack, where a
// write fails; the Vfs warns once per path, and only when the write lands in a folder source.
// The release stub mounts through pack::mount_release (core/pack/release_pack.hpp), which calls mount_game_folders first.
#pragma once
#include <core/vfs/vfs.hpp>
#include <filesystem>
#include <string_view>

namespace zodiac {
struct Project;
}

namespace zodiac::vfs {
void mount_game_folders(Vfs& vfs,const std::filesystem::path& root,std::string_view game_name,bool host_paths,bool dev_mode);
void mount_project(Vfs& vfs,const Project& project,bool dev_mode);
}
