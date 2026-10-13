# SfWasm: Emscripten / WebAssembly settings, included from the top-level
# CMakeLists when EMSCRIPTEN is set.
#   - Emscripten ports replace the desktop SDL2, OpenAL and OpenGL packages.
#   - Movie decoding needs SF_ENABLE_MEDIA=ON and SF_WASM_FFMPEG_ROOT;
#     otherwise movies are stubbed.
#   - Builds are single-threaded; no COOP/COEP headers are required.

# Disc parsing and save recovery rely on C++ catch handlers. Apply this before
# creating every target so exceptions can cross library boundaries.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fwasm-exceptions>")
add_link_options("-fwasm-exceptions")

# external/PsyCross calls find_package(SDL2/OPENAL) in module mode. Prefer config
# mode and point it at the shims in cmake/wasm-ports; set before
# add_subdirectory(external/PsyCross) so the vendored calls resolve to them.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
set(SDL2_DIR "${CMAKE_CURRENT_LIST_DIR}/wasm-ports")
set(OPENAL_DIR "${CMAKE_CURRENT_LIST_DIR}/wasm-ports")

# Port targets under the names the desktop build links.
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED)
    target_compile_options(SDL2::SDL2 INTERFACE "-sUSE_SDL=2")
    target_link_options(SDL2::SDL2 INTERFACE "-sUSE_SDL=2")
endif()
if(NOT TARGET OpenAL::OpenAL)
    add_library(OpenAL::OpenAL INTERFACE IMPORTED)
    target_link_libraries(OpenAL::OpenAL INTERFACE "-lopenal")
endif()
if(NOT TARGET OpenGL::GL)
    add_library(OpenGL::GL INTERFACE IMPORTED)
    target_link_options(OpenGL::GL INTERFACE
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2" "-sFULL_ES3=1")
endif()

# Link settings for a final WASM executable: heap growth (disc images are
# large), the filesystem layer for staged disc files, and the SDL2/WebGL2 ports.
function(sf_target_wasm_executable target)
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    target_link_options(${target} PRIVATE
        "--profiling-funcs"
        # Guest bridge snapshots and renderer scratch exceed the SDK's 64 KiB
        # default C++ stack. Keep explicit bounds checks in the browser build.
        "-sSTACK_SIZE=4194304"
        "-sSTACK_OVERFLOW_CHECK=2"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sFORCE_FILESYSTEM=1"
        "-sUSE_SDL=2"
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2")
endfunction()
