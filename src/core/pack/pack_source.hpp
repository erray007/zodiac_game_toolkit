// zodiac::pack::make_pack_source: a PackStorage as a vfs::Source, for Vfs::mount (spec 8.3 of the library-and-packs design).
//
// - Names: a relative path is an entry name, found by fold_name. The source's root ("") is a folder while the storage is open;
//   every folder of an entry name (a name some file entry continues with '/') is a folder too. Packs hold no empty folders.
// - Deletion markers: stat gives EntryKind::deleted, open_read a null reader, list appends them; the Vfs then hides the name
//   in every older mount and in the folder below (vfs.hpp).
// - Reads: readers are PackStorage::open_mounted's, so each holds the storage: a file opened through a mount keeps reading after
//   the mount (and every pack object) is gone, and the storage is freed with its last reader. A read never takes the storage's
//   writer mutex, so it never waits for a write's I/O or encryption.
// - Writes (writable only; read only, each is access_denied): open_write buffers the file in memory, starting from the entry's
//   bytes for append and update (update needs the entry: not_found otherwise), and stores it with PackStorage::add (now as
//   its time) at commit, so a truncating write replaces the entry only then. A name that exists keeps its stored spelling.
//   sync stores what has been written so far (when anything changed since it was last stored) and flushes the pack; after
//   commit it flushes the pack. A write target holds the storage. make_directory succeeds for anything but a file (folders
//   come with the files in them); remove_directory removes every file below a folder when recursive, else is not_empty;
//   rename is PackStorage::rename, which moves the entry without rewriting it. A folder is is_directory for open_write.
// - writable sets read_only() to its opposite; the Vfs routes writes by the flag given to Vfs::mount, which mount_pack sets
//   to the same value.
// Every function may be called from several threads at once (the storage is thread-safe); a write target by one at a time.
#pragma once
#include <core/pack/pack_storage.hpp>
#include <core/vfs/vfs.hpp>
#include <memory>

namespace zodiac::pack {
std::shared_ptr<vfs::Source> make_pack_source(std::shared_ptr<PackStorage> storage,bool writable);
}
