include(FetchContent)
set(FETCHCONTENT_QUIET ON)
# Zodiac's change to AngelScript (marked in the source and in THIRD_PARTY_LICENSES, as its zlib license asks): decimal literals compile to the
# nearest double or float. The hashes of the patch and of the script that applies it are on the command line (the script ignores them) so that
# FetchContent applies the patch again when either changes.
find_package(Git REQUIRED)
set(ZODIAC_AS_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/angelscript-exact-literals.patch")
set(ZODIAC_APPLY_PATCH "${CMAKE_CURRENT_LIST_DIR}/ZodiacApplyPatch.cmake")
file(SHA256 "${ZODIAC_AS_PATCH}" ZODIAC_AS_PATCH_SHA256)
file(SHA256 "${ZODIAC_APPLY_PATCH}" ZODIAC_APPLY_PATCH_SHA256)
FetchContent_Declare(angelscript_sdk
URL https://www.angelcode.com/angelscript/sdk/files/angelscript_2.39.0.zip
URL_HASH SHA256=f029bea4e52c360c750d3a805b80f9317d177241fadbc8f93845e38355d11287
DOWNLOAD_EXTRACT_TIMESTAMP TRUE
PATCH_COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}" "-DPATCH=${ZODIAC_AS_PATCH}" "-DPATCH_SHA256=${ZODIAC_AS_PATCH_SHA256}" "-DSCRIPT_SHA256=${ZODIAC_APPLY_PATCH_SHA256}" "-DSOURCE_DIR=<SOURCE_DIR>" -P "${ZODIAC_APPLY_PATCH}"
)
FetchContent_Declare(tomlplusplus
URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(doctest
URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB ON CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(tomlplusplus doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)
FetchContent_GetProperties(angelscript_sdk)
if(NOT angelscript_sdk_POPULATED)
FetchContent_MakeAvailable(angelscript_sdk)
endif()
set(AS_SRC "${angelscript_sdk_SOURCE_DIR}/angelscript/source")
set(AS_ADDON "${angelscript_sdk_SOURCE_DIR}/add_on")
file(GLOB AS_CORE_SOURCES "${AS_SRC}/as_*.cpp")
list(FILTER AS_CORE_SOURCES EXCLUDE REGEX "as_callfunc_[a-z0-9_]+\.cpp$")
set(AS_CALLFUNC_SOURCES "${AS_SRC}/as_callfunc_x64_gcc.cpp" "${AS_SRC}/as_callfunc_x64_mingw.cpp")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
set(AS_CALLFUNC_SOURCES "${AS_SRC}/as_callfunc_arm64.cpp")
if(NOT WIN32)
enable_language(ASM)
if(APPLE)
list(APPEND AS_CALLFUNC_SOURCES "${AS_SRC}/as_callfunc_arm64_xcode.S")
else()
list(APPEND AS_CALLFUNC_SOURCES "${AS_SRC}/as_callfunc_arm64_gcc.S")
endif()
endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm" AND NOT WIN32)
enable_language(ASM)
set(AS_CALLFUNC_SOURCES "${AS_SRC}/as_callfunc_arm.cpp" "${AS_SRC}/as_callfunc_arm_gcc.S")
set_property(SOURCE "${AS_SRC}/as_callfunc_arm_gcc.S" APPEND PROPERTY COMPILE_FLAGS "-Wa,-mimplicit-it=always")
endif()
add_library(angelscript STATIC
${AS_CORE_SOURCES}
"${AS_SRC}/as_callfunc.cpp"
${AS_CALLFUNC_SOURCES}
"${AS_ADDON}/scriptbuilder/scriptbuilder.cpp"
"${AS_ADDON}/scripthelper/scripthelper.cpp"
)
# Users of angelscript see only the headers of the add-ons it holds (scriptbuilder, scripthelper), copied into an include
# directory of their own; the add_on directory itself, with the sample add-ons' headers, is public only on
# angelscript_sample_addons below.
set(AS_ADDON_INCLUDE "${CMAKE_BINARY_DIR}/angelscript_addon_include")
foreach(header scriptbuilder/scriptbuilder.h scripthelper/scripthelper.h)
configure_file("${AS_ADDON}/${header}" "${AS_ADDON_INCLUDE}/${header}" COPYONLY)
endforeach()
target_include_directories(angelscript SYSTEM PUBLIC "${angelscript_sdk_SOURCE_DIR}/angelscript/include" "${AS_ADDON_INCLUDE}")
find_package(Threads REQUIRED)
target_link_libraries(angelscript PUBLIC Threads::Threads)
set_target_properties(angelscript PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(angelscript PRIVATE -w)
# AngelScript's number literal scanner for test_script_literals: its private header as_string_util.h (AS_ZODIAC_FROM_CHARS, the test hook).
# ZODIAC_AS_STRTOD_FALLBACK builds the scanner's fallback, strtod_l and strtof_l in the "C" locale, which a toolchain whose C++ library has a
# correctly rounded floating-point from_chars never compiles (Zodiac's Windows toolchain), with the hook that makes the "C" locale handle null.
# Only for testing the fallback (CI builds test_script_literals with it); the default build, the kit and games never set it.
option(ZODIAC_AS_STRTOD_FALLBACK "Build AngelScript's number literal scanner with its strtod_l fallback and test hook (tests only)" OFF)
add_library(angelscript_literal_scanner INTERFACE)
target_include_directories(angelscript_literal_scanner SYSTEM INTERFACE "${AS_SRC}")
target_link_libraries(angelscript_literal_scanner INTERFACE angelscript)
if(ZODIAC_AS_STRTOD_FALLBACK)
set(ZODIAC_AS_FALLBACK_DEFINITIONS AS_ZODIAC_FROM_CHARS=0 AS_ZODIAC_TEST_HOOKS)
target_compile_definitions(angelscript PRIVATE ${ZODIAC_AS_FALLBACK_DEFINITIONS})
target_compile_definitions(angelscript_literal_scanner INTERFACE ${ZODIAC_AS_FALLBACK_DEFINITIONS} ZODIAC_AS_STRTOD_FALLBACK)
endif()
# The sample add-ons Zodiac's string, array<T>, dictionary and maths replace (spec 8.4). Only tests/bench links them, to measure Zodiac's
# types and library against them; games, the core and the kit never get them (test_angelscript_has_no_sample_addons checks every archive
# of lib/release).
add_library(angelscript_sample_addons STATIC
"${AS_ADDON}/scriptstdstring/scriptstdstring.cpp"
"${AS_ADDON}/scriptstdstring/scriptstdstring_utils.cpp"
"${AS_ADDON}/scriptarray/scriptarray.cpp"
"${AS_ADDON}/scriptdictionary/scriptdictionary.cpp"
"${AS_ADDON}/scriptmath/scriptmath.cpp"
)
target_include_directories(angelscript_sample_addons SYSTEM PUBLIC "${AS_ADDON}")
target_link_libraries(angelscript_sample_addons PUBLIC angelscript)
target_compile_options(angelscript_sample_addons PRIVATE -w)
# scriptmath registers float functions by default; Zodiac's maths are double, so the add-on is measured in its double configuration.
target_compile_definitions(angelscript_sample_addons PRIVATE AS_USE_FLOAT=0)
# mimalloc (MIT, THIRD_PARTY_LICENSES), only for the allocator experiment (ZODIAC_BENCH_ALLOCATOR=mimalloc, Task 5c): a static library that
# does not override malloc (MI_OVERRIDE OFF; the C runtime's malloc on MinGW stays the C runtime's), called by name from core/types/heap.hpp,
# the engine's memory functions and bench_types' operator new and delete. Never fetched otherwise.
if(ZODIAC_BENCH_ALLOCATOR STREQUAL "mimalloc")
FetchContent_Declare(mimalloc
URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.5.3.tar.gz
URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47
DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)
target_compile_options(mimalloc-static PRIVATE -w)
endif()
