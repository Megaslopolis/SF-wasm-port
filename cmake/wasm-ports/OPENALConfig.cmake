# Emscripten OpenAL port shim (WebAudio-backed `-lopenal`).
#
# Resolves the module-mode call `find_package(OPENAL REQUIRED)` made by
# external/PsyCross via config mode (CMAKE_FIND_PACKAGE_PREFER_CONFIG, set in
# SfWasm.cmake) without touching the vendored build file. Mirrors the variable
# contract of cmake/FindOPENAL.cmake (used on desktop) so PsyCross's
# `target_include_directories(... ${OpenAL_BINARY_DIR} ...)` stays valid.
set(OPENAL_FOUND TRUE)
set(OPENAL_INCLUDE_DIR "")
set(OPENAL_LIBRARY "-lopenal")
set(OpenAL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(OpenAL_BINARY_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT TARGET OpenAL)
    add_library(OpenAL INTERFACE IMPORTED)
    target_link_libraries(OpenAL INTERFACE "-lopenal")
endif()
