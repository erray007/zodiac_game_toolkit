// zodiac::cli::load_or_create_pack_key: the game's release pack key in <root>/.zodiac/pack.key (spec 8.2 and 8.4 of the
// library-and-packs design), read by every zodiac build that packs assets.
// - The file is the key as 64 lowercase hex digits and "\n". Read back, the digits may be upper case and the line may end with
//   "\r\n" or nothing (an editor or a copy may change those); anything else is
//   `error: .zodiac/pack.key is damaged (expected 64 hex digits); restore it from your backup`, and the file is never replaced:
//   a new key would orphan every patch of the shipped game.
// - Only when the file does not exist is a key made (crypto::random_bytes):
//   1. .zodiac/ is created, and .zodiac/.gitignore gets a "pack.key" line when it has none (the file is created when missing);
//   2. the key is written to .zodiac/pack.key.tmp (created new, POSIX mode 0600; on Windows it takes the project folder's
//      inherited ACL, which no portable call can narrow) and flushed to the device;
//   3. it gets its name atomically: a hard link .zodiac/pack.key, which fails rather than replace a key another build made in
//      between (that key is then read and used); on a file system without hard links, a rename after checking the name is free;
//   4. the temporary name goes and the folder is flushed (POSIX), and standard output gets
//      `created .zodiac/pack.key; back it up, later builds and patches need it` (before the build's summary line).
//   A failure removes the temporary file and gives `error: cannot write "<file>": <reason>` (".zodiac/pack.key" or
//   ".zodiac/.gitignore"; "the entropy source failed" when there are no random bytes).
// - Any other failure to read it: `error: cannot read ".zodiac/pack.key": <reason>`.
// - The key is never logged or printed. Every copy made here is wiped; the caller wipes the one returned (crypto::wipe).
// - expand_assets never returns anything under <root>/.zodiac (core/project_file.hpp), so the key is never an asset.
// Notes for the user documentation (Task 14): back up .zodiac/pack.key and keep it secret. Every release build and every patch of
// the game is sealed with it: without it no patch the shipped game accepts can be made again, and anyone who has it can read and
// change the game's packs. .zodiac/.gitignore keeps it out of git; the rest of .zodiac/ (the release manifest) belongs in version
// control, since patches are made against it.
// Threads: main thread.
#pragma once
#include <core/pack/crypto.hpp>
#include <expected>
#include <filesystem>
#include <string>

namespace zodiac::cli {
std::expected<crypto::Key,std::string> load_or_create_pack_key(const std::filesystem::path& root);
}
