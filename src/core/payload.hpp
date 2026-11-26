// zodiac::GamePayload is everything a release executable needs to start a game: the kit and game identity, the modules to register
// (in registration order, with their versions and how they are linked), [core] exclude, diagnostics settings, the release pack and
// [files] settings, and the compiled scripts.
//
// Wire format version 2 (all integers little-endian, independent of the host):
// "ZDPAYLD1" | u32 format_version | str kit_version | str game_name | u32 n, n x str requested
// | u32 n, n x (str name, str version, u32 link) | u32 n, n x str core_exclude
// | u32 console, str file (UTF-8, '/' separators), u32 dialog | u32 pack_mode | u32 pack_writable | u32 host_paths | str pack_key
// | u32 n, n x (str section, u32 is_main, str bytecode) | u32 crc
// where str is a u32 byte length followed by the bytes, and crc is the CRC-32 (IEEE 802.3, as in zlib) of every byte before it.
// Version 1 (without the four pack and file fields) is no longer read: a game is always built and run by the same kit.
// Threads: both functions are pure and may be called from any thread.
#pragma once
#include <core/project_file.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace zodiac {
enum class LinkMode:uint8_t{static_link=1,dynamic_link=2};
struct PayloadModule{
std::string name;
std::string version;
LinkMode link;
bool operator==(const PayloadModule&) const=default;
};
struct PayloadScript{
// Section name as used in messages (project-relative, '/' separators).
std::string section;
bool is_main;
// Output of ScriptRuntime::save_bytecode.
std::string bytecode;
bool operator==(const PayloadScript&) const=default;
};
// Where a release's pack is (spec 8.4): none (the assets are copied files, or there are none), appended to the executable, or <game>.zpk beside it.
enum class PackMode:uint8_t{none=0,embedded=1,external=2};
// The size of GamePayload::pack_key when it is set (a crypto::Key).
inline constexpr std::size_t kPayloadPackKeySize=32;
struct GamePayload{
uint32_t format_version=2;
std::string kit_version;
std::string game_name;
std::vector<std::string> requested;      // names passed to ModuleRegistry::load, in that order
std::vector<PayloadModule> modules;      // registration order
std::vector<std::string> core_exclude;
DiagnosticsConfig diagnostics;           // file stored relative to the root
PackMode pack_mode=PackMode::none;
// The release mounts its pack writable ([pack] writable).
bool pack_writable=false;
// [files] host_paths.
bool host_paths=false;
// The pack's key: empty, or exactly kPayloadPackKeySize bytes.
std::string pack_key;
std::vector<PayloadScript> scripts;
bool operator==(const GamePayload&) const=default;
};
// Encodes payload in the wire format above; format_version is written as given. Throws std::length_error when a string or a count does not fit in a u32.
std::string serialize_payload(const GamePayload& payload);
// Decodes and validates bytes: magic, CRC, every length and count against the remaining bytes, format version 2 (any other:
// "(unsupported version <n>)"), enum values, flags of 0 or 1, the key's size, no trailing data.
// Never reads outside bytes. Each error is one line starting with "error: the game data is damaged", e.g. "error: the game data is damaged (checksum mismatch)".
std::expected<GamePayload,std::string> parse_payload(std::string_view bytes);
// CRC-32 (IEEE 802.3, reflected, initial and final value 0xFFFFFFFF) of bytes.
uint32_t crc32(std::string_view bytes) noexcept;
}
