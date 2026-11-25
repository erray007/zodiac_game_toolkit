# cmake -DNM=<nm> -DHOOKS=<ON|OFF> -DARCHIVES=<a.a|b.a> -DBINARIES=<x.exe|y.exe> -P check_no_test_hooks.cmake
#
# The test test_no_test_hooks: the table hash seed hook (platform/entropy.hpp, compiled only with ZODIAC_TEST_HOOKS=ON) is absent from
# every archive and executable of a tree without it, and present in a tree with it, so the check itself is shown to see it.
# - ARCHIVES (the cores, zodiac_core and zodiac_core_release; separated by | so the list survives add_test's command line): llvm-nm lists
#   the symbols test_hook_table_hash_seed and string_array_steps (any mangling) iff HOOKS. Each archive must also define fill_entropy, the hook's neighbour, so a
#   listing that read nothing cannot pass. Release archives hold LLVM bitcode (full LTO), which llvm-nm reads.
# - BINARIES (linked executables, native code): the hooks' variable names ZODIAC_TABLE_HASH_SEED and ZODIAC_ARRAY_BLOCK_CACHE, the texts
#   they read the environment with, are in the file iff HOOKS. Each must also hold "zodiac-dictionary 1" (serialize()'s header, in every core), so a read that found no
#   text cannot pass.
# - In every tree: no archive names mimalloc's functions (mi_malloc, mi_free, mi_new...) and no binary holds the text "mimalloc"; only the
#   allocator experiment's tree (ZODIAC_BENCH_ALLOCATOR=mimalloc, which refuses to install or package) links it.
# It fails naming each archive or executable that breaks the rule.
foreach(variable NM HOOKS ARCHIVES BINARIES)
if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
message(FATAL_ERROR "check_no_test_hooks: ${variable} is not set")
endif()
endforeach()
string(REPLACE "|" ";" archives "${ARCHIVES}")
string(REPLACE "|" ";" binaries "${BINARIES}")
set(failures "")
foreach(archive IN LISTS archives)
if(NOT EXISTS "${archive}")
message(FATAL_ERROR "check_no_test_hooks: ${archive} does not exist")
endif()
execute_process(COMMAND "${NM}" "${archive}" RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
message(FATAL_ERROR "check_no_test_hooks: ${NM} failed on ${archive} (${result}): ${errors}")
endif()
if(NOT symbols MATCHES "[0-9a-fA-F-]+ +T +[^\r\n]*fill_entropy")
message(FATAL_ERROR "check_no_test_hooks: ${NM} listed no definition of fill_entropy in ${archive}, so its symbols were not read")
endif()
# Every hooks-only symbol: the seed hook (platform/entropy.hpp), keys()'s step sums (types/array.hpp) and, in Windows trees (PRIVATE_HEAP
# ON), zodiac::heap::private_heap of the private heap (types/heap.cpp; elsewhere it is an inline wrapper that no core file calls, and
# heap.cpp is empty).
set(hooks test_hook_table_hash_seed string_array_steps)
if(PRIVATE_HEAP)
list(APPEND hooks "4heap12private_heap")
endif()
# mimalloc (the allocator experiment's, ZODIAC_BENCH_ALLOCATOR=mimalloc) is in no tree but that experiment's, hooks or not: no core
# archive may name its functions (a hooks tree does not change that, so this is checked in every tree).
string(REGEX MATCHALL "[^\r\n]* mi_(malloc|calloc|realloc|free|new)[^\r\n]*" mimalloc_found "${symbols}")
if(mimalloc_found)
list(JOIN mimalloc_found "\n" shown)
string(APPEND failures "\n${archive} names mimalloc's functions:\n${shown}")
endif()
foreach(hook IN LISTS hooks)
string(REGEX MATCHALL "[^\r\n]*${hook}[^\r\n]*" found "${symbols}")
if(HOOKS AND NOT found)
string(APPEND failures "\n${archive} lacks the hook symbol ${hook} in a ZODIAC_TEST_HOOKS tree")
elseif(NOT HOOKS AND found)
list(JOIN found "\n" shown)
string(APPEND failures "\n${archive} holds the test hook ${hook}:\n${shown}")
endif()
endforeach()
endforeach()
foreach(binary IN LISTS binaries)
if(NOT EXISTS "${binary}")
message(FATAL_ERROR "check_no_test_hooks: ${binary} does not exist")
endif()
file(STRINGS "${binary}" texts REGEX "ZODIAC_TABLE_HASH_SEED|ZODIAC_ARRAY_BLOCK_CACHE|zodiac-dictionary 1|mimalloc")
if(texts MATCHES "mimalloc")
string(APPEND failures "\n${binary} holds the text mimalloc (only the allocator experiment's tree may)")
endif()
if(NOT texts MATCHES "zodiac-dictionary 1")
message(FATAL_ERROR "check_no_test_hooks: \"zodiac-dictionary 1\" was not found in ${binary}, so its texts were not read")
endif()
# The hooks' environment variables: the table hash seed (platform/entropy.hpp) and the array block cache bypass (types/array.cpp).
foreach(variable ZODIAC_TABLE_HASH_SEED ZODIAC_ARRAY_BLOCK_CACHE)
if(texts MATCHES "${variable}")
set(present TRUE)
else()
set(present FALSE)
endif()
if(HOOKS AND NOT present)
string(APPEND failures "\n${binary} lacks ${variable} in a ZODIAC_TEST_HOOKS tree")
elseif(NOT HOOKS AND present)
string(APPEND failures "\n${binary} holds ${variable}, a test hook's environment variable")
endif()
endforeach()
endforeach()
if(failures)
message(FATAL_ERROR "check_no_test_hooks:${failures}")
endif()
if(HOOKS)
message(STATUS "check_no_test_hooks: the hook is in every checked file (a ZODIAC_TEST_HOOKS tree): ${archives} ${binaries}")
else()
message(STATUS "check_no_test_hooks: no checked file holds the hook: ${archives} ${binaries}")
endif()
