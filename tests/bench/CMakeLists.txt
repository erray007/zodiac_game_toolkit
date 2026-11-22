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
