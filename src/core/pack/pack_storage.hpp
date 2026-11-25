// zodiac::pack::PackStorage: one open pack file (layout in pack_format.hpp, spec 8.2 and 8.3 of the library-and-packs design),
// shared by every pack object, mount and reader of it in the process.
//
// Keys: KeySource is a password (Argon2id with the header's settings and salt, kdf 1) or a raw 256-bit key (kdf 0); either is
// the master key, and the file key every chunk and index is sealed under is crypto::derive_file_key(master, salt). A key of
// the other kind than the header's is a wrong key. open wipes its master and file key copies; the storage wipes its file key
// when it is freed, and a reader its own copy.
//
// Opening (open):
// - A new pack (create, and nothing at host; never with a window) gets a random salt, PackOptions' settings (std::invalid_argument
//   outside the header's limits) and at once its header, an empty index and a footer, flushed to the device. A failure removes
//   the file it made.
// - An existing pack is opened for reading and writing, or for reading only when the system refuses writing (access denied:
//   read_only() is then true). With window the pack lies at window->offset for window->size bytes of the file (a pack inside an
//   executable): it is read only, every offset in it is relative to window->offset, and nothing outside the window is read.
// - The header is decoded first: a newer version is PackError text kNewerFormatText, anything else wrong kWrongKeyText, before
//   any key is derived. A key derivation that throws (settings out of range, std::bad_alloc) is kWrongKeyText too.
// - The footer is the window's last kFooterSize bytes when they are complete (inspect_footer). When they are marked (a footer
//   that was changed: its magic or its CRC is still right), the open fails with kWrongKeyText. Only when they are none (a
//   truncated file or an interrupted append) is the window scanned backwards for the footer magic, and the first complete
//   footer found is taken: its index must authenticate, else kWrongKeyText (a complete footer is never skipped). No complete
//   footer is kWrongKeyText. The index is opened with the header as its aad, decoded and validated (validate_index, with
//   the data region ending where it starts); any failure is kWrongKeyText. Header flags that are set are kNewerFormatText,
//   checked only now, so a damaged flags byte reads as damage while a real newer pack under the right key is named.
// - After a recovery, the first write truncates the file to the end of the recovered footer.
// - A process-wide registry, keyed by the canonical host path (std::filesystem::weakly_canonical) and the window offset,
//   returns the storage already open there: the key is checked by deriving the file key from key and that storage's header and
//   comparing it in constant time (kWrongKeyText on a mismatch), and create and options are ignored. A storage leaves the
//   registry when it closes; an open waits for a storage being freed to finish closing, so two storages never write one file.
// - Failures: PackError's action and failure give the text through vfs::failure_text: failure.message is set for the pack's
//   own texts (kWrongKeyText, kNewerFormatText); otherwise the system's FileError, action read for opening and reading,
//   write for creating.
//
// Entry ids and nonces (the binding rule: a (file key, nonce) pair is never used twice):
// - A chunk's nonce is chunk_nonce(chunk index, entry id) under the file key, and a pack file keeps its salt, so its file key,
//   for life, compact() included. So every write of chunk bytes takes an id never used before in the file: a new id per add
//   (a replacement and a re-add after remove included) and per entry in compact(), drawn from crypto::random_bytes and drawn
//   again until it is non-zero and not among the ids this storage has seen (every id of the index it opened and every id it
//   has handed out since, kept for its whole life). Entries lost in a crash, and those of older indexes, are never known to
//   the storage, so for them the guarantee is the 64-bit random draw's (a collision chance of about n * m / 2^64 for n new and
//   m unknown ids). No chunk is ever rewritten in place under its old id.
// - Every index is sealed with a fresh random 12-byte nonce, never derived from the generation.
//
// Changing (exclusive lock; each needs an open, writable storage: io when closed, access_denied when read only):
// - add appends the entry's chunks after the last footer (or the chunks added since). A name that canonical_entry_name does not
//   give back unchanged is bad_path; an entry of more than 2^32 chunks is io. A replacement adds the old entry's stored bytes to
//   wasted_bytes. add_reader takes the bytes from a vfs::Reader in batches instead of from memory (MoveError::to false for a
//   failure of the reader, true for one of the pack). A failed add leaves the index as it was.
// - add_deletion_marker stores a marker (stat's EntryKind::deleted; list and count leave it out; it hides older packs' entry in
//   patch order, Task 11).
// - remove: false when there is no such entry (a marker is an entry); the entry's stored bytes become wasted.
// - set_metadata replaces the metadata (key=value lines; Tasks 10 and 11), written by the next flush.
// - flush appends a new index (generation + 1) and footer in one write and flushes them to the device (sync_data); the index and
//   footer it supersedes are added to wasted_bytes. Nothing to write is a success that writes nothing.
// - compact writes "<file>.compact.tmp" beside the file: the same header (salt, so the file key), every entry re-sealed under a
//   new id, and an index with wasted_bytes 0; flushes it, gives it the file's POSIX mode, replaces the file with it
//   (platform::replace_file) and flushes the folder (POSIX). Readers already open keep reading the old file. A failure before
//   the replace removes the temporary file and changes nothing; a failure of the folder's flush after it is reported, with the
//   new file already in use. A chunk that fails authentication fails it with damaged.
// - close flushes, stops every lookup (stat and open_reader fail with not_found, list is empty) and leaves the registry; its
//   flush's failure is returned. The last holder's release closes the storage too, sending a failure to the log (set_log).
// Lookups (shared lock): names are compared by fold_name, as VFS paths are. stat gives the entry (EntryInfo::name its last
// segment as asked); list gives the names of the files sorted by bytes; count is the number of files. open_reader takes, at
// open, the current file, the entry's id, size and offset, the chunk size and the file key, and never looks at the storage
// again: a compact or a replacement cannot change what it reads. It reads only the chunks a read touches, verifying each, and
// keeps one decrypted chunk; a chunk that fails authentication (or is cut short) is damaged, and no unauthenticated byte is
// ever returned. Readers are used by one thread at a time each, any number at once.
//
// Exceptions: std::bad_alloc from open, add, add_reader, flush and compact when memory runs out (open leaves a pack it was making
// removed, and a failed add leaves the index as it was); std::invalid_argument from open for PackOptions outside the header's
// limits when it creates a pack. Every other failure is returned.
//
// Thread-safe: a std::shared_mutex, readers share it and changes take it exclusively.
#pragma once
#include <core/pack/crypto.hpp>
#include <core/pack/pack_format.hpp>
#include <core/vfs/vfs.hpp>
#include <platform/file.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace zodiac::pack {
inline constexpr char kWrongKeyText[]="wrong key or damaged pack";
inline constexpr char kNewerFormatText[]="unsupported pack (newer format)";
struct PackOptions{
std::uint32_t kdf_kib=crypto::kDefaultKdfKib;
std::uint32_t kdf_passes=crypto::kDefaultKdfPasses;
std::uint8_t chunk_log2=kDefaultChunkLog2;
};
struct PackLocation{
std::uint64_t offset;
std::uint64_t size;
};
using KeySource=std::variant<std::string,crypto::Key>;
struct PackError{
vfs::FileAction action=vfs::FileAction::read;
vfs::Failure failure;
};
class PackStorage{
public:
using Log=std::function<void(std::string_view line)>;
static std::expected<std::shared_ptr<PackStorage>,PackError> open(const std::filesystem::path& host,KeySource key,bool create,const PackOptions& options={},std::optional<PackLocation> window={});
~PackStorage();
PackStorage(const PackStorage&)=delete;
PackStorage& operator=(const PackStorage&)=delete;
std::expected<void,vfs::FileError> add(std::string_view name,std::span<const std::byte> bytes,std::int64_t modified_utc_ms);
std::expected<void,vfs::MoveError> add_reader(std::string_view name,vfs::Reader& from,std::int64_t modified_utc_ms);
std::expected<void,vfs::FileError> add_deletion_marker(std::string_view name);
std::expected<bool,vfs::FileError> remove(std::string_view name);
std::expected<vfs::EntryInfo,vfs::FileError> stat(std::string_view name) const;
std::vector<std::string> list() const;
std::size_t count() const;
std::uint64_t wasted_bytes() const;
std::expected<std::shared_ptr<vfs::Reader>,vfs::FileError> open_reader(std::string_view name) const;
std::expected<void,vfs::FileError> flush();
std::expected<void,vfs::FileError> compact();
std::string metadata() const;
std::expected<void,vfs::FileError> set_metadata(std::string metadata);
std::expected<void,vfs::FileError> close();
bool is_open() const;
bool read_only() const noexcept{return read_only_;}
// Where a failure of the closing flush on release is reported: once set, later calls change nothing. The line is
// `error: cannot write "<name>": <reason>`.
void set_log(std::string name,Log log);
private:
struct Entry{
IndexEntry record;
// Bytes on disk (stored_size), 0 for a marker.
std::uint64_t stored=0;
};
PackStorage()=default;
static std::expected<std::unique_ptr<PackStorage>,PackError> load(const std::filesystem::path& host,const KeySource& key,bool create,const PackOptions& options,std::optional<PackLocation> window);
static std::shared_ptr<PackStorage> share(std::unique_ptr<PackStorage> storage);
std::expected<void,PackError> create_new(const PackOptions& options,const KeySource& key);
std::expected<void,PackError> read_existing(const KeySource& key);
std::expected<void,PackError> try_footer(std::uint64_t footer_offset,const Footer& footer);
std::expected<void,vfs::FileError> writable_locked() const;
std::expected<void,vfs::FileError> prepare_append_locked();
std::uint64_t new_id_locked();
// Seals size bytes as an entry's chunks at end_ under a new id: fill(offset, room) returns the plaintext of the size(room) bytes
// at offset of the entry (room itself, filled, or other memory). Returns the record (offset, id, size), or the failure.
template<class Fill>
std::expected<IndexEntry,vfs::MoveError> append_entry(std::uint64_t size,Fill&& fill);
void insert_locked(std::string fold,IndexEntry record);
std::expected<void,vfs::FileError> flush_locked();
std::expected<void,vfs::FileError> compact_locked();
// Writes the index of generation, wasted and entries, and its footer, at offset at of file (one write). Returns its length.
std::expected<std::uint64_t,vfs::FileError> write_index(platform::File& file,std::uint64_t at,std::uint64_t generation,std::uint64_t wasted,const std::vector<const IndexEntry*>& entries);
void unregister() noexcept;
void record_seal(const crypto::Nonce& nonce,bool index) const;
mutable std::shared_mutex mutex_;
std::shared_ptr<platform::File> file_;
std::filesystem::path host_;
std::uint64_t base_=0;
std::uint64_t window_size_=0;
bool windowed_=false;
bool read_only_=false;
HeaderBytes header_bytes_{};
Header header_{};
crypto::Key file_key_{};
std::uint64_t chunk_size_=0;
// By fold_name.
std::unordered_map<std::string,Entry> entries_;
// Every id the storage has seen in its index or handed out (the nonce rule above).
std::unordered_set<std::uint64_t> used_ids_;
std::size_t files_=0;
std::uint64_t generation_=0;
std::uint64_t wasted_=0;
std::string metadata_;
// Relative to base_: where the next append goes, and the current index (its footer follows it).
std::uint64_t end_=0;
std::uint64_t index_offset_=0;
std::uint64_t index_length_=0;
bool dirty_=false;
// The file holds bytes past end_ (a recovered crash, a failed write): the next append truncates it first.
bool truncate_pending_=false;
bool closed_=false;
// The registry slot (the canonical path's text and base_): set once the storage is shared, cleared when it leaves.
std::filesystem::path::string_type registry_path_;
bool registered_=false;
std::string log_name_;
Log log_;
#if defined(ZODIAC_TEST_HOOKS)
std::array<std::uint8_t,32> key_tag_{};
#endif
};
#if defined(ZODIAC_TEST_HOOKS)
namespace testing {
// Test hook: one seal a PackStorage made, under the file key whose BLAKE2b-256 hash is key_tag: a chunk's (index false) or
// an index's (index true).
struct SealRecord{
std::array<std::uint8_t,32> key_tag;
crypto::Nonce nonce;
bool index;
};
// Starts recording every seal of every PackStorage into log (nullptr stops). Thread-safe; log must outlive the recording.
void pack_record_seals(std::vector<SealRecord>* log) noexcept;
}
#endif
}
