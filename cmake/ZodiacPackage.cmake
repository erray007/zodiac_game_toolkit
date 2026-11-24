# zodiac_package()
#
# Windows only. Creates the targets that package the kit:
# - zodiac_zip, the portable kit:
#   - dist/Zodiac is emptied and filled by "cmake --install" (the layout of spec 8.1) plus README.txt, copied from packaging/README-zip.txt,
#   - tools/sign_kit.py signs bin/*.exe, bin/*.dll and modules/*.dll there when ZODIAC_SIGN_TOOL holds a sign command (it does nothing otherwise),
#   - dist/zodiac-<version>-windows-x86_64.zip holds that folder, with Zodiac/ as its only top-level entry, made by cmake -E tar --format=zip.
#   It builds what the install copies (both tools, the staged toolchain files and the shipped modules) first, but not the tests.
# - zodiac_installer, the setup wizard: after zodiac_zip, tools/build_installer.py runs .tools/innosetup/ISCC.exe (installed by bootstrap.py) on
#   packaging/zodiac.iss with /DZodiacVersion=<version> /DKitDir=<dist/Zodiac>, giving dist/zodiac-setup-<version>.exe. The setup and its uninstaller
#   are signed only when the environment variable ZODIAC_SIGN_TOOL holds a sign command (see tools/build_installer.py).
# Both refuse a Debug build (CMAKE_BUILD_TYPE Debug): the kit ships optimized binaries, so package from the windows-release preset.
# Both also refuse a tree configured with ZODIAC_TEST_HOOKS=ON, and so does "cmake --install" of it: test hooks (such as the fixed
# table hash seed, platform/entropy.hpp) must never reach a kit or a game built with one. The same holds for a tree of the allocator
# experiment (ZODIAC_BENCH_ALLOCATOR set).
# Call it after zodiac_kit_stage and every zodiac_add_module call.
include_guard(GLOBAL)
function(zodiac_package)
set(dist "${CMAKE_SOURCE_DIR}/dist")
set(kit "${dist}/Zodiac")
set(zip "${dist}/zodiac-${PROJECT_VERSION}-windows-x86_64.zip")
if(ZODIAC_TEST_HOOKS)
foreach(target zodiac_zip zodiac_installer)
add_custom_target(${target}
COMMAND "${CMAKE_COMMAND}" -E echo "${target}: this tree is configured with ZODIAC_TEST_HOOKS=ON, which no kit may ship. Build ${target} with the windows-release preset."
COMMAND "${CMAKE_COMMAND}" -E false
VERBATIM)
endforeach()
return()
endif()
# The allocator experiment (ZODIAC_BENCH_ALLOCATOR, Task 5c): no kit ships another allocator without the owner's decision.
if(NOT ZODIAC_BENCH_ALLOCATOR STREQUAL "")
foreach(target zodiac_zip zodiac_installer)
add_custom_target(${target}
COMMAND "${CMAKE_COMMAND}" -E echo "${target}: this tree is configured with ZODIAC_BENCH_ALLOCATOR=${ZODIAC_BENCH_ALLOCATOR}, which no kit may ship. Build ${target} with the windows-release preset."
COMMAND "${CMAKE_COMMAND}" -E false
VERBATIM)
endforeach()
return()
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
foreach(target zodiac_zip zodiac_installer)
add_custom_target(${target}
COMMAND "${CMAKE_COMMAND}" -E echo "${target}: this is a Debug build, and the kit ships optimized binaries. Build ${target} with the windows-release preset."
COMMAND "${CMAKE_COMMAND}" -E false
VERBATIM)
endforeach()
return()
endif()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_custom_target(zodiac_zip
COMMAND "${CMAKE_COMMAND}" -E make_directory "${dist}"
COMMAND "${CMAKE_COMMAND}" -E rm -rf "${kit}" "${zip}"
COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${kit}"
COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/packaging/README-zip.txt" "${kit}/README.txt"
COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/sign_kit.py" --kit "${kit}"
COMMAND "${CMAKE_COMMAND}" -E chdir "${dist}" "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip -- Zodiac
COMMENT "Packaging ${zip}"
VERBATIM)
add_dependencies(zodiac_zip zodiac zodiacw zodiac_kit_stage)
get_property(shipped_modules GLOBAL PROPERTY ZODIAC_SHIPPED_MODULES)
foreach(module IN LISTS shipped_modules)
add_dependencies(zodiac_zip zodiac_mod_${module})
endforeach()
add_custom_target(zodiac_installer
COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/build_installer.py"
--iscc "${CMAKE_SOURCE_DIR}/.tools/innosetup/ISCC.exe"
--script "${CMAKE_SOURCE_DIR}/packaging/zodiac.iss"
--version "${PROJECT_VERSION}"
--kit "${kit}"
--output "${dist}"
COMMENT "Building ${dist}/zodiac-setup-${PROJECT_VERSION}.exe"
VERBATIM)
add_dependencies(zodiac_installer zodiac_zip)
endfunction()
