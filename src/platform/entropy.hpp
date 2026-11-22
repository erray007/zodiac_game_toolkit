// Random bytes from the operating system's entropy source: RtlGenRandom on Windows, getrandom (then /dev/urandom) elsewhere.
// Implemented per platform in src/platform/<os>/entropy.cpp. It seeds Zodiac's per-process secrets, such as the keys of
// String::table_hash() (src/core/types/string.cpp), which places every dictionary key, and is where the script random
// generator of plan 4b is to be seeded from.
// Test hook: only a tree configured with ZODIAC_TEST_HOOKS=ON (off by default; never the release presets or the kit, whose
// packaging targets refuse such a tree) declares and defines test_hook_table_hash_seed. test_no_test_hooks proves the hook's
// symbol and its variable name are absent from every other build.
#pragma once
#include <cstddef>
#if defined(ZODIAC_TEST_HOOKS)
#include <cstdint>
#endif

namespace zodiac::platform {
// Fills size bytes at buffer; false when the source fails (the bytes are then unspecified). Thread-safe.
bool fill_entropy(void* buffer,std::size_t size) noexcept;
#if defined(ZODIAC_TEST_HOOKS)
// The whole decimal number in the environment variable ZODIAC_TABLE_HASH_SEED, read from the operating system's environment
// block (safe before main); false when it is unset or empty. Any other text prints a line to stderr and aborts, so a
// mistyped seed never measures a random one.
bool test_hook_table_hash_seed(std::uint64_t& seed) noexcept;
#endif
}
