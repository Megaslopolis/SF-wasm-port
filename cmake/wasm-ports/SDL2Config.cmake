# Emscripten SDL2 port shim.
#
# Resolves the module-mode call `find_package(SDL2 REQUIRED)` made by
# external/PsyCross via config mode (CMAKE_FIND_PACKAGE_PREFER_CONFIG, set in
# SfWasm.cmake) without touching the vendored build file or shadowing CMake's
# builtin FindSDL2 module on desktop.
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "")
set(SDL2_LIBRARIES "-sUSE_SDL=2")
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    target_compile_options(SDL2::SDL2 INTERFACE "-sUSE_SDL=2")
    target_link_options(SDL2::SDL2 INTERFACE "-sUSE_SDL=2")
endif()
