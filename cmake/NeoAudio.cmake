# miniaudio is built into the application; it is not downloaded by end users.
# An offline source checkout may be supplied at configure time.
set(NEOLIP_MINIAUDIO_ROOT "" CACHE PATH "Offline miniaudio 0.11.23 source directory")
if(NOT NEOLIP_MINIAUDIO_ROOT)
    include(FetchContent)
    FetchContent_Declare(neolip_miniaudio_source
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG f40cf03f80cdb7e741d43e53b7e706e8c1394bcf
        GIT_PROGRESS FALSE
        SOURCE_SUBDIR neolip-do-not-build-upstream-examples)
    FetchContent_MakeAvailable(neolip_miniaudio_source)
    set(NEOLIP_MINIAUDIO_ROOT "${neolip_miniaudio_source_SOURCE_DIR}")
endif()
if(NOT EXISTS "${NEOLIP_MINIAUDIO_ROOT}/miniaudio.h")
    message(FATAL_ERROR "miniaudio source is missing; the product cannot be built without its built-in audio decoder.")
endif()
file(READ "${NEOLIP_MINIAUDIO_ROOT}/miniaudio.h" _neolip_audio_version LIMIT 2048)
if(NOT _neolip_audio_version MATCHES "v0\\.11\\.23")
    message(FATAL_ERROR "NeoLIP requires miniaudio 0.11.23; use the pinned source version.")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/neolip_miniaudio.cpp" "#define MINIAUDIO_IMPLEMENTATION\n#include \"miniaudio.h\"\n")
add_library(neolip_audio_codec STATIC "${CMAKE_CURRENT_BINARY_DIR}/neolip_miniaudio.cpp")
target_include_directories(neolip_audio_codec SYSTEM PUBLIC "${NEOLIP_MINIAUDIO_ROOT}")
target_compile_definitions(neolip_audio_codec PUBLIC
    MA_NO_RESOURCE_MANAGER MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_GENERATION MA_NO_ENCODING MA_NO_FLAC)
if(EMSCRIPTEN)
    # Use miniaudio's native Emscripten/WebAudio backend and its own threading
    # stubs. MA_NO_THREADING disables device APIs; never combine it with ma_device.
    # No AudioWorklets, WASM workers, SharedArrayBuffer, or application pthreads.
    target_compile_options(neolip_audio_codec PRIVATE -Wno-deprecated-declarations)
elseif(WIN32)
    target_link_libraries(neolip_audio_codec PUBLIC ole32 uuid winmm)
elseif(APPLE)
    target_link_libraries(neolip_audio_codec PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(neolip_audio_codec PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
# Third-party warnings must not make our -Werror project checks compiler-specific.
if(MSVC)
    target_compile_options(neolip_audio_codec PRIVATE /w)
else()
    target_compile_options(neolip_audio_codec PRIVATE -w)
endif()
neolip_force_static_msvc_runtime(neolip_audio_codec)
neo_apply_minimal_release(neolip_audio_codec)
