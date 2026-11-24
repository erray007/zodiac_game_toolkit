if(WIN32)
set(ZODIAC_PLATFORM_DIR windows)
else()
set(ZODIAC_PLATFORM_DIR posix)
endif()
set(ZODIAC_PLATFORM_SOURCES
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/dynamic_library.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/console.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/executable_path.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/run_process.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/entropy.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/clock.cpp
${PROJECT_SOURCE_DIR}/src/platform/${ZODIAC_PLATFORM_DIR}/file.cpp
)
set(ZODIAC_CORE_SOURCES host.cpp host.hpp project_file.cpp project_file.hpp text.cpp text.hpp dynamic_library.hpp path_utf8.hpp module_registry.cpp module_registry.hpp diagnostics.cpp diagnostics.hpp script_runtime.cpp script_runtime.hpp kit_layout.cpp kit_layout.hpp payload.cpp payload.hpp coff_object.cpp coff_object.hpp static_modules.hpp types/unicode.cpp types/unicode.hpp types/unicode_tables.cpp types/unicode_tables.hpp types/heap.cpp types/heap.hpp types/string.cpp types/string.hpp types/script_guard.hpp types/string_binding.cpp types/string_binding.hpp types/exception_binding.cpp types/array.cpp types/array.hpp types/array_fast_paths.cpp types/dictionary.cpp types/dictionary.hpp library/library.hpp library/round.cpp library/round.hpp library/math_binding.cpp library/random.cpp library/random.hpp library/random_binding.cpp library/date_time.cpp library/date_time.hpp library/time_binding.cpp vfs/path.cpp vfs/path.hpp vfs/vfs.cpp vfs/vfs.hpp vfs/folder_source.cpp vfs/folder_source.hpp vfs/host_source.cpp vfs/host_source.hpp vfs/startup.cpp vfs/startup.hpp vfs/files.hpp vfs/file_binding.cpp ${ZODIAC_PLATFORM_SOURCES} ${PROJECT_SOURCE_DIR}/src/platform/file.hpp ${PROJECT_SOURCE_DIR}/src/platform/entropy.hpp ${PROJECT_SOURCE_DIR}/src/platform/clock.hpp ${PROJECT_SOURCE_DIR}/src/platform/local_offset_cache.hpp ${PROJECT_SOURCE_DIR}/src/platform/console.hpp ${PROJECT_SOURCE_DIR}/src/platform/process.hpp)
# zodiac_core is the dev configuration (zodiac.exe); zodiac_core_release is the configuration linked into release executables (lib/release in the kit).
# Both come from the same sources; the release one adds the static module record walk and ZODIAC_RELEASE=1, which plan 4's dev-only checks key on.
function(zodiac_core_target name)
add_library(${name} STATIC ${ARGN})
target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(${name} PRIVATE "ZODIAC_MODULE_SUFFIX=\"${CMAKE_SHARED_LIBRARY_SUFFIX}\"")
target_link_libraries(${name} PUBLIC angelscript zodiac_plugin_abi ${CMAKE_DL_LIBS})
target_link_libraries(${name} PRIVATE tomlplusplus::tomlplusplus)
if(ZODIAC_TEST_HOOKS)
target_compile_definitions(${name} PUBLIC ZODIAC_TEST_HOOKS=1)
endif()
# The mimalloc arm of the allocator experiment (top-level ZODIAC_BENCH_ALLOCATOR): core/types/heap.hpp calls mimalloc.
if(ZODIAC_BENCH_ALLOCATOR STREQUAL "mimalloc")
target_compile_definitions(${name} PUBLIC ZODIAC_BENCH_MIMALLOC=1)
target_link_libraries(${name} PUBLIC mimalloc-static)
endif()
# shell32 for SHGetFolderPathW (platform::user_data_folder); it is in every game's captured link line already.
if(WIN32)
target_link_libraries(${name} PUBLIC shell32)
endif()
zodiac_target_warnings(${name})
endfunction()
# pe_imports is for zodiac build only (shipping the DLLs dynamic modules import), so release executables' core leaves it out.
zodiac_core_target(zodiac_core ${ZODIAC_CORE_SOURCES} pe_imports.cpp pe_imports.hpp)
zodiac_core_target(zodiac_core_release ${ZODIAC_CORE_SOURCES} static_modules.cpp)
target_compile_definitions(zodiac_core_release PUBLIC ZODIAC_RELEASE=1)
# Like every target, the release core is unoptimized in Debug builds so it stays debuggable; the hello size gate (at most
# 5 MB, test_hello_size_gate) therefore measures Release builds only.
