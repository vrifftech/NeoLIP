cmake_minimum_required(VERSION 3.18)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" FORCE)

project(NeoLIP VERSION 1.0.0 LANGUAGES CXX)
set(NEOSHARED_ROOT "" CACHE PATH
    "Path to the neoshared source repository; defaults to ../neoshared")

if(NOT NEOSHARED_ROOT AND DEFINED ENV{NEOSHARED_ROOT}
   AND NOT "$ENV{NEOSHARED_ROOT}" STREQUAL "")
    set(NEOSHARED_ROOT "$ENV{NEOSHARED_ROOT}" CACHE PATH
        "Path to the neoshared source repository" FORCE)
endif()

if(NOT NEOSHARED_ROOT)
    foreach(_neolip_shared_name IN ITEMS neoshared NeoShared)
        get_filename_component(_neolip_shared_candidate
            "${CMAKE_CURRENT_LIST_DIR}/../${_neolip_shared_name}" ABSOLUTE)
        if(EXISTS "${_neolip_shared_candidate}/CMakeLists.txt")
            set(NEOSHARED_ROOT "${_neolip_shared_candidate}")
            break()
        endif()
    endforeach()
    if(NOT NEOSHARED_ROOT)
        get_filename_component(NEOSHARED_ROOT
            "${CMAKE_CURRENT_LIST_DIR}/../neoshared" ABSOLUTE)
    endif()
else()
    get_filename_component(NEOSHARED_ROOT "${NEOSHARED_ROOT}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(NEOSHARED_ROOT "${NEOSHARED_ROOT}" CACHE PATH
    "Path to the neoshared source repository" FORCE)

if(NOT EXISTS "${NEOSHARED_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "neoshared was not found at '${NEOSHARED_ROOT}'. "
        "Clone the neoshared repository beside this repository, or configure "
        "with -DNEOSHARED_ROOT=/path/to/neoshared.")
endif()

if(MSVC)
    foreach(_neolip_runtime_flag_var IN ITEMS
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_neolip_runtime_flag_var})
            string(REGEX REPLACE "(^|[ /-])MDd?($|[ ]|/)" "\\1MT\\2" ${_neolip_runtime_flag_var} "${${_neolip_runtime_flag_var}}")
        endif()
    endforeach()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoWasm.cmake")
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()

set(NEOLIP_APP_VERSION_STRING "${PROJECT_VERSION}")
set(NEOLIP_APP_PHASE_STRING "0")

option(NEOLIP_BUILD_WX_GUI "Build the NeoLIP wxWidgets desktop application" ON)
option(NEOLIP_REQUIRE_WX_GUI "Fail configure if wxWidgets is unavailable while NEOLIP_BUILD_WX_GUI is ON" ON)
option(NEOLIP_BUILD_CLI "Build the optional command-line automation frontend (not packaged)" OFF)
option(NEOLIP_BUILD_TESTS "Build engine and native GUI regression tests (not packaged)" OFF)
if(NEOLIP_BUILD_TESTS)
    enable_testing()
endif()

# Backward-compatible aliases for older build scripts.
if(DEFINED CSLU_BUILD_WX_GUI)
    set(NEOLIP_BUILD_WX_GUI "${CSLU_BUILD_WX_GUI}" CACHE BOOL "Build the optional wxWidgets GUI frontend" FORCE)
endif()
if(DEFINED CSLU_REQUIRE_WX_GUI)
    set(NEOLIP_REQUIRE_WX_GUI "${CSLU_REQUIRE_WX_GUI}" CACHE BOOL "Require the optional wxWidgets GUI frontend" FORCE)
endif()

function(neolip_force_static_msvc_runtime target_name)
    if(MSVC AND TARGET ${target_name})
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target_name} PRIVATE /utf-8
            $<$<CONFIG:Debug>:/MTd>
            $<$<NOT:$<CONFIG:Debug>>:/MT>
        )
    endif()
endfunction()

if(NOT TARGET neoshared::dlg)
    message(FATAL_ERROR "NeoLIP dialogue batches require an updated NeoShared with neoshared::dlg. Update NEOSHARED_REF before building.")
endif()

include(cmake/NeoAudio.cmake)

