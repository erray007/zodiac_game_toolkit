# Release-configuration compile and link flags of every target: Zodiac's own, its dependencies (AngelScript, its sample add-ons, doctest), the kit's
# lib/release archives, the modules, the tests and the tools (zodiac.exe, zodiacw.exe). Include it after project() and before any target or dependency
# is added. Chosen by measurement (the Outcome of Task 7 in docs/superpowers/plans/2026-11-16-zodiac-core-types.md): the owner wants release builds
# fastest first, then smallest, on every platform (game servers will run on Linux); build time does not count.
# - Every release object (C and C++): -O3 -DNDEBUG, -ffunction-sections -fdata-sections and full LTO (-flto), so the archives (the kit's lib/release
#   on Windows) hold LLVM bitcode and every link optimizes the whole program as one module; zodiac build links games with the kit's own ld.lld.
#   Full LTO measured faster than ThinLTO (paired geomean 0.953 of ThinLTO's times over the 29 bench operations) and gives a smaller hello.
# - Every release link (executables and shared libraries, the kit's own and, through the captured link line, every game): -flto (the LTO backend
#   runs at the -O3 the driver passes it as -plugin-opt=O3), --gc-sections, -s and --icf=safe. Safe ICF folds identical functions only when their
#   address is never taken in a way that is significant, as the objects' address-significance tables record (clang writes them by default on COFF
#   and ELF), so comparisons of function pointers, such as AngelScript's, keep their meaning; --icf=all saved nothing more on the hello.
# - Both platforms link with lld: llvm-mingw's on Windows, the system's on Linux (cmake/toolchain-clang-linux.cmake, -fuse-ld=lld), with the LLVM
#   archiver and symbol lister, since GNU ar, ranlib and nm cannot index or read bitcode.
#   The kit's executables are linked like games, so bench_types measures exactly what a game gets.
# Debug builds are left alone (CMake's -O0 -g), so they stay fully debuggable.
# Sets ZODIAC_RELEASE_GAME_LINK_FLAGS: the driver flags the release link line is captured with (cmake/CaptureReleaseLink.cmake); they hold the
# optimization level because the driver turns it into the LTO backend's level.
include_guard(GLOBAL)
set(ZODIAC_RELEASE_OPT -O3)
set(CMAKE_C_FLAGS_RELEASE "${ZODIAC_RELEASE_OPT} -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${ZODIAC_RELEASE_OPT} -DNDEBUG")
set(compile -ffunction-sections -fdata-sections -flto)
set(link -flto -Wl,--gc-sections -s -Wl,--icf=safe)
# Code alignment (Task 5c fix round 2, ZODIAC_RELEASE_CODE_ALIGN): how much of the code starts on fixed boundaries, so a change elsewhere moves
# less of what runs hot (the regression gate read such layout shifts as regressions). "functions": every function on 64 bytes (-falign-functions
# at compile time, which the bitcode keeps, and the LTO code generator's -align-all-functions at the link). "blocks": that, plus every block
# not entered by fall-through and every loop on 32 bytes at the link (the VM's computed-goto handlers in asCContext::ExecuteNext are such
# blocks). Empty: neither. The link options are in the captured game link line too, so games are laid out like the kit.
# blocks is the default (placement-study 45582108121, 8 Windows runners, 4 relink layouts each): the spread of an operation's time across
# layouts fell from a 90th percentile of 6.8 % to 4.1 %, and every operation got faster or stayed (geometric mean 0.966, slowest 1.007).
# Its halves (blocks without loops, loops without the other blocks) and functions alone were no better on any runner class
# (placement-study 45606248890, 16 runners); functions alone made Zodiac 3-11 % slower than blocks.
set(ZODIAC_RELEASE_CODE_ALIGN "blocks" CACHE STRING "Release code alignment: empty, functions or blocks")
set_property(CACHE ZODIAC_RELEASE_CODE_ALIGN PROPERTY STRINGS "" functions blocks)
if(ZODIAC_RELEASE_CODE_ALIGN MATCHES "^(functions|blocks)$")
list(APPEND compile -falign-functions=64)
list(APPEND link "-Wl,-mllvm,-align-all-functions=6")
if(ZODIAC_RELEASE_CODE_ALIGN STREQUAL "blocks")
list(APPEND link "-Wl,-mllvm,-align-all-nofallthru-blocks=5" "-Wl,-mllvm,-align-loops=32")
endif()
elseif(NOT ZODIAC_RELEASE_CODE_ALIGN STREQUAL "")
message(FATAL_ERROR "ZODIAC_RELEASE_CODE_ALIGN is \"${ZODIAC_RELEASE_CODE_ALIGN}\"; it takes functions, blocks or nothing")
endif()
add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:${compile}>")
add_link_options("$<$<CONFIG:Release>:${link}>")
set(ZODIAC_RELEASE_GAME_LINK_FLAGS ${ZODIAC_RELEASE_OPT} ${link})
