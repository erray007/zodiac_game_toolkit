// zodiac::cli::load_or_create_pack_key: the game's release pack key in <root>/.zodiac/pack.key (spec 8.2 and 8.4 of the
// library-and-packs design), read by every zodiac build that packs assets.
// - The file is the key as 64 lowercase hex digits and "\n". Read back, the digits may be upper case and the line may end with
//   "\r\n" or nothing (an editor or a copy may change those); anything else is
//   `error: .zodiac/pack.key is damaged (expected 64 hex digits); restore it from your backup`, and the file is never replaced:
//   a new key would orphan every patch of the shipped game.
// - Every load that finds the key also removes .zodiac/pack.key.tmp (a crash's leftover: a second name of the key, or a key that
//   never got its name) and makes sure .zodiac/.gitignore has a "*" line, so git ignores everything in .zodiac/.
// - Only when the file does not exist is a key made (crypto::random_bytes):
//   1. .zodiac/ is created, and .zodiac/.gitignore gets its "*" line when it has none (the file is created when missing);
//   2. a leftover .zodiac/pack.key.tmp (a key no build used, since pack.key is missing) is removed, and the key is written to it,
//      created new with platform::FileAccess::create_secret (POSIX mode 0600; on Windows a protected DACL for the owner, SYSTEM
//      and Administrators only, nothing inherited) and flushed to the device;
//   3. it gets its name atomically and without ever replacing a key: a hard link .zodiac/pack.key (which shares the file and its
//      DACL), or on a file system without hard links platform::rename_no_replace; when another build made the key in between,
//      that key is read and used, and this one goes;
//   4. the temporary name goes and the folder is flushed (POSIX), and standard output gets
//      `created .zodiac/pack.key; back it up, later builds and patches need it` (before the build's summary line).
//   A failure removes the temporary file and gives `error: cannot write "<file>": <reason>` (".zodiac/pack.key" or
//   ".zodiac/.gitignore"; "the entropy source failed" when there are no random bytes).
// - Any other failure to read it: `error: cannot read ".zodiac/pack.key": <reason>`.
// - The key is never logged or printed. Every copy made here is wiped; the caller wipes the one returned (crypto::wipe).
// - expand_assets never returns anything under <root>/.zodiac (core/project_file.hpp), so the key is never an asset.
// Notes for the user documentation (Task 14):
// - Back up the whole .zodiac/ folder (pack.key and release.manifest) and keep pack.key secret. Every release build and every patch
//   of the game is sealed with the key: without it no patch the shipped game accepts can be made again, and without the manifest
//   no patch can be made against the shipped build. .zodiac/.gitignore keeps the folder out of git, so a repository is no backup.
// - Until the protection plan, the key also ships in clear inside every packed executable (pack/release_pack.hpp): the packs are
//   obfuscation, not protection. Anyone who has the game can extract the key and read or change its packs and patches; keeping
//   pack.key secret only keeps people who have not got the game out.
// Threads: main thread.
#pragma once
#include <core/pack/crypto.hpp>
#include <expected>
#include <filesystem>
#include <string>

namespace zodiac::cli {
std::expected<crypto::Key,std::string> load_or_create_pack_key(const std::filesystem::path& root);
}
