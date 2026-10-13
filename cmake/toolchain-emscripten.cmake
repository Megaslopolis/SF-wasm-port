# Emscripten toolchain entry point. Requires EMSDK (environment variable or
# -DEMSDK=<path>) to point at an activated SDK; delegates to Emscripten's own
# toolchain. Use `cmake --preset wasm` rather than `emcmake`, which would
# override this file. Port settings live in cmake/SfWasm.cmake.

if(DEFINED EMSDK AND NOT EMSDK STREQUAL "")
    set(_sf_emsdk_dir "${EMSDK}")
elseif(DEFINED ENV{EMSDK} AND NOT "$ENV{EMSDK}" STREQUAL "")
    set(_sf_emsdk_dir "$ENV{EMSDK}")
else()
    message(FATAL_ERROR
        "EMSDK is not set. Install the Emscripten SDK "
        "(https://emscripten.org/docs/getting_started/downloads.html), run "
        "'source <emsdk>/emsdk_env.sh', then re-configure.")
endif()

set(_sf_emscripten_toolchain
    "${_sf_emsdk_dir}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_sf_emscripten_toolchain}")
    message(FATAL_ERROR
        "Emscripten toolchain not found at ${_sf_emscripten_toolchain}. "
        "Re-run 'emsdk install' + 'emsdk activate' in ${_sf_emsdk_dir}.")
endif()

include("${_sf_emscripten_toolchain}")
