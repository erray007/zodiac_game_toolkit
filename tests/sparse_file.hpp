// zodiac_test::make_sparse (sparse_file.cpp): a sparse file of offset + text.size() bytes holding text at offset and nothing (a
// hole) before it, for tests of files larger than anything they could write out (test_files, test_pack_mount). Fails (a doctest
// REQUIRE) rather than write the hole out when the file system cannot keep it sparse. The system headers stay in the .cpp, so
// windows.h's macros (near, far, ...) never reach a test.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace zodiac_test {
void make_sparse(const std::filesystem::path& file,std::uint64_t offset,std::string_view text);
}
