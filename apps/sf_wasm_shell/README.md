# Browser build

WebAssembly build of the PsyCross backend: single thread, WebGL2, SDL2 input,
queued OpenAL audio, optional FFmpeg movies. Gameplay, disc, PSX and save code
are the desktop sources; the player supplies their own disc image in the page.

## Run

Serve the build over HTTP and open it in a browser with WebGL2 and WebAssembly
exception handling:

```sh
python3 -m http.server 8000 --directory build/wasm
```

Open `http://localhost:8000/sf_wasm_shell.html`. In the packaged ZIP the page is
`index.html`; extract it and serve that folder. `file://` does not work.

Select the USA v1.1 (`SCUS-94240`) `.cue` and the `.bin` files it lists, together
or one at a time; each appears on its CUE or BIN line, and the CUE shows which
BIN is still missing. Then **Launch**. The disc is read in the browser and is
not uploaded.

### Host

Upload `index.html`, `sf_wasm_shell.js`, `sf_wasm_shell.wasm` and
`sf_wasm_shell.data` together to a static HTTP/HTTPS site and serve `.wasm` as
`application/wasm`. No backend,
pthreads or cross-origin isolation headers are needed.

### Controls

Esc pauses and shows the menu under the game; **Resume** continues. At the title
Esc toggles the menu. Menus: W/S (A/D) navigate, F selects, Esc goes back; Enter
also confirms at the title and briefing.

| Action | Control |
| --- | --- |
| Move forward / backward | W / S |
| Turn left / right | A / D |
| Strafe left / right | Q / E |
| Run forward | Left Shift + W |
| Roll forward / sideways | Space / Q or E + Space |
| Toggle crouch / stealth | C |
| Turn 180 degrees | Backspace |
| Fire current weapon | Left mouse button |
| Manual first-person aim | Hold right mouse button, move mouse |
| Automatic target lock | Tab (hold to keep) |
| Reload | R |
| Interact / use / dismiss radio | F |
| Quick weapon switch | Middle mouse button |
| Previous / next weapon | [ / ] |
| Weapon ribbon | Mouse wheel |
| Quick weapon slots | 1–9, 0 (available weapons only) |
| Scope zoom in / out | F (or R) / Space while aiming |
| Frame counter (FPS, LOGIC, MS) | F6 |

Click the game during play to capture the mouse; that click does not fire. Esc
or losing focus releases capture and pauses; use **Resume** (clicking the paused
game does nothing). Menus do not capture the mouse.

### Saves

The campaign save (`SFPC_SAVE_V5`, five slots, 8 KiB) is written on the
mission-end save screen. The title host calls `Module.sfSaveTransfer.updated()`;
the page exports the file with `sf_export_save` and stores it in localStorage
(`sf-campaign-save`). At load it is written back with `sf_import_save`, where
the native loader validates it. Nothing is uploaded. Clearing site data removes
it. Mission checkpoints are not saves; **Options → Cheats → Stage select**
reaches later missions. Other settings are not kept across reloads.

## Build

Requires CMake, Ninja and Emscripten (6.0.12). Activate the SDK, then from the
repository root:

```sh
source /path/to/emsdk/emsdk_env.sh
cmake --preset wasm
cmake --build --preset wasm-shell -j 4
```

With movies, point `SF_WASM_FFMPEG_ROOT` at a static FFmpeg built with the same
SDK (`lib/libavformat.a` etc. and `include/`; configure options in
[FFMPEG-NOTICE.txt](FFMPEG-NOTICE.txt)):

```sh
cmake --preset wasm-media -DSF_WASM_FFMPEG_ROOT=/path/to/prefix
cmake --build --preset wasm-media-shell -j 4
```

Without movies the movie player and STR decoder are stubs and every movie is
skipped. Package the output with
`python3 tools/package_wasm.py --build-dir build/wasm`; the ZIP and its
SHA-256 file are written to the build directory.

## Layout

| Path | Role |
| --- | --- |
| `apps/sf_wasm_shell/shell.html` | Page: disc picker, menu, pointer-lock and save bridges |
| `apps/sf_wasm_shell/main.cpp` | Entry point: stages the disc, `sf_boot_game`, save import/export |
| `cmake/SfWasm.cmake`, `cmake/toolchain-emscripten.cmake`, `cmake/wasm-ports/` | Emscripten toolchain, ports and link settings |
| `src/media/str_decoder_stub.cpp`, `src/platform/psycross_movie_player_stub.cpp` | Movie stubs for `SF_ENABLE_MEDIA=OFF` |
| `tools/package_wasm.py` | Distribution ZIP |

Mission skybox images (`assets/skyboxes/*.bmp`) are preloaded into the in-memory
filesystem and shipped as `sf_wasm_shell.data`.

Page bridges: `Module.sfPointer` (pause/resume, pointer lock, title menu) and
`Module.sfSaveTransfer` (persists the campaign save). Exported functions:
`sf_boot_game`, `sf_import_save`, `sf_export_save`.

## Changes to shared sources

The desktop configuration is unchanged: `SF_ENABLE_MEDIA` defaults to ON outside
Emscripten, and every browser-only branch is behind `EMSCRIPTEN` /
`__EMSCRIPTEN__`.

- **Frame loop.** The title, mission-start, movie, scene and save flows are
  pollable sessions (`begin()` / `poll()`). Desktop `run()` loops over the same
  `poll()` calls; the browser drives them from `emscripten_set_main_loop`.
  `psycross_host.cpp`, `psycross_mission_start.*`, `psycross_movie_player.*`,
  `psycross_scene_viewer.*`, `psycross_scene_runtime.inc`.
- **Audio.** Larger queued buffers, WebAudio clock pacing and quiet diagnostics
  in the browser; OpenAL Soft callback extensions are desktop-only.
  `psycross_audio_output.*`, `PsyX_SPUAL.cpp`.
- **Rendering.** GLES3/WebGL2 shader headers, VRAM and depth formats; the SMAA
  and skybox passes also build for GLES3. `PsyX_render.h`, `PsyX_render.cpp`,
  `PsyX_smaa.inc`, `PsyX_skybox.inc`, `libgpu.h`, `libapi.h`.
- **Timer and input.** Browser interval timer instead of a sleeping thread;
  pointer lock owned by the page. `PsyX_main.cpp`,
  `psycross_runtime_guards.cpp`.
- **Build.** `C` language enabled; FFmpeg is also located through pkg-config
  when no CMake package exists; explicit `<algorithm>` in
  `raw_sector_source.cpp` and `mission.cpp`.
- **Movies.** `str_decoder.cpp` treats an `EIO` at end of stream as EOF, which
  FFmpeg 6.x reports for the STR demuxer.

## Limitations

Volumetric effects are written for desktop OpenGL and are disabled in the WebGL2
renderer (they are off by default on desktop); the original sprites are used.
