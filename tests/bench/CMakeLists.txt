# bench_types (spec 8.4): Zodiac's string, array<T> and dictionary against AngelScript's sample add-ons, in one process.
# Built with everything so it always compiles, but not a ctest test: CI runs it on the release presets and gates its output with
# tools/check_bench.py. It is the only target that links angelscript_sample_addons; no game, module or kit file gets them.
add_executable(bench_types bench_types.cpp)
# The release configuration of the core: the types exactly as games link them.
target_link_libraries(bench_types PRIVATE zodiac_core_release angelscript_sample_addons)
zodiac_target_warnings(bench_types)
# A link map (bench_types.map beside the executable), so CI can find a function in the stripped, LTO-linked release binary and print
# its disassembly (the byte_at leaf's placement, Task 5b). Nothing ships it: bench_types is not part of the kit.
target_link_options(bench_types PRIVATE "-Wl,-Map=$<TARGET_FILE:bench_types>.map")
# The placement experiment (bench-reference.yml): -DZODIAC_BENCH_PAD=<bytes> links an unused function of that many nop bytes into the
# bench, a no-op relink that only moves code. Never set by a preset.
if(DEFINED ZODIAC_BENCH_PAD AND NOT ZODIAC_BENCH_PAD STREQUAL "0")
target_compile_definitions(bench_types PRIVATE ZODIAC_BENCH_PAD=${ZODIAC_BENCH_PAD})
endif()
# A second pad, inside the release core right after the byte_at leaf (string_binding.cpp), so that pairs of pads move code against other
# code instead of all of it by one shift. It changes the core of this tree only; no preset sets it.
if(DEFINED ZODIAC_BENCH_CORE_PAD AND NOT ZODIAC_BENCH_CORE_PAD STREQUAL "0")
set_property(SOURCE ${PROJECT_SOURCE_DIR}/src/core/types/string_binding.cpp TARGET_DIRECTORY zodiac_core_release APPEND PROPERTY COMPILE_DEFINITIONS ZODIAC_CORE_PLACEMENT_PAD=${ZODIAC_BENCH_CORE_PAD})
endif()
# The placement study (placement-study.yml, Task 5c): -DZODIAC_BENCH_FIXED_BASE=ON links bench_types without ASLR, so every copy of the
# file loads at its preferred base instead of a base Windows draws per image file. Windows only; never set by a preset.
option(ZODIAC_BENCH_FIXED_BASE "Link bench_types without ASLR (the placement study only)" OFF)
if(ZODIAC_BENCH_FIXED_BASE AND WIN32)
target_link_options(bench_types PRIVATE "-Wl,--disable-dynamicbase")
endif()
# The Segment Heap arm of the allocator experiment (top-level ZODIAC_BENCH_ALLOCATOR=segment): bench_types' manifest asks Windows for it,
# so the process heap and the C runtime's heap on it are segment heaps. The mimalloc arm needs nothing here: the core carries it.
if(ZODIAC_BENCH_ALLOCATOR STREQUAL "segment")
enable_language(RC)
target_sources(bench_types PRIVATE segment_heap.rc)
set_property(SOURCE segment_heap.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")
set_property(SOURCE segment_heap.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/segment_heap.manifest")
endif()
