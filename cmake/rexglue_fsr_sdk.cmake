# rexglue_fsr_sdk.cmake — Optional AMD FSR SDK (FSR 4 / FSR 3.1.5) for the 3D
# scene, D3D12 only
#
# Expects REXGLUE_ENABLE_FSR_SDK to be set before inclusion. Uses AMD's
# FidelityFX SDK v2.3.0 files from REXGLUE_FSR_SDK_DIR - a folder laid out like
# the SDK's Kits/FidelityFX (api/include, upscalers/include, signedbin,
# docs/license.md) - or downloads exactly those files (pinned commit, checked
# hashes). Only the headers and AMD's signed DLLs are used: the loader DLL is
# loaded at runtime (no import library), and it loads the upscaler DLL from the
# executable's folder. The files are under AMD's license (docs/license.md), not
# this project's: they are only fetched, never committed.
#
# This is separate from rexglue_fidelityfx.cmake (the FidelityFX SDK built from
# source for the presenter's FSR): the two never meet in one module.
#
# On success, sets REXGLUE_FSR_SDK_INCLUDE_DIR (the folder with api/ and
# upscalers/), REXGLUE_FSR_SDK_RUNTIME_DLLS (copied next to executables by
# rexglue_configure_target) and REXGLUE_FSR_SDK_LICENSE.

unset(REXGLUE_FSR_SDK_INCLUDE_DIR CACHE)
unset(REXGLUE_FSR_SDK_RUNTIME_DLLS CACHE)
unset(REXGLUE_FSR_SDK_LICENSE CACHE)

if(NOT REXGLUE_ENABLE_FSR_SDK)
    return()
endif()

if(NOT WIN32 OR NOT REXGLUE_USE_D3D12)
    message(WARNING "REXGLUE_ENABLE_FSR_SDK needs Windows and the D3D12 backend - disabling the FSR SDK.")
    set(REXGLUE_ENABLE_FSR_SDK OFF CACHE BOOL "" FORCE)
    return()
endif()

set(REXGLUE_FSR_SDK_DIR "" CACHE PATH
    "AMD FidelityFX SDK v2.3.0 Kits/FidelityFX folder (api, upscalers, signedbin, docs); empty = download the needed files")

# AMD FidelityFX SDK v2.3.0: relative path, SHA-256.
set(_rexglue_fsr_files
    "api/include/ffx_api.h|e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"
    "api/include/ffx_api_types.h|cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"
    "api/include/dx12/ffx_api_dx12.h|c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"
    "upscalers/include/ffx_upscale.h|65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"
    "signedbin/amd_fidelityfx_loader_dx12.dll|e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"
    "signedbin/amd_fidelityfx_upscaler_dx12.dll|d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"
    "docs/license.md|6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963"
)

if(NOT REXGLUE_FSR_SDK_DIR)
    set(_rexglue_fsr_commit 60f4ea81909200d8542eca14dccb2628b763a9a3)
    set(_rexglue_fsr_base
        "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_rexglue_fsr_commit}/Kits/FidelityFX")
    set(_rexglue_fsr_sdk "${CMAKE_BINARY_DIR}/_deps/amd_fsr_sdk")
    foreach(_rexglue_fsr_entry ${_rexglue_fsr_files})
        string(REPLACE "|" ";" _rexglue_fsr_parts "${_rexglue_fsr_entry}")
        list(GET _rexglue_fsr_parts 0 _rexglue_fsr_file)
        list(GET _rexglue_fsr_parts 1 _rexglue_fsr_hash)
        set(_rexglue_fsr_path "${_rexglue_fsr_sdk}/${_rexglue_fsr_file}")
        if(EXISTS "${_rexglue_fsr_path}")
            file(SHA256 "${_rexglue_fsr_path}" _rexglue_fsr_existing_hash)
            if(_rexglue_fsr_existing_hash STREQUAL _rexglue_fsr_hash)
                continue()
            endif()
        endif()
        message(STATUS "Downloading AMD FSR SDK file ${_rexglue_fsr_file}")
        file(DOWNLOAD "${_rexglue_fsr_base}/${_rexglue_fsr_file}" "${_rexglue_fsr_path}"
             EXPECTED_HASH SHA256=${_rexglue_fsr_hash} STATUS _rexglue_fsr_status)
        list(GET _rexglue_fsr_status 0 _rexglue_fsr_status_code)
        if(NOT _rexglue_fsr_status_code EQUAL 0)
            message(WARNING "Failed to download ${_rexglue_fsr_file} (${_rexglue_fsr_status}) - disabling the FSR SDK.")
            set(REXGLUE_ENABLE_FSR_SDK OFF CACHE BOOL "" FORCE)
            return()
        endif()
    endforeach()
else()
    set(_rexglue_fsr_sdk "${REXGLUE_FSR_SDK_DIR}")
    foreach(_rexglue_fsr_entry ${_rexglue_fsr_files})
        string(REPLACE "|" ";" _rexglue_fsr_parts "${_rexglue_fsr_entry}")
        list(GET _rexglue_fsr_parts 0 _rexglue_fsr_file)
        list(GET _rexglue_fsr_parts 1 _rexglue_fsr_hash)
        if(NOT EXISTS "${_rexglue_fsr_sdk}/${_rexglue_fsr_file}")
            message(WARNING "AMD FSR SDK file ${_rexglue_fsr_sdk}/${_rexglue_fsr_file} is missing - disabling the FSR SDK.")
            set(REXGLUE_ENABLE_FSR_SDK OFF CACHE BOOL "" FORCE)
            return()
        endif()
        file(SHA256 "${_rexglue_fsr_sdk}/${_rexglue_fsr_file}" _rexglue_fsr_existing_hash)
        if(NOT _rexglue_fsr_existing_hash STREQUAL _rexglue_fsr_hash)
            message(WARNING "AMD FSR SDK file ${_rexglue_fsr_file} isn't the v2.3.0 one - the code expects that version.")
        endif()
    endforeach()
endif()

set(REXGLUE_FSR_SDK_INCLUDE_DIR "${_rexglue_fsr_sdk}" CACHE INTERNAL "AMD FSR SDK headers (api/, upscalers/)")
set(REXGLUE_FSR_SDK_RUNTIME_DLLS
    "${_rexglue_fsr_sdk}/signedbin/amd_fidelityfx_loader_dx12.dll"
    "${_rexglue_fsr_sdk}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    CACHE INTERNAL "AMD FSR SDK runtime DLLs")
set(REXGLUE_FSR_SDK_LICENSE "${_rexglue_fsr_sdk}/docs/license.md" CACHE INTERNAL "AMD FSR SDK license")
message(STATUS "AMD FSR SDK: ${_rexglue_fsr_sdk}")
