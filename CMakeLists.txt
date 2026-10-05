# Host build: portable core, BearSSL, the vjo-cli tool and unit tests.
# The Vita build lives in vita/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
project(vita_jp_overlay_host C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB_RECURSE BEARSSL_SRC ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/*.c)
add_library(bearssl STATIC ${BEARSSL_SRC})
target_include_directories(bearssl PUBLIC third_party/bearssl/inc PRIVATE third_party/bearssl/src)
target_compile_options(bearssl PRIVATE -w)

set(CORE_SRC
  core/arena.c core/pb.c core/utf.c core/lens.c core/json.c core/jpdb.c
  core/textfilter.c core/entries.c core/jiten.c core/config.c core/regions.c
  core/http.c core/tls.c core/tls_roots.c core/client.c core/styled.c core/jpegsw.c
  core/base64.c core/anki.c core/net.c)
add_library(vjo_core STATIC ${CORE_SRC})
target_include_directories(vjo_core PUBLIC core include)
target_link_libraries(vjo_core PUBLIC bearssl)
target_compile_definitions(vjo_core PUBLIC VJO_HOST=1)
target_compile_options(vjo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(vjo_host STATIC host/net_posix.c host/render.c host/replay.c)
target_include_directories(vjo_host PUBLIC host)
target_link_libraries(vjo_host PUBLIC vjo_core)
target_compile_options(vjo_host PRIVATE -Wall -Wextra)

add_executable(vjo-cli host/cli.c)
target_link_libraries(vjo-cli vjo_host)

# Raw RGBA -> JPEG with the software encoder (tools/test_jpegsw.py).
add_executable(vjo-jpegsw host/jpegsw_tool.c)
target_link_libraries(vjo-jpegsw vjo_host)

enable_testing()
# kernel/foreground.c, kernel/triggers.c: kernel state machines (no psp2kern).
add_executable(vjo-tests host/tests/test_core.c kernel/foreground.c kernel/triggers.c)
target_include_directories(vjo-tests PRIVATE third_party/acutest/include kernel)
target_link_libraries(vjo-tests vjo_host)
target_compile_definitions(vjo-tests PRIVATE VJO_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures")
add_test(NAME core COMMAND vjo-tests)
add_executable(vjo-anki-tests host/tests/test_anki.c)
target_include_directories(vjo-anki-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-anki-tests vjo_host)
add_test(NAME anki COMMAND vjo-anki-tests)
