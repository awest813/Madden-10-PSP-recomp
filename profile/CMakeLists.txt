set(PSPWEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(PSPWEB_GAME "hello" CACHE STRING
    "Title whose generated AOT corpus is linked (profiles/web/generated/<game>)")
set(PSPWEB_GUEST_PATH "ms0:/PSP/GAME/PSPWEB/EBOOT.PBP" CACHE STRING
    "Module path passed to module_start as argp")
set(PSPWEB_LINK_OPT "1" CACHE STRING
    "Emscripten only: link-time optimization level (wasm-opt -O3 on the full corpus takes very long)")
option(PSPWEB_PROFILING "Emscripten only: keep function names in the wasm for browser profilers" OFF)
set(PSPWEB_PRELOAD_DIR "" CACHE PATH
    "Emscripten only: host directory preloaded as /game (EBOOT.BIN, disc/, ms0/)")
option(PSPWEB_SDL "Native only: build the SDL2 windowed player (software present; GL later)" OFF)

set(PSPWEB_GENERATED_DIR "${PSPWEB_DIR}/generated/${PSPWEB_GAME}")
file(GLOB PSPWEB_GENERATED CONFIGURE_DEPENDS "${PSPWEB_GENERATED_DIR}/*.cpp")
if(NOT PSPWEB_GENERATED)
    message(FATAL_ERROR "No generated AOT code in ${PSPWEB_GENERATED_DIR}; run scripts/generate.sh first")
endif()

# Generated units are huge single functions; keep them at the configured
# optimization level and silence warnings that are expected in emitted code.
add_library(pspweb_generated STATIC ${PSPWEB_GENERATED})
target_link_libraries(pspweb_generated PUBLIC psprecomp_core)
target_compile_options(pspweb_generated PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -w)
set(PSPWEB_INSTRUCTION_TRACE_UNITS "" CACHE STRING
    "AOT unit suffixes with pre-instruction fault diagnostics, e.g. 0286")
set(PSPWEB_INSTRUCTION_TRACE_OPT_LEVEL "" CACHE STRING
    "Optional optimization override for diagnostic units, e.g. 0")
foreach(unit IN LISTS PSPWEB_INSTRUCTION_TRACE_UNITS)
    set_property(SOURCE "${PSPWEB_GENERATED_DIR}/generated_unit_${unit}.cpp"
        APPEND PROPERTY COMPILE_DEFINITIONS PSPRECOMP_TRACE_INSTRUCTIONS=1)
    if(NOT PSPWEB_INSTRUCTION_TRACE_OPT_LEVEL STREQUAL "")
        set_property(SOURCE "${PSPWEB_GENERATED_DIR}/generated_unit_${unit}.cpp"
            APPEND PROPERTY COMPILE_OPTIONS "-O${PSPWEB_INSTRUCTION_TRACE_OPT_LEVEL}")
    endif()
endforeach()

# ATRAC3/ATRAC3+ decoder (FFmpeg code via PPSSPP, LGPL 2.1+), used for streamed music and speech.
set(PSPWEB_AT3_DIR "${PSPWEB_DIR}/third_party/at3_standalone")
add_library(pspweb_at3 STATIC
    ${PSPWEB_AT3_DIR}/atrac.cpp ${PSPWEB_AT3_DIR}/atrac3.cpp ${PSPWEB_AT3_DIR}/atrac3plus.cpp
    ${PSPWEB_AT3_DIR}/atrac3plusdec.cpp ${PSPWEB_AT3_DIR}/atrac3plusdsp.cpp ${PSPWEB_AT3_DIR}/get_bits.cpp
    ${PSPWEB_AT3_DIR}/compat.cpp ${PSPWEB_AT3_DIR}/fft.cpp ${PSPWEB_AT3_DIR}/mem.cpp)
target_include_directories(pspweb_at3 PRIVATE "${PSPWEB_DIR}/third_party/at3_shim")
target_include_directories(pspweb_at3 PUBLIC "${PSPWEB_AT3_DIR}")
target_compile_options(pspweb_at3 PRIVATE -O2 -w)

set(PSPWEB_HOST_SOURCES host/kernel.cpp host/devices.cpp host/net.cpp host/osk.cpp host/io.cpp host/pgd.cpp host/mpeg.cpp host/sas.cpp host/atrac.cpp host/savedata.cpp host/ge.cpp host/ge_gl.cpp host/ge_worker.cpp host/webfs.cpp host/sdl_frontend.cpp)
add_executable(pspweb host/main.cpp ${PSPWEB_HOST_SOURCES})
target_link_libraries(pspweb PRIVATE pspweb_generated psprecomp_core pspweb_at3)
target_compile_definitions(pspweb PRIVATE PSPWEB_GUEST_PATH="${PSPWEB_GUEST_PATH}")
if(PSPWEB_GAME STREQUAL "ncaa10")
    target_compile_definitions(pspweb PRIVATE PSPWEB_NCAA10=1)
    target_sources(pspweb PRIVATE host/ncaa_sound.cpp)
    target_sources(pspweb_generated PRIVATE generated/ncaa10_sound/ncaa_sound.cpp)
elseif(PSPWEB_GAME STREQUAL "madden10")
    target_compile_definitions(pspweb PRIVATE PSPWEB_MADDEN10=1)
    target_sources(pspweb PRIVATE host/madden_sound.cpp)
    target_sources(pspweb_generated PRIVATE generated/madden10_sound/madden_sound.cpp)
endif()
target_compile_options(pspweb PRIVATE -Wall -Wextra)
set_target_properties(pspweb PROPERTIES OUTPUT_NAME "pspweb_${PSPWEB_GAME}")

if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(pspweb PRIVATE EGL GLESv2 Threads::Threads)
    option(PSPWEB_HOST_TESTS "Build native host HLE regression tests" OFF)
    if(PSPWEB_HOST_TESTS)
        enable_testing()
        # Native display-list diagnostic; no game data or CPU interpreter.
        add_executable(pspweb_ge_replay tests/ge_replay.cpp host/ge.cpp host/ge_gl.cpp)
        target_include_directories(pspweb_ge_replay PRIVATE host)
        target_link_libraries(pspweb_ge_replay PRIVATE psprecomp_core EGL GLESv2 Threads::Threads)
        add_library(pspweb_test_host STATIC ${PSPWEB_HOST_SOURCES})
        target_include_directories(pspweb_test_host PUBLIC host)
        target_link_libraries(pspweb_test_host PUBLIC psprecomp_core pspweb_at3 EGL GLESv2 Threads::Threads)
        foreach(suite audio utility controller ge ge_plane scheduler presentation memory)
            add_executable(pspweb_${suite}_tests tests/${suite}.cpp)
            target_link_libraries(pspweb_${suite}_tests PRIVATE pspweb_test_host)
            if(MSVC)
                target_compile_options(pspweb_${suite}_tests PRIVATE /UNDEBUG)
            else()
                target_compile_options(pspweb_${suite}_tests PRIVATE -UNDEBUG)
            endif()
            add_test(NAME pspweb_${suite}_tests COMMAND pspweb_${suite}_tests)
        endforeach()
        add_test(NAME pspweb_scheduler_diagnostic_tests COMMAND pspweb_scheduler_tests)
        set_tests_properties(pspweb_scheduler_diagnostic_tests PROPERTIES
            ENVIRONMENT "PSPRECOMP_TRACE_ON_ERROR=1")
        add_test(NAME pspweb_scheduler_profile_tests COMMAND pspweb_scheduler_tests)
        set_tests_properties(pspweb_scheduler_profile_tests PROPERTIES
            ENVIRONMENT "PSPRECOMP_PROFILE_DISPATCH=1;PSPRECOMP_PROFILE_DISPATCH_START=100000000")
        if(PSPWEB_GAME STREQUAL "ncaa10")
            add_executable(pspweb_disc_reader_tests tests/disc_reader.cpp)
            target_include_directories(pspweb_disc_reader_tests PRIVATE host)
            target_link_libraries(pspweb_disc_reader_tests PRIVATE pspweb_generated)
            if(MSVC)
                target_compile_options(pspweb_disc_reader_tests PRIVATE /UNDEBUG)
            else()
                target_compile_options(pspweb_disc_reader_tests PRIVATE -UNDEBUG)
            endif()
            if("0284" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS AND
               "0285" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS AND
               "0286" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS AND
               "0306" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS AND
               "0309" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS)
                add_test(NAME pspweb_disc_reader_tests COMMAND pspweb_disc_reader_tests diagnostic provenance)
            elseif("0286" IN_LIST PSPWEB_INSTRUCTION_TRACE_UNITS)
                add_test(NAME pspweb_disc_reader_tests COMMAND pspweb_disc_reader_tests diagnostic)
            else()
                add_test(NAME pspweb_disc_reader_tests COMMAND pspweb_disc_reader_tests)
            endif()
        endif()
    endif()
    if(PSPWEB_SDL)
        find_package(SDL2 REQUIRED)
        target_link_libraries(pspweb PRIVATE SDL2::SDL2)
        target_compile_definitions(pspweb PRIVATE PSPWEB_USE_SDL=1)
    endif()
endif()

if(EMSCRIPTEN)
    configure_file(web/local_iso.js "${CMAKE_CURRENT_BINARY_DIR}/local_iso.js" COPYONLY)
    if(PSPWEB_PRELOAD_DIR STREQUAL "")
        message(FATAL_ERROR "PSPWEB_PRELOAD_DIR is required for the Emscripten build")
    endif()
    set_target_properties(pspweb PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
    # 128-bit SIMD lets the compiler vectorize the GE's vertex math (all current browsers support it).
    target_compile_options(pspweb PRIVATE -msimd128)
    target_compile_options(pspweb_at3 PRIVATE -msimd128)
    target_link_options(pspweb PRIVATE
        -O${PSPWEB_LINK_OPT}
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=8388608
        -fwasm-exceptions
        # The GE runs on a thread of its own with the canvas (OffscreenCanvas);
        # the page must be served cross-origin isolated (see scripts/serve.py).
        -pthread
        -sPTHREAD_POOL_SIZE=2
        -sDEFAULT_PTHREAD_STACK_SIZE=2097152
        -sOFFSCREENCANVAS_SUPPORT=1
        -lidbfs.js
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        "--preload-file=${PSPWEB_PRELOAD_DIR}@/game"
        "--shell-file=${PSPWEB_DIR}/web/shell.html")
    if(PSPWEB_PROFILING)
        target_link_options(pspweb PRIVATE --profiling-funcs)
    endif()
    set_property(TARGET pspweb APPEND PROPERTY LINK_DEPENDS "${PSPWEB_DIR}/web/shell.html")
    file(GLOB_RECURSE PSPWEB_PRELOAD_INPUTS CONFIGURE_DEPENDS "${PSPWEB_PRELOAD_DIR}/*")
    set_property(TARGET pspweb APPEND PROPERTY LINK_DEPENDS ${PSPWEB_PRELOAD_INPUTS})
endif()
