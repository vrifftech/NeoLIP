# Preserve the existing offline dependency setting for NeoLIP build scripts.
if(NEOLIP_MINIAUDIO_ROOT AND NOT NEOSHARED_MINIAUDIO_ROOT)
    set(NEOSHARED_MINIAUDIO_ROOT "${NEOLIP_MINIAUDIO_ROOT}" CACHE PATH "Offline miniaudio 0.11.23 source directory")
endif()
include("${NEOSHARED_ROOT}/cmake/NeoAudio.cmake")
if(NOT TARGET neolip_audio_codec)
    add_library(neolip_audio_codec ALIAS neoshared_audio_codec)
endif()