set(NEOLIP_CORE_SOURCES
    src/tsv.cpp
    src/nnet45.cpp
    src/phn.cpp
    src/lip.cpp
    src/feature_context.cpp
    src/wav.cpp
    src/audio.cpp
    src/audio_codecs.cpp
    src/playback_core.cpp
    src/fingerprint.cpp
    src/review.cpp
    src/raw_frontend.cpp
    src/backend.cpp
    src/graph_build.cpp
    src/text_ntp.cpp
    src/transcript.cpp
)

set(NEOLIP_COMMON_DEFINITIONS
    CSLU_BUNDLED_DICTIONARY_DIR="dictionaries"
    CSLU_SOURCE_DOCS_DIR="docs"
    CSLU_APP_VERSION="${NEOLIP_APP_VERSION_STRING}"
    CSLU_APP_PHASE="${NEOLIP_APP_PHASE_STRING}"
)

add_library(neolip_core STATIC ${NEOLIP_CORE_SOURCES})
target_include_directories(neolip_core PUBLIC src)
target_link_libraries(neolip_core PUBLIC neolip_audio_codec)
target_compile_features(neolip_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(neolip_core PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_definitions(neolip_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(neolip_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
neolip_force_static_msvc_runtime(neolip_core)
neo_apply_minimal_release(neolip_core)


add_library(neolip_engine STATIC src/engine.cpp src/dialogue_batch.cpp)
add_library(neolip_playback STATIC src/playback_device.cpp)
target_link_libraries(neolip_playback PUBLIC neolip_core)
neolip_force_static_msvc_runtime(neolip_playback)
target_include_directories(neolip_engine PUBLIC src)
target_link_libraries(neolip_engine PUBLIC neolip_core neoshared::dlg neoshared::tlk)
target_compile_features(neolip_engine PUBLIC cxx_std_17)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(neolip_engine PUBLIC Threads::Threads)
endif()
if(MSVC)
    target_compile_options(neolip_engine PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_definitions(neolip_engine PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(neolip_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
neolip_force_static_msvc_runtime(neolip_engine)
neo_apply_minimal_release(neolip_engine)
if(NEOLIP_BUILD_CLI)
    add_executable(neolip_cli src/main.cpp)
    target_link_libraries(neolip_cli PRIVATE neolip_engine)
    target_compile_definitions(neolip_cli PRIVATE ${NEOLIP_COMMON_DEFINITIONS})
    neolip_force_static_msvc_runtime(neolip_cli)
endif()


# Runtime assets are installed only when the desktop application target exists.
# Core-only configurations must not produce an asset-only package.
set(NEOLIP_CSLU_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/cslu_runtime")
set(NEOLIP_CSLU_FA_DIR "${NEOLIP_CSLU_RUNTIME_DIR}/Toolkit/2.0/apps/fa")
set(NEOLIP_CSLU_CMU_DIR "${NEOLIP_CSLU_RUNTIME_DIR}/Toolkit/2.0/script/dictionary_1.0/cmu")
set(NEOLIP_CSLU_OGI_LEXICON "${NEOLIP_CSLU_RUNTIME_DIR}/Festival/1.4.2/festival/lib/dicts/ogi_lex.out")
set(NEOLIP_CSLU_OGI_ADDENDA "${NEOLIP_CSLU_RUNTIME_DIR}/Festival/1.4.2/festival/lib/ogi_lex_addenda.scm")
set(NEOLIP_BUNDLED_DICTIONARY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries")
set(NEOLIP_BUNDLED_CMU_DIR "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/cmu")
set(NEOLIP_BUNDLED_OGI_LEXICON "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/ogi_lex.out")
set(NEOLIP_BUNDLED_OGI_ADDENDA "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/ogi_lex_addenda.scm")

# The text aligner must never configure successfully with a partial dictionary
# bundle: missing data otherwise appears later as every transcript word being
# unknown. These are the pronunciation assets from the supplied CSLU Toolkit,
# augmented by NeoLIP's KOTOR-specific override.
set(NEOLIP_REQUIRED_DICTIONARY_ASSETS
    "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/custom_dictionary.txt"
    "${NEOLIP_BUNDLED_OGI_LEXICON}"
    "${NEOLIP_BUNDLED_OGI_ADDENDA}"
    "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/kotor_override.dict"
)
foreach(_neolip_letter IN ITEMS a b c d e f g h i j k l m n o p q r s t u v w x y z)
    list(APPEND NEOLIP_REQUIRED_DICTIONARY_ASSETS
        "${NEOLIP_BUNDLED_CMU_DIR}/${_neolip_letter}.ob")
endforeach()
foreach(_neolip_dictionary_asset IN LISTS NEOLIP_REQUIRED_DICTIONARY_ASSETS)
    if(NOT EXISTS "${_neolip_dictionary_asset}")
        message(FATAL_ERROR
            "NeoLIP's pronunciation dictionary bundle is incomplete: "
            "${_neolip_dictionary_asset}")
    endif()
endforeach()

# Direct Windows wxWidgets variables. These remain available for source-built wxWidgets trees.
set(NEOLIP_WX_ROOT_DIR "" CACHE PATH "Root of an installed wxWidgets tree, for example C:/wxWidgets-3.2.6")
set(NEOLIP_WX_LIB_DIR "" CACHE PATH "wxWidgets compiler-specific library directory, for example C:/wxWidgets-3.2.6/lib/vc_x64_dll")
set(NEOLIP_WX_CONFIGURATION "mswu" CACHE STRING "wxWidgets setup.h configuration directory, usually mswu, mswud, mswu_static, or mswud_static")
set(NEOLIP_WX_USE_STATIC "" CACHE STRING "Whether the installed wxWidgets libraries are static: ON or OFF")

function(neolip_find_required_wx_lib out_var)
    set(options)
    set(oneValueArgs DESCRIPTION)
    set(multiValueArgs NAMES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    find_library(_found_lib NAMES ${ARG_NAMES} PATHS "${NEOLIP_WX_LIB_DIR}" NO_DEFAULT_PATH)
    if(NOT _found_lib)
        message(FATAL_ERROR "Could not find ${ARG_DESCRIPTION} in '${NEOLIP_WX_LIB_DIR}'. Tried: ${ARG_NAMES}")
    endif()
    set(${out_var} "${_found_lib}" PARENT_SCOPE)
    unset(_found_lib CACHE)
endfunction()

function(neolip_find_optional_wx_lib out_var)
    set(options)
    set(oneValueArgs DESCRIPTION)
    set(multiValueArgs NAMES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    find_library(_found_lib NAMES ${ARG_NAMES} PATHS "${NEOLIP_WX_LIB_DIR}" NO_DEFAULT_PATH)
    if(_found_lib)
        set(${out_var} "${_found_lib}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
    unset(_found_lib CACHE)
endfunction()

function(neolip_link_wx target_name)
    target_compile_definitions(${target_name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    set(_wx_targets)
    foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wx::aui wxWidgets::wxWidgets)
        if(TARGET ${_wx_target})
            list(APPEND _wx_targets ${_wx_target})
        endif()
    endforeach()

    if(_wx_targets)
        target_link_libraries(${target_name} PRIVATE ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PRIVATE ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PRIVATE ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PRIVATE ${wxWidgets_LIBRARIES})
    endif()

    neolip_force_static_msvc_runtime(${target_name})
endfunction()

if(NEOLIP_BUILD_WX_GUI)

    set(NEOLIP_GUI_WX_INCLUDE_DIRS "")
    set(NEOLIP_GUI_WX_LIBRARIES "")
    set(NEOLIP_GUI_WX_DEFINITIONS "")
    set(NEOLIP_GUI_WX_MANUAL OFF)
    set(NEOLIP_GUI_WX_FOUND OFF)

    if(WIN32 AND NEOLIP_WX_ROOT_DIR AND NEOLIP_WX_LIB_DIR)
        set(_neolip_wx_include_dir "${NEOLIP_WX_ROOT_DIR}/include")
        set(_neolip_wx_setup_dir "${NEOLIP_WX_LIB_DIR}/${NEOLIP_WX_CONFIGURATION}")
        if(NOT EXISTS "${_neolip_wx_include_dir}/wx/wx.h")
            message(FATAL_ERROR "wxWidgets header not found: ${_neolip_wx_include_dir}/wx/wx.h")
        endif()
        if(NOT EXISTS "${_neolip_wx_setup_dir}/wx/setup.h")
            message(FATAL_ERROR "wxWidgets setup.h not found: ${_neolip_wx_setup_dir}/wx/setup.h. Check NEOLIP_WX_LIB_DIR and NEOLIP_WX_CONFIGURATION.")
        endif()

        set(NEOLIP_GUI_WX_MANUAL ON)
        set(NEOLIP_GUI_WX_FOUND ON)
        set(NEOLIP_GUI_WX_INCLUDE_DIRS "${_neolip_wx_setup_dir}" "${_neolip_wx_include_dir}")

        string(TOLOWER "${NEOLIP_WX_CONFIGURATION}" _neolip_wx_cfg_lower)
        set(_neolip_wx_debug_suffix "")
        if(_neolip_wx_cfg_lower MATCHES "d")
            set(_neolip_wx_debug_suffix "d")
        endif()

        if(NEOLIP_WX_USE_STATIC STREQUAL "")
            if(NEOLIP_WX_LIB_DIR MATCHES "_lib$" OR _neolip_wx_cfg_lower MATCHES "static")
                set(NEOLIP_WX_USE_STATIC ON)
            else()
                set(NEOLIP_WX_USE_STATIC OFF)
            endif()
        endif()

        neolip_find_required_wx_lib(NEOLIP_WX_BASE_LIB DESCRIPTION "wxWidgets base library"
            NAMES wxbase33u${_neolip_wx_debug_suffix} wxbase32u${_neolip_wx_debug_suffix} wxbase31u${_neolip_wx_debug_suffix} wxbase30u${_neolip_wx_debug_suffix})
        neolip_find_required_wx_lib(NEOLIP_WX_CORE_LIB DESCRIPTION "wxWidgets MSW core library"
            NAMES wxmsw33u${_neolip_wx_debug_suffix}_core wxmsw32u${_neolip_wx_debug_suffix}_core wxmsw31u${_neolip_wx_debug_suffix}_core wxmsw30u${_neolip_wx_debug_suffix}_core)
        neolip_find_required_wx_lib(NEOLIP_WX_AUI_LIB DESCRIPTION "wxWidgets MSW AUI library"
            NAMES wxmsw33u${_neolip_wx_debug_suffix}_aui wxmsw32u${_neolip_wx_debug_suffix}_aui wxmsw31u${_neolip_wx_debug_suffix}_aui wxmsw30u${_neolip_wx_debug_suffix}_aui)
        neolip_find_optional_wx_lib(NEOLIP_WX_ADV_LIB DESCRIPTION "wxWidgets MSW adv library"
            NAMES wxmsw33u${_neolip_wx_debug_suffix}_adv wxmsw32u${_neolip_wx_debug_suffix}_adv wxmsw31u${_neolip_wx_debug_suffix}_adv wxmsw30u${_neolip_wx_debug_suffix}_adv)

        set(NEOLIP_GUI_WX_LIBRARIES ${NEOLIP_WX_AUI_LIB} ${NEOLIP_WX_CORE_LIB} ${NEOLIP_WX_BASE_LIB})
        if(NEOLIP_WX_ADV_LIB)
            list(INSERT NEOLIP_GUI_WX_LIBRARIES 0 ${NEOLIP_WX_ADV_LIB})
        endif()
        set(NEOLIP_GUI_WX_DEFINITIONS __WXMSW__ UNICODE _UNICODE)
        if(NOT NEOLIP_WX_USE_STATIC)
            list(APPEND NEOLIP_GUI_WX_DEFINITIONS WXUSINGDLL)
        else()
            foreach(_wx_extra wxregexu${_neolip_wx_debug_suffix} wxpng wxjpeg wxtiff wxzlib wxexpat)
                find_library(_wx_extra_lib NAMES ${_wx_extra} PATHS "${NEOLIP_WX_LIB_DIR}" NO_DEFAULT_PATH)
                if(_wx_extra_lib)
                    list(APPEND NEOLIP_GUI_WX_LIBRARIES ${_wx_extra_lib})
                endif()
                unset(_wx_extra_lib CACHE)
            endforeach()
            list(APPEND NEOLIP_GUI_WX_LIBRARIES
                comctl32 rpcrt4 ws2_32 winmm version shlwapi comdlg32 ole32 oleaut32 uuid uxtheme imm32 setupapi gdiplus)
        endif()

        message(STATUS "Using direct wxWidgets paths for NeoLIP GUI")
        message(STATUS "wxWidgets root: ${NEOLIP_WX_ROOT_DIR}")
        message(STATUS "wxWidgets lib dir: ${NEOLIP_WX_LIB_DIR}")
        message(STATUS "wxWidgets config: ${NEOLIP_WX_CONFIGURATION}")
        message(STATUS "wxWidgets static: ${NEOLIP_WX_USE_STATIC}")
    endif()

    if(NOT NEOLIP_GUI_WX_MANUAL)
        find_package(wxWidgets CONFIG QUIET COMPONENTS core base adv aui)
        if(NOT wxWidgets_FOUND)
            find_package(wxWidgets QUIET COMPONENTS core base adv aui)
        endif()
        if(wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets)
            set(NEOLIP_GUI_WX_FOUND ON)
        endif()
    endif()

    if(NEOLIP_GUI_WX_FOUND)
        set(NEOLIP_WX_SOURCES src/gui/wx_main.cpp)
        if(WIN32)
            list(APPEND NEOLIP_WX_SOURCES resources/neolip.rc)
        endif()

        if(WIN32)
            add_executable(NeoLIP WIN32 ${NEOLIP_WX_SOURCES})
        elseif(APPLE)
            add_executable(NeoLIP MACOSX_BUNDLE ${NEOLIP_WX_SOURCES})
        else()
            add_executable(NeoLIP ${NEOLIP_WX_SOURCES})
        endif()
        neo_configure_macos_bundle(NeoLIP
            NAME "NeoLIP"
            IDENTIFIER "com.vrifftech.neolip"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neolip.icns"
        )
        neo_configure_wasm_target(NeoLIP
            NAME "NeoLIP"
            SLUG "neolip"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neolip.svg"
            PRELOAD
                "${NEOLIP_BUNDLED_DICTIONARY_ROOT}@/dictionaries"
                "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.train.spec@/models/phonemes_new_16k.train.spec"
                "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.parts@/models/phonemes_new_16k.parts"
                "${NEOLIP_CSLU_FA_DIR}/bc_16k@/models/bc_16k"
                "${NEOLIP_CSLU_FA_DIR}/trans_16k@/models/trans_16k"
        )
        target_include_directories(NeoLIP PRIVATE src)
        target_compile_definitions(NeoLIP PRIVATE ${NEOLIP_COMMON_DEFINITIONS})
        target_link_libraries(NeoLIP PRIVATE neolip_engine neoshared::wx)
        if(NEOLIP_GUI_WX_MANUAL)
            target_include_directories(NeoLIP SYSTEM PRIVATE ${NEOLIP_GUI_WX_INCLUDE_DIRS})
            target_compile_definitions(NeoLIP PRIVATE ${NEOLIP_GUI_WX_DEFINITIONS})
            target_link_libraries(NeoLIP PRIVATE ${NEOLIP_GUI_WX_LIBRARIES})
        else()
            neolip_link_wx(NeoLIP)
        endif()
        target_compile_features(NeoLIP PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(NeoLIP PRIVATE /W4 /permissive- /Zc:__cplusplus)
            target_compile_definitions(NeoLIP PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
        else()
            target_compile_options(NeoLIP PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        neolip_force_static_msvc_runtime(NeoLIP)
        neo_apply_minimal_release(NeoLIP)
        if(APPLE)
            set(_neolip_build_assets "$<TARGET_BUNDLE_DIR:NeoLIP>/Contents/Resources")
        else()
            set(_neolip_build_assets "$<TARGET_FILE_DIR:NeoLIP>")
        endif()
        if(NOT EMSCRIPTEN)
            add_custom_command(TARGET NeoLIP POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${NEOLIP_BUNDLED_DICTIONARY_ROOT}" "${_neolip_build_assets}/dictionaries"
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${NEOLIP_CSLU_FA_DIR}/bc_16k" "${_neolip_build_assets}/models/bc_16k"
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${NEOLIP_CSLU_FA_DIR}/trans_16k" "${_neolip_build_assets}/models/trans_16k"
                COMMAND "${CMAKE_COMMAND}" -E copy
                    "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.train.spec"
                    "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.parts"
                    "${_neolip_build_assets}/models/"
                VERBATIM)
        endif()
        target_link_libraries(NeoLIP PRIVATE neolip_playback)
        if(NEOLIP_BUILD_TESTS AND NOT EMSCRIPTEN)
            # Separate executable: no test switch or test code enters NeoLIP.
            if(WIN32)
                add_executable(neolip_ui_tests WIN32 src/gui/wx_main.cpp)
            else()
                add_executable(neolip_ui_tests src/gui/wx_main.cpp)
            endif()
            foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
                get_target_property(_value NeoLIP ${_property})
                if(_value)
                    set_property(TARGET neolip_ui_tests PROPERTY ${_property} "${_value}")
                endif()
            endforeach()
            target_compile_definitions(neolip_ui_tests PRIVATE NEOLIP_UI_SMOKE_TEST=1
                NEOLIP_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                NEOLIP_TEST_APP_EXECUTABLE="$<TARGET_FILE:NeoLIP>")
            neolip_force_static_msvc_runtime(neolip_ui_tests)
            if(UNIX AND NOT APPLE)
                find_program(NEOLIP_XVFB_RUN xvfb-run)
                if(NEOLIP_XVFB_RUN)
                    add_test(NAME neolip_ui_tests COMMAND "${NEOLIP_XVFB_RUN}" -a
                        -s "-screen 0 1400x1000x24" $<TARGET_FILE:neolip_ui_tests>)
                else()
                    message(STATUS "Install xvfb and xauth to run the native GUI smoke test")
                endif()
            else()
                add_test(NAME neolip_ui_tests COMMAND neolip_ui_tests)
            endif()
            if(TEST neolip_ui_tests)
                set_tests_properties(neolip_ui_tests PROPERTIES TIMEOUT 180)
            endif()
        endif()
        if(WIN32)
            set(NEOLIP_RUNTIME_DESTINATION ".")
        else()
            set(NEOLIP_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
        endif()
        install(TARGETS NeoLIP
            BUNDLE DESTINATION .
            RUNTIME DESTINATION "${NEOLIP_RUNTIME_DESTINATION}")
        if(APPLE)
            set(NEOLIP_DATA_PREFIX "NeoLIP.app/Contents/Resources/")
            set(NEOLIP_DATA_ROOT "NeoLIP.app/Contents/Resources")
        else()
            set(NEOLIP_DATA_PREFIX "")
            set(NEOLIP_DATA_ROOT ".")
        endif()

        if(UNIX AND NOT APPLE)
            foreach(NEOLIP_ICON_SIZE 16 24 32 48 64 128 256 512)
                install(FILES resources/icons/${NEOLIP_ICON_SIZE}x${NEOLIP_ICON_SIZE}/neolip.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEOLIP_ICON_SIZE}x${NEOLIP_ICON_SIZE}/apps)
            endforeach()
            install(FILES resources/neolip.svg
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
            configure_file(resources/neolip.desktop.in
                ${CMAKE_CURRENT_BINARY_DIR}/neolip.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neolip.desktop
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
        endif()

        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/licenses/miniaudio.txt"
            DESTINATION "${NEOLIP_DATA_PREFIX}licenses")
        install(DIRECTORY "${NEOLIP_BUNDLED_DICTIONARY_ROOT}/"
            DESTINATION "${NEOLIP_DATA_PREFIX}dictionaries")
        install(FILES
            "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.train.spec"
            "${NEOLIP_CSLU_FA_DIR}/phonemes_new_16k.parts"
            DESTINATION "${NEOLIP_DATA_PREFIX}models")
        install(DIRECTORY
            "${NEOLIP_CSLU_FA_DIR}/bc_16k"
            "${NEOLIP_CSLU_FA_DIR}/trans_16k"
            DESTINATION "${NEOLIP_DATA_PREFIX}models")

    elseif(NEOLIP_REQUIRE_WX_GUI)
        message(FATAL_ERROR "wxWidgets was not found, but NEOLIP_REQUIRE_WX_GUI is ON. On Windows, use the pinned wxWidgets 3.3.3 overlay from neoshared; on Debian or Ubuntu, install libwxgtk3.2-dev")
    else()
        message(STATUS "wxWidgets not found; skipping NeoLIP desktop application because NEOLIP_REQUIRE_WX_GUI is OFF")
    endif()
endif()

set(CPACK_PACKAGE_NAME "NeoLIP")
set(CPACK_PACKAGE_VENDOR "Unofficial fan project")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "NeoLIP: GUI for CSLU-compatible KOTOR LIP generation")
set(CPACK_PACKAGE_FILE_NAME "NeoLIP-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_GENERATOR "ZIP")
if(TARGET NeoLIP)
    include(CPack)
else()
    message(STATUS "NeoLIP package generation is disabled because the desktop application target was not built")
endif()


# Browser media/export only. The same engine is linked directly on every target.
if(EMSCRIPTEN AND TARGET NeoLIP)
    target_sources(NeoLIP PRIVATE wasm/neolip_wasm_runtime.cpp)
    target_include_directories(NeoLIP PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/wasm")
    target_link_options(NeoLIP PRIVATE
        "-sASYNCIFY=1" "-sASYNCIFY_STACK_SIZE=1048576")
endif()

if(NEOLIP_BUILD_TESTS)
    enable_testing()
    add_executable(neolip_engine_tests tests/engine_tests.cpp)
    target_link_libraries(neolip_engine_tests PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_engine_tests)
    add_test(NAME neolip_engine_tests COMMAND neolip_engine_tests "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NEOLIP_BUILD_TESTS)
    add_executable(neolip_lip_tests tests/lip_tests.cpp)
    target_link_libraries(neolip_lip_tests PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_lip_tests)
    add_test(NAME neolip_lip_tests COMMAND neolip_lip_tests)
endif()

if(NEOLIP_BUILD_TESTS)
    add_executable(neolip_polish_tests tests/polish_tests.cpp)
    target_link_libraries(neolip_polish_tests PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_polish_tests)
    add_test(NAME neolip_polish_tests COMMAND neolip_polish_tests "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NEOLIP_BUILD_TESTS)
    add_executable(neolip_workflow_tests tests/workflow_tests.cpp)
    target_link_libraries(neolip_workflow_tests PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_workflow_tests)
    add_test(NAME neolip_workflow_tests COMMAND neolip_workflow_tests "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NEOLIP_BUILD_TESTS AND NOT EMSCRIPTEN)
    # Compile the real device integration against the pinned miniaudio header.
    # The null output requires no sound card on hosted CI and is not shipped.
    add_executable(neolip_device_tests tests/device_tests.cpp src/playback_device.cpp)
    target_compile_definitions(neolip_device_tests PRIVATE NEOLIP_AUDIO_TEST_NULL_DEVICE=1)
    target_link_libraries(neolip_device_tests PRIVATE neolip_engine neolip_audio_codec)
    neolip_force_static_msvc_runtime(neolip_device_tests)
    add_test(NAME neolip_device_tests COMMAND neolip_device_tests)
    set_tests_properties(neolip_device_tests PROPERTIES TIMEOUT 20)
endif()

option(NEOLIP_BUILD_BENCHMARKS "Build non-installed engineering comparison tools" OFF)
if(NEOLIP_BUILD_BENCHMARKS)
    add_executable(neolip_quality_benchmark tests/quality_benchmark.cpp)
    target_link_libraries(neolip_quality_benchmark PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_quality_benchmark)
endif()

if(NEOLIP_BUILD_TESTS)
    add_executable(neolip_dialogue_tests tests/dialogue_tests.cpp)
    target_link_libraries(neolip_dialogue_tests PRIVATE neolip_engine)
    neolip_force_static_msvc_runtime(neolip_dialogue_tests)
    add_test(NAME neolip_dialogue_tests COMMAND neolip_dialogue_tests "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
