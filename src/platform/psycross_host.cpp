#include "sf/platform/host.hpp"

#include "psycross_audio_output.hpp"
#include "psycross_mission_start.hpp"
#include "psycross_movie_player.hpp"
#include "psycross_scene_viewer.hpp"
#include "volumetric_atlas_texture.hpp"
#include "psycross_video_mode.hpp"
#include "psycross_window_mode.hpp"

#include "sf/core/error.hpp"
#include "sf/game/campaign.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/game/title.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>
#include <psx/libpad.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sf::platform {
namespace {

#ifdef __EMSCRIPTEN__
void stopBrowserSession() {
  EM_ASM({
    sfSetStatus('Session stopped. Reload to retry.', 'error');
    if (Module['sfPointer']) { Module['sfPointer'].setScene(false, false); }
  });
  emscripten_cancel_main_loop();
}

template <typename Step> void pumpBrowserFrame(Step &&step) {
  try {
    if (step()) {
      emscripten_cancel_main_loop();
    }
  } catch (const std::exception &error) {
    std::cerr << "Session stopped: " << error.what() << '\n';
    stopBrowserSession();
  } catch (...) {
    std::cerr << "Session stopped: unknown error\n";
    stopBrowserSession();
  }
}
#endif

detail::StandaloneMovieSkipPolicy
endingMovieSkipPolicy(const game::MissionDefinition &definition) noexcept {
  const auto catalog = game::missionCatalog();
  if (!catalog.empty() && definition.index == catalog.back().index) {
    // EOL/SILO.STR contains the credits and the post-credits scene.  It is a
    // single retail stream, so allowing a carried confirm/cancel edge from the
    // save menu to skip it loses the entire campaign ending.
    return detail::StandaloneMovieSkipPolicy::prevent;
  }
  return detail::StandaloneMovieSkipPolicy::allow;
}

void configureGraphics(const GraphicsSettings &settings) noexcept {
  // SMAA and MSAA are complete scene antialiasing alternatives. Keeping them
  // exclusive avoids paying twice and gives SMAA an ordinary single-sample
  // depth texture for geometry-aware edge detection.
  g_cfg_msaaSamples = settings.smaa ? 0 : settings.msaa_samples;
  g_cfg_bilinearFiltering = settings.bilinear_filtering ? 1 : 0;
  g_cfg_trilinearFiltering = settings.trilinear_filtering ? 1 : 0;
  g_cfg_anisotropicFiltering = settings.anisotropic_filtering ? 1 : 0;
  g_cfg_smaa = settings.smaa ? 1 : 0;
  g_cfg_volumetricEffects = settings.volumetric_effects ? 1 : 0;
  g_cfg_aspectMode = settings.aspect_ratio == AspectRatioMode::adaptive
                         ? PSYX_ASPECT_ADAPTIVE
                         : PSYX_ASPECT_ORIGINAL_4_3;
  g_cfg_renderWidth = std::max(settings.width, 1);
  g_cfg_renderHeight = std::max(settings.height, 1);
  g_cfg_swapInterval = settings.vsync ? 1 : 0;
  // Presentation is native: no game code samples the displayed framebuffer
  // through PSX VRAM, and the guest simulation has its own deterministic
  // 20 Hz clock. Avoid the legacy readback and busy VBlank compatibility
  // paths, both of which otherwise steal time from display-refresh pacing.
  g_cfg_framebufferFeedback = 0;
  g_cfg_vblankThread = 0;
}

void configureControllerProtocol(ControllerProtocol protocol) noexcept {
  const auto set = [](const char *name, bool enabled) {
    static_cast<void>(
        SDL_SetHintWithPriority(name, enabled ? "1" : "0", SDL_HINT_OVERRIDE));
  };

  // Fix the Windows joystick driver set before SDL initializes it. Forced
  // modes disable competing drivers so one physical pad is opened once.
  const auto automatic = protocol == ControllerProtocol::automatic;
  set(SDL_HINT_XINPUT_ENABLED,
      automatic || protocol == ControllerProtocol::xinput);
  set(SDL_HINT_DIRECTINPUT_ENABLED,
      automatic || protocol == ControllerProtocol::direct_input);
  set(SDL_HINT_JOYSTICK_RAWINPUT,
      automatic || protocol == ControllerProtocol::raw_input);
  set(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, true);
  set(SDL_HINT_JOYSTICK_WGI, automatic);
  set(SDL_HINT_JOYSTICK_HIDAPI, automatic);
  set(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, true);
  set(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, true);
}

void configurePresentation(const GraphicsSettings &settings) noexcept {
  // SDL's high-resolution timer and GL context both exist only after
  // PsyX_Initialise. Apply the two independent presentation controls here:
  // swap interval removes tearing, while the software cap controls cadence.
  PsyX_EnableSwapInterval(settings.vsync ? 1 : 0);
  PsyX_SetSwapInterval(1);
#ifdef __EMSCRIPTEN__
  // requestAnimationFrame controls presentation. The desktop limiter sleeps
  // or spins, which would block the browser's event and audio processing.
  PsyX_SetFrameLimit(0);
#else
  PsyX_SetFrameLimit(static_cast<int>(settings.frame_limit));
#endif
  if (settings.volumetric_effects) {
    using namespace detail::volumetric_atlas_texture;
    static_assert(rgba.size() == static_cast<std::size_t>(width) * height * 4U);
    if (GR_UploadVolumetricDensityAtlas(
            static_cast<int>(width), static_cast<int>(height), rgba.data()) ==
        0) {
      // The renderer keeps its procedural volume path, and ultimately the
      // exact retail sprite fallback, if the authored atlas cannot upload.
      PsyX_Log_Warning(
          "Volumetric density atlas is unavailable; using procedural shapes\n");
    }
  }
}

void configureInput() {
  g_cfg_keyboardMapping.kc_dpad_up =
      PsyX_LookupKeyboardMapping("W", g_cfg_keyboardMapping.kc_dpad_up);
  g_cfg_keyboardMapping.kc_dpad_down =
      PsyX_LookupKeyboardMapping("S", g_cfg_keyboardMapping.kc_dpad_down);
  g_cfg_keyboardMapping.kc_dpad_left =
      PsyX_LookupKeyboardMapping("A", g_cfg_keyboardMapping.kc_dpad_left);
  g_cfg_keyboardMapping.kc_dpad_right =
      PsyX_LookupKeyboardMapping("D", g_cfg_keyboardMapping.kc_dpad_right);
  // PC actions are sampled directly by the player-input adapter. Keep them
  // out of PsyCross's merged virtual pad so Left Shift cannot turn a physical
  // R1 target-lock into the PC run action and Space cannot become Select.
  g_cfg_keyboardMapping.kc_r1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_r1);
  g_cfg_keyboardMapping.kc_l1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_l1);
  g_cfg_keyboardMapping.kc_select =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_select);
}

class PsyCrossHost final : public Host {
public:
  PsyCrossHost(std::string title, GraphicsSettings graphics)
      : title_(title.begin(), title.end()), graphics_(graphics) {
    title_.push_back('\0');
  }

  void run() override {
    initialise();
#ifdef __EMSCRIPTEN__
    // The browser owns the frame cadence; run() returns after scheduling it.
    emscripten_set_main_loop_arg(&PsyCrossHost::pump, this, 0, 0);
#else
    for (;;) {
      stepFrame();
    }
#endif
  }

  void stepFrame() {
    if (PsyX_BeginScene() != 0) {
      PsyX_EndScene();
    }
  }

private:
#ifdef __EMSCRIPTEN__
  static void pump(void *host) {
    pumpBrowserFrame([host] {
      static_cast<PsyCrossHost *>(host)->stepFrame();
      return false;
    });
  }
#endif

  void initialise() {
    configureGraphics(graphics_);
    configureControllerProtocol(graphics_.controller_protocol);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    // The window-mode guard's SDL event filter must outlive run() in the browser.
    window_mode_.emplace(graphics_.fullscreen);
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);
  }

  std::vector<char> title_;
  GraphicsSettings graphics_;
  std::optional<detail::PsyCrossWindowMode> window_mode_;
};

void uploadTitleNoticeFont() {
  // Keep PsyCross's debug font clear of the title TIMs at x=896..950 and the
  // movie upload rectangle at x=0..319, y=256..495.
  FntLoad(960, 256);
}

void configureTitleNotice() {
  uploadTitleNoticeFont();
  static_cast<void>(FntOpen(54, 70, 252, 154, 2, 256));
}

game::TitleSaveSlots
loadTitleSaveSlots(const std::filesystem::path &path) noexcept {
  const auto loaded = game::loadTitleSaveSlotsFile(path);
  if (loaded.status == game::TitleSaveLoadStatus::invalid) {
    PsyX_Log_Error("Ignoring invalid or unreadable title save file\n");
  } else if (loaded.status == game::TitleSaveLoadStatus::recovered) {
    PsyX_Log_Info("Recovered title save from the last complete backup\n");
  }
  return loaded.slots;
}

bool storeTitleSaveSlots(const std::filesystem::path &path,
                         const game::TitleSaveSlots &slots) noexcept {
  const auto stored = game::storeTitleSaveSlotsFile(path, slots);
  if (!stored) {
    PsyX_Log_Error("Campaign progress could not be persisted\n");
  }
#ifdef __EMSCRIPTEN__
  if (stored) {
    EM_ASM({
      if (Module['sfSaveTransfer']) { Module['sfSaveTransfer'].updated(); }
    });
  }
#endif
  return stored;
}

enum class SaveStoreDecision {
  stored,
  continue_without_saving,
  return_to_title,
};

std::uint16_t readHostButtons(const PADRAW &pad) noexcept {
  return static_cast<std::uint16_t>(pad.buttons[0]) |
         (static_cast<std::uint16_t>(pad.buttons[1]) << 8U);
}

KeyboardMouseActionSnapshot
sampleHostKeyboardMouseActions(const KeyboardMouseBindings &bindings) {
  int keyboard_count{};
  const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
  const auto keyboard_state =
      keyboard != nullptr && keyboard_count > 0
          ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                        keyboard_count)}
          : std::span<const std::uint8_t>{};
  const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
  return sampleKeyboardMouseActions(
      bindings, KeyboardMouseDeviceState{
                    .keyboard = keyboard_state,
                    .mouse_left = (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                    .mouse_right = (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                    .mouse_middle = (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                    .mouse_x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                    .mouse_x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                    .mouse_wheel_delta = detail::consumePsyCrossMouseWheel(),
                });
}

std::string buildSaveFailedNotice(const KeyboardMouseBindings &bindings) {
  const auto input_name = [&](KeyboardMouseAction action) {
    return std::string{keyboardMouseInputName(bindings[action])};
  };
  return "       SAVE FAILED\n\n  " + input_name(KeyboardMouseAction::interact) +
         "  Retry\n  " + input_name(KeyboardMouseAction::fire) +
         "  Continue without saving\n  " + input_name(KeyboardMouseAction::pause) +
         "  Return to title\n\n"
         "Campaign progress remains active.";
}

// Retry prompt for a failed title-save store, advanced one frame at a time.
//
// The host owns this prompt across frames and presents notice() until the
// player retries, continues without saving, or returns to the title.
class TitleSaveRecovery {
public:
  TitleSaveRecovery(std::filesystem::path path,
                    const game::TitleSaveSlots &slots,
                    const KeyboardMouseBindings &bindings)
      : path_(std::move(path)), slots_(slots), bindings_(bindings),
        notice_(buildSaveFailedNotice(bindings)) {
    configureTitleNotice();
  }

  [[nodiscard]] const std::string &notice() const noexcept { return notice_; }

  // Samples input once; on a retry edge attempts the store again. Returns the
  // player's decision once resolved, or std::nullopt to present the notice and
  // poll again next frame.
  std::optional<SaveStoreDecision> poll(PADRAW &pad,
                                        std::uint16_t &previous_buttons) {
    PsyX_UpdateInput();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings_);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto fire_down = actions[KeyboardMouseAction::fire];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized_ && interact_down && !interact_was_down_;
    const auto fire_pressed =
        keyboard_initialized_ && fire_down && !fire_was_down_;
    const auto pause_pressed =
        keyboard_initialized_ && pause_down && !pause_was_down_;
    keyboard_initialized_ = true;
    interact_was_down_ = interact_down;
    fire_was_down_ = fire_down;
    pause_was_down_ = pause_down;

    constexpr std::uint16_t retry_buttons = 0x4000U | 0x08U;
    constexpr std::uint16_t continue_buttons = 0x8000U;
    constexpr std::uint16_t title_buttons = 0x2000U | 0x01U;
    if ((pressed & title_buttons) != 0U || pause_pressed) {
      return SaveStoreDecision::return_to_title;
    }
    if ((pressed & continue_buttons) != 0U || fire_pressed) {
      PsyX_Log_Info("Continuing campaign without durable save progress\n");
      return SaveStoreDecision::continue_without_saving;
    }
    if (((pressed & retry_buttons) != 0U || interact_pressed) &&
        storeTitleSaveSlots(path_, slots_)) {
      return SaveStoreDecision::stored;
    }
    return std::nullopt;
  }

private:
  std::filesystem::path path_;
  game::TitleSaveSlots slots_;
  KeyboardMouseBindings bindings_;
  std::string notice_;
  bool keyboard_initialized_{};
  bool interact_was_down_{};
  bool fire_was_down_{};
  bool pause_was_down_{};
};

ControllerMenuSample
controllerMenuSample(const PsyXControllerSnapshot &snapshot) noexcept;
ControllerMenuSample updateTitleInputPromptBindings(
    detail::PsyCrossCampaignSaveRenderer &renderer,
    const KeyboardMouseBindings &keyboard_bindings,
    const KeyboardMouseActionSnapshot &keyboard_actions,
    InputPromptBindings &active_bindings, int &previous_controller_instance);

class CampaignSaveSession final {
public:
  CampaignSaveSession(const game::MissionPackage &mission,
                      const KeyboardMouseBindings &bindings)
      : renderer(mission, bindings),
        active_prompt_bindings(keyboardMouseInputPromptBindings(bindings)) {
    PsyX_Log_Info("Campaign save UI entered\n");
  }

  std::optional<game::CampaignSaveResult>
  poll(const game::TitleSaveSlots &slots, PADRAW &pad,
       std::uint16_t &previous_buttons, detail::PsyCrossUiAudio &ui_audio,
       const KeyboardMouseBindings &bindings) {
    constexpr std::uint16_t previous_buttons_mask = 0x80U | 0x10U;
    constexpr std::uint16_t next_buttons_mask = 0x20U | 0x40U;
    constexpr std::uint16_t confirm_buttons_mask = 0x4000U | 0x8000U | 0x08U;
    constexpr std::uint16_t cancel_buttons_mask = 0x2000U | 0x01U;
    PsyX_UpdateInput();
    ui_audio.update();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized && interact_down && !interact_was_down;
    const auto pause_pressed =
        keyboard_initialized && pause_down && !pause_was_down;
    keyboard_initialized = true;
    interact_was_down = interact_down;
    pause_was_down = pause_down;
    const auto menu_sample = updateTitleInputPromptBindings(
        renderer, bindings, actions, active_prompt_bindings,
        previous_controller_instance);
    const auto analog = analog_navigation.update(menu_sample);
    const game::CampaignSaveInput input{
        (pressed & previous_buttons_mask) != 0U || analog.previous,
        (pressed & next_buttons_mask) != 0U || analog.next,
        (pressed & confirm_buttons_mask) != 0U || interact_pressed,
        (pressed & cancel_buttons_mask) != 0U || pause_pressed,
    };
    const auto previous_phase = menu.phase();
    const auto previous_save_selection = menu.saveSelected();
    const auto previous_overwrite_selection = menu.overwriteSelected();
    const auto previous_slot = menu.slotSelection();
    const auto result = menu.update(input, slots);
    if (input.cancel) {
      ui_audio.play(detail::PsyCrossUiCue::cancel);
    } else if (input.confirm) {
      ui_audio.play(detail::PsyCrossUiCue::confirm);
    } else if (menu.phase() != previous_phase ||
               menu.saveSelected() != previous_save_selection ||
               menu.overwriteSelected() != previous_overwrite_selection ||
               menu.slotSelection() != previous_slot) {
      ui_audio.play(detail::PsyCrossUiCue::navigate);
    }
    if (result.decision != game::CampaignSaveDecision::none) {
      return result;
    }

    // Draw into either a fresh scene or the still-open implicit scene left by
    // the terminal guest packet. Ending that scene before covering it used to
    // swap the old, un-faded gameplay backbuffer to the window for one frame.
    // The ACD renderer starts with an opaque full-screen black tile, so it is
    // also the correct recovery path for an inherited scene.
    static_cast<void>(PsyX_BeginScene());
    renderer.draw(menu, slots);
    PsyX_EndScene();
    if (!first_frame_presented) {
      first_frame_presented = true;
      PsyX_Log_Info("Campaign save UI first frame presented\n");
    }
    return std::nullopt;
  }

private:
  // Reuse the retail renderer and gameplay presentation target, including an
  // implicit scene left open by the terminal guest packet.
  detail::PsyCrossCampaignSaveRenderer renderer;
  InputPromptBindings active_prompt_bindings;
  int previous_controller_instance{-1};
  game::CampaignSaveMenu menu;
  ControllerMenuNavigator analog_navigation;
  bool first_frame_presented{};
  bool keyboard_initialized{};
  bool interact_was_down{};
  bool pause_was_down{};
};

ControllerMenuSample
controllerMenuSample(const PsyXControllerSnapshot &snapshot) noexcept {
  if (snapshot.connected == 0U) {
    return {};
  }
  // Menus must remain usable after either gameplay stick layout is selected.
  // Collapse both physical sticks to the strongest axis; the stateful menu
  // navigator below rejects center noise and emits one edge per gesture.
  const auto axis =
      dominantControllerMenuAxis(snapshot.analog[0], snapshot.analog[1],
                                 snapshot.analog[2], snapshot.analog[3]);
  return ControllerMenuSample{.connected = true,
                              .instance_id = snapshot.instanceId,
                              .horizontal = 128U,
                              .vertical = axis};
}

ControllerMenuSample updateTitleInputPromptBindings(
    detail::PsyCrossCampaignSaveRenderer &renderer,
    const KeyboardMouseBindings &keyboard_bindings,
    const KeyboardMouseActionSnapshot &keyboard_actions,
    InputPromptBindings &active_bindings, int &previous_controller_instance) {
  PsyXControllerSnapshot snapshot{};
  static_cast<void>(PsyX_Pad_GetControllerSnapshot(0, &snapshot));
  const auto menu_sample = controllerMenuSample(snapshot);
  auto keyboard_mouse_activity = false;
  for (const auto held : keyboard_actions.held) {
    keyboard_mouse_activity = keyboard_mouse_activity || held;
  }
  const auto buttons = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(snapshot.buttons[0]) |
      (static_cast<std::uint16_t>(snapshot.buttons[1]) << 8U));
  const auto controller_held = static_cast<std::uint16_t>(~buttons);
  constexpr int prompt_axis_deadzone = 56;
  const auto axis_offset = static_cast<int>(menu_sample.vertical) - 128;
  const auto controller_activity = controller_held != 0U ||
                                   axis_offset < -prompt_axis_deadzone ||
                                   axis_offset > prompt_axis_deadzone;
  const auto controller_identity_changed =
      menu_sample.connected &&
      snapshot.instanceId != previous_controller_instance;
  if (!menu_sample.connected || keyboard_mouse_activity) {
    active_bindings = keyboardMouseInputPromptBindings(keyboard_bindings);
  } else if (controller_activity || controller_identity_changed ||
             active_bindings.device == InputPromptDevice::controller) {
    active_bindings =
        detail::titleControllerInputPromptBindings(snapshot.family);
  }
  renderer.setInputPromptBindings(active_bindings);
  previous_controller_instance =
      menu_sample.connected ? snapshot.instanceId : -1;
  return menu_sample;
}

std::vector<u_long> packWords(std::span<const std::uint16_t> words) {
  std::vector<u_long> packed((words.size() + 1U) / 2U);
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto shift = static_cast<unsigned int>((index & 1U) * 16U);
    packed[index / 2U] |= static_cast<u_long>(words[index]) << shift;
  }
  return packed;
}

RECT16 blockRect(const assets::TimBlock &block) {
  const auto checked = [](std::uint16_t value) {
    if (value > static_cast<std::uint16_t>(std::numeric_limits<short>::max())) {
      throw core::Error{core::ErrorCode::unsupported,
                        "TIM VRAM coordinate exceeds PsyCross range"};
    }
    return static_cast<short>(value);
  };
  return RECT16{
      checked(block.x),
      checked(block.y),
      checked(block.width_words),
      checked(block.height),
  };
}

void uploadBlock(const assets::TimBlock &block) {
  auto rect = blockRect(block);
  auto packed = packWords(block.words);
  LoadImage(&rect, packed.data());
}

int texturePageMode(assets::TimPixelMode mode) {
  switch (mode) {
  case assets::TimPixelMode::indexed4:
    return 0;
  case assets::TimPixelMode::indexed8:
    return 1;
  case assets::TimPixelMode::direct16:
    return 2;
  case assets::TimPixelMode::direct24:
    return 3;
  }
  return 2;
}

void uploadTitleAssets(const game::TitleAssets &assets) {
  for (const auto &sprite : assets.sprites()) {
    if (sprite.image.clut()) {
      uploadBlock(*sprite.image.clut());
    }
    uploadBlock(sprite.image.pixels());
  }
  DrawSync(0);
}

void drawTitleSprite(const game::TitleSprite &source, std::uint8_t brightness) {
  if (source.image.mode() != assets::TimPixelMode::indexed8 ||
      !source.image.clut()) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu sprites must use an 8-bit indexed TIM"};
  }

  const auto &pixels = source.image.pixels();
  const auto &clut = *source.image.clut();
  constexpr unsigned int texture_page_word_width = 64U;
  constexpr unsigned int texture_page_height = 256U;
  constexpr unsigned int indexed8_pixels_per_word = 2U;
  const auto u = static_cast<unsigned int>(pixels.x % texture_page_word_width) *
                 indexed8_pixels_per_word;
  const auto v = static_cast<unsigned int>(pixels.y % texture_page_height);
  const auto width = static_cast<unsigned int>(source.image.displayWidth());
  const auto height = static_cast<unsigned int>(source.image.displayHeight());
  if (u + width > 256U || v + height > 256U) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu TIM crosses its texture-page boundary"};
  }

  DR_TPAGE page{};
  // TITLE.OVL assigns ABR 1 (background + foreground) to all four sprites.
  // Title sprites are part of the visible 320x240 movie frame. Keep DFE
  // enabled so PsyCross composes them into the active display target, not its
  // VRAM-copy offscreen path.
  SetDrawTPage(&page, 1, 1, GetTPage(1, 1, pixels.x, pixels.y));
  DrawPrim(&page);

  // TITLE.OVL authored these positions for the regular 384x240 display, but
  // MOVIE.OVL presents TITLE.STR in a 320x240 target. A raw SPRT kept the
  // 384-wide coordinates and consequently pushed VIDEO.TIM through the right
  // edge of the 4:3 movie. Reproject the horizontal layout into the active
  // movie canvas; FT4 keeps the complete source image while independently
  // scaling its destination width.
  constexpr auto title_layout_width = 384U;
  constexpr auto title_movie_width = 320U;
  const auto scale_title_x = [](unsigned int value) {
    return (value * title_movie_width + title_layout_width / 2U) /
           title_layout_width;
  };
  const auto destination_x = scale_title_x(
      static_cast<unsigned int>(std::max<std::int16_t>(source.x, 0)));
  const auto destination_width = scale_title_x(width);
  const auto destination_y =
      static_cast<unsigned int>(std::max<std::int16_t>(source.y, 0));

  POLY_FT4 sprite{};
  setPolyFT4(&sprite);
  setSemiTrans(&sprite, 1);
  setRGB0(&sprite, brightness, brightness, brightness);
  sprite.tpage = GetTPage(1, 1, pixels.x, pixels.y);
  sprite.clut = GetClut(clut.x, clut.y);
  setXY4(&sprite, static_cast<float>(destination_x),
         static_cast<float>(destination_y),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y), static_cast<float>(destination_x),
         static_cast<float>(destination_y + height),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y + height));
  setUV4(&sprite, static_cast<u_char>(u), static_cast<u_char>(v),
         static_cast<u_char>(u + width), static_cast<u_char>(v),
         static_cast<u_char>(u), static_cast<u_char>(v + height),
         static_cast<u_char>(u + width), static_cast<u_char>(v + height));
  DrawPrim(&sprite);
}

class ControllerSettingsPersistence final {
public:
  ControllerSettingsPersistence(
      GraphicsSettings &graphics,
      const ControllerSettingsCommitCallback &callback) noexcept
      : graphics_(graphics), callback_(callback) {}

  [[nodiscard]] bool commit(const ControllerButtonBindings &bindings,
                            bool vibration) noexcept {
    graphics_.controller_bindings = bindings;
    graphics_.controller_vibration = vibration;
    return persistCurrent();
  }

  void retry() noexcept {
    if (!pending_) {
      return;
    }
    if (!persistCurrent()) {
      PsyX_Log_Warning("Controller settings persistence remains pending\n");
    }
  }

private:
  [[nodiscard]] bool persistCurrent() noexcept {
    if (!callback_) {
      pending_ = false;
      return true;
    }
    try {
      pending_ = !callback_(graphics_.controller_bindings,
                            graphics_.controller_vibration);
    } catch (...) {
      pending_ = true;
    }
    return !pending_;
  }

  GraphicsSettings &graphics_;
  const ControllerSettingsCommitCallback &callback_;
  bool pending_{};
};

class PsyCrossTitleHost final : public Host {
public:
  PsyCrossTitleHost(std::string title, game::TitleAssets assets,
                    game::TitleMovies movies,
                    game::MissionPackage initial_mission,
                    std::filesystem::path cue_path,
                    std::string supported_game_serial,
                    GraphicsSettings graphics, KeyboardMouseBindings input,
                    game::RetailCheatState cheats,
                    ControllerSettingsCommitCallback controller_settings_commit)
      : title_(title.begin(), title.end()), assets_(std::move(assets)),
        movies_(std::move(movies)),
        initial_mission_(std::move(initial_mission)),
        cue_path_(std::move(cue_path)),
        supported_game_serial_(std::move(supported_game_serial)),
        graphics_(graphics), input_(input), cheats_(cheats),
        controller_settings_commit_(std::move(controller_settings_commit)),
        controller_settings_persistence(graphics_, controller_settings_commit_) {
    title_.push_back('\0');
  }

  void run() override {
    begin();
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(&PsyCrossTitleHost::pump, this, 0, 0);
#else
    while (!poll()) {
    }
#endif
  }

private:
  enum class Phase {
    title,
    prepare_mission,
    movie,
    mission_start,
    scene,
    campaign_save,
    save_recovery,
    done,
  };

  enum class MoviePurpose { opening, recovered_ending, ending };
  enum class SavePurpose {
    recovered_advance,
    stage_completion,
    completion_advance,
  };

  void begin() {
    configureGraphics(graphics_);
    configureControllerProtocol(graphics_.controller_protocol);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    window_mode.emplace(graphics_.fullscreen);
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    uploadTitleAssets(assets_);
    configureTitleNotice();
    const auto save_location =
        game::defaultTitleSaveLocation(cue_path_, supported_game_serial_);
    const auto migration = game::migrateLegacyTitleSaveSlotsFile(save_location);
    if (migration == game::TitleSaveMigrationStatus::migrated) {
      PsyX_Log_Info("Migrated campaign save to the user-data directory\n");
    } else if (migration == game::TitleSaveMigrationStatus::failed) {
      PsyX_Log_Error("Legacy campaign save migration failed\n");
    }
    save_path = save_location.primary;
    menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    PadInitDirect(reinterpret_cast<unsigned char *>(&pad), nullptr);
    PadStartCom();

    ui_audio.emplace(cue_path_);
    title_load_renderer.emplace(initial_mission_, input_);
    active_title_prompt_bindings = keyboardMouseInputPromptBindings(input_);
    overlay = makeOverlay();
    beginTitle(true);
  }

  detail::MovieOverlayCallbacks makeOverlay() {
    return detail::MovieOverlayCallbacks{
        [this](std::uint16_t pressed, std::uint32_t movie_frame) {
          ui_audio->update();
          const auto actions = sampleHostKeyboardMouseActions(input_);
          const auto interact_down = actions[KeyboardMouseAction::interact];
          const auto pause_down = actions[KeyboardMouseAction::pause];
          const auto interact_pressed = title_keyboard_initialized &&
                                        interact_down &&
                                        !title_interact_was_down;
          const auto pause_pressed =
              title_keyboard_initialized && pause_down && !title_pause_was_down;
          title_keyboard_initialized = true;
          title_interact_was_down = interact_down;
          title_pause_was_down = pause_down;
          const auto menu_sample = updateTitleInputPromptBindings(
              *title_load_renderer, input_, actions,
              active_title_prompt_bindings,
              previous_title_controller_instance);
          const auto analog = title_menu_navigation_.update(menu_sample);
          const game::TitleInput input{
              .previous = (pressed & (0x80U | 0x10U)) != 0 || analog.previous,
              .next = (pressed & (0x20U | 0x40U)) != 0 || analog.next,
              .confirm = (pressed & (0x4000U | 0x8000U | 0x08U)) != 0 ||
                         interact_pressed,
              .cancel = (pressed & (0x2000U | 0x01U)) != 0 || pause_pressed,
              .confirm_down = ((~readHostButtons(pad)) &
                               (0x4000U | 0x8000U | 0x08U)) != 0U ||
                              interact_down,
          };
          const auto previous_selection = menu_.selection();
          const auto previous_phase = menu_.phase();
          const auto previous_slot = menu_.loadSlotSelection();
          const auto previous_difficulty = menu_.difficultySelection();
          const auto command = menu_.update(input, movie_frame);
          if ((previous_phase == game::TitlePhase::load_slots ||
               previous_phase == game::TitlePhase::select_difficulty ||
               previous_phase == game::TitlePhase::agent_warning) &&
              menu_.phase() == game::TitlePhase::menu) {
            // The retail font atlas shares VRAM pages with TITLE.HOG. Restore
            // the title sprites before returning from a font-backed picker.
            uploadTitleAssets(assets_);
          }
          if (input.cancel) {
            ui_audio->play(detail::PsyCrossUiCue::cancel);
          } else if (input.confirm && (command != game::TitleCommand::none ||
                                       menu_.phase() != previous_phase)) {
            ui_audio->play(detail::PsyCrossUiCue::confirm);
          } else if (menu_.selection() != previous_selection ||
                     menu_.loadSlotSelection() != previous_slot ||
                     menu_.difficultySelection() != previous_difficulty) {
            ui_audio->play(detail::PsyCrossUiCue::navigate);
          }
#ifdef __EMSCRIPTEN__
          // A browser tab cannot quit: Esc on the title's top level toggles the
          // page menu instead of ending the host and stopping the frame loop.
          if (command == game::TitleCommand::exit) {
            EM_ASM({
              if (Module['sfPointer']) { Module['sfPointer'].toggleTitleMenu(); }
            });
            return true;
          }
#endif
          if (command == game::TitleCommand::exit ||
              command == game::TitleCommand::new_game ||
              command == game::TitleCommand::load_game ||
              command == game::TitleCommand::training_video) {
            selected_command_ = command;
            PsyX_Log_Info("Title command accepted: %s\n",
                          game::titleCommandName(command).data());
            return false;
          }
          return true;
        },
        [this] {
          if (menu_.phase() == game::TitlePhase::load_slots) {
            title_load_renderer->drawLoadSlots(menu_.saveSlots(),
                                              menu_.loadSlotSelection());
          } else if (menu_.phase() == game::TitlePhase::select_difficulty) {
            title_load_renderer->drawDifficultySelection(menu_);
          } else if (menu_.phase() == game::TitlePhase::agent_warning) {
            title_load_renderer->drawAgentModeWarning();
          } else {
            for (std::size_t index = 0; index < game::TitleMenu::visual_count;
                 ++index) {
              const auto visual = static_cast<game::TitleVisual>(index);
              const auto brightness = menu_.brightness(visual);
              if (brightness != 0) {
                drawTitleSprite(assets_.sprite(visual), brightness);
              }
            }
          }
        },
        [this]() -> const game::DiscMovie * {
          if (selected_command_ == game::TitleCommand::new_game &&
              !initial_mission_.openingMovie().path.empty()) {
            return &initial_mission_.openingMovie();
          }
          if (selected_command_ == game::TitleCommand::training_video) {
            return &movies_.trainingMovie();
          }
          return nullptr;
        },
    };
  }

  // Each callback polls one session. Transitions release its video/audio
  // guards before entering the next presentation mode.
  bool poll() {
    switch (phase) {
    case Phase::title:
      if (const auto buttons = title_movie->poll(pad)) {
        previous_buttons = *buttons;
        title_movie.reset();
        finishTitle();
      }
      break;
    case Phase::prepare_mission:
      prepareMission();
      break;
    case Phase::movie:
      if (const auto buttons = movie->poll(pad)) {
        previous_buttons = *buttons;
        movie.reset();
        finishMovie();
      }
      break;
    case Phase::mission_start:
      if (const auto buttons = mission_start->poll(pad)) {
        previous_buttons = *buttons;
        beginScene();
      }
      break;
    case Phase::scene:
      if (const auto result = scene_viewer->poll(pad)) {
        finishScene(*result);
      }
      break;
    case Phase::campaign_save:
      if (const auto result = campaign_save->poll(
              save_slots, pad, previous_buttons, *ui_audio, input_)) {
        campaign_save.reset();
        finishCampaignSave(*result);
      }
      break;
    case Phase::save_recovery:
      if (const auto decision = save_recovery->poll(pad, previous_buttons)) {
        save_recovery.reset();
        finishSave(*decision);
      } else if (PsyX_BeginScene() != 0) {
        char format[] = "%s";
        static_cast<void>(FntPrint(format, save_recovery->notice().c_str()));
        static_cast<void>(FntFlush());
        PsyX_EndScene();
      }
      break;
    case Phase::done:
      return true;
    }
    return phase == Phase::done;
  }

  void beginTitle(bool play_startup_movies = false) {
    selected_command_ = game::TitleCommand::none;
    // Require a released stick after startup, training, or gameplay.
    title_menu_navigation_.reset();
    title_movie.emplace(movies_, previous_buttons, overlay, play_startup_movies);
    phase = Phase::title;
  }

  void finishTitle() {
    if (selected_command_ == game::TitleCommand::training_video) {
      menu_.completeSearch();
      beginTitle();
      return;
    }
    if (selected_command_ != game::TitleCommand::new_game &&
        selected_command_ != game::TitleCommand::load_game) {
      finish();
      return;
    }

    save_slots = menu_.saveSlots();
    campaign_carry.reset();
    const auto title_played_opening_movie =
        selected_command_ == game::TitleCommand::new_game &&
        !initial_mission_.openingMovie().path.empty();
    if (selected_command_ == game::TitleCommand::load_game) {
      campaign = game::CampaignProgress::resume(save_slots,
                                                menu_.loadSlotSelection());
      if (!campaign) {
        PsyX_Log_Error("Load Game rejected invalid selected slot\n");
        returnToTitle(false);
        return;
      }
      PsyX_Log_Info("Load Game: slot=%zu mission=%u\n",
                    *campaign->saveSlot() + 1U, campaign->missionIndex());
      campaign_carry = save_slots[*campaign->saveSlot()].carry;
    } else {
      campaign = game::CampaignProgress::startUnsaved(
          initial_mission_.definition().index, title_played_opening_movie,
          menu_.selectedDifficulty());
      if (!campaign) {
        PsyX_Log_Error("New Game rejected an invalid campaign cursor\n");
        returnToTitle(false);
        return;
      }
      PsyX_Log_Info(
          "New Game FMV complete; opening unsaved mission %u on %s difficulty\n",
          campaign->missionIndex() + 1U,
          game::campaignDifficultyDisplayName(campaign->difficulty()).data());
    }
    controller_settings_persistence.retry();
    mission_start.emplace();
    scene_viewer.emplace(
        input_, cheats_, campaign->difficulty(), graphics_.controller_bindings,
        graphics_.controller_vibration,
        [this](const ControllerButtonBindings &bindings, bool vibration) {
          return controller_settings_persistence.commit(bindings, vibration);
        },
        graphics_.mission_skyboxes);
    phase = Phase::prepare_mission;
  }

  void prepareMission() {
    const auto mission_index = campaign->missionIndex();
    if (mission_index == initial_mission_.definition().index) {
      mission = &initial_mission_;
    } else {
      auto disc = game::GameDisc::open(cue_path_);
      loaded_mission.emplace(game::MissionPackage::load(disc, mission_index));
      mission = &*loaded_mission;
    }
    if (campaign->pendingEndingMovieMission()) {
      PsyX_Log_Info("Recovering interrupted EOL for mission %u\n",
                    mission_index + 1U);
      beginMovie(MoviePurpose::recovered_ending);
    } else if (campaign->openingMovieRequired(mission->definition())) {
      beginMovie(MoviePurpose::opening);
    } else {
      beginMissionStart();
    }
  }

  void beginMovie(MoviePurpose purpose) {
    movie_purpose = purpose;
    const auto &source = purpose == MoviePurpose::opening
                             ? mission->openingMovie()
                             : mission->endingMovie();
    if (source.path.empty()) {
      finishMovie();
      return;
    }
    movie.emplace(source, previous_buttons,
                  purpose == MoviePurpose::opening
                      ? detail::StandaloneMovieSkipPolicy::allow
                      : endingMovieSkipPolicy(mission->definition()));
    phase = Phase::movie;
  }

  void finishMovie() {
    switch (movie_purpose) {
    case MoviePurpose::opening:
      beginMissionStart();
      break;
    case MoviePurpose::recovered_ending:
      prepareAdvance(true);
      break;
    case MoviePurpose::ending:
      prepareAdvance(false);
      break;
    }
  }

  void beginMissionStart() {
    campaign->markOpeningMovieHandled();
    mission_start->begin(
        *mission, previous_buttons, input_, campaign_carry,
        campaign->difficulty() == game::CampaignDifficulty::agent);
    phase = Phase::mission_start;
  }

  void beginScene() {
    const auto &definition = mission->definition();
    std::cout << "Starting mission " << (definition.index + 1U) << ": "
              << definition.title << " [" << definition.resource_name
              << "]\nMounted " << mission->archive().entries().size()
              << " FOG files, " << mission->textureFileCount()
              << " textures and " << mission->worldModelCount()
              << " world models\n";
    scene_viewer->begin(*mission, previous_buttons, cue_path_,
                        campaign->maximumUnlockedMission(),
                        mission_start->takePreloadedGameplay(),
                        mission_start->takePreloadedAudio());
    phase = Phase::scene;
  }

  void finishScene(const detail::SceneViewerResult &result) {
    controller_settings_persistence.retry();
    previous_buttons = result.previous_buttons;
    if (result.reason == detail::SceneExitReason::mission_selected &&
        result.selected_mission) {
      const auto selected = *result.selected_mission;
      if (!campaign->selectUnlockedMission(selected)) {
        auto replacement = game::CampaignProgress::startUnsaved(
            selected, false, campaign->difficulty());
        if (!replacement) {
          PsyX_Log_Error("Pause mission selection rejected mission %u\n",
                         selected + 1U);
          returnToTitle();
          return;
        }
        campaign = std::move(replacement);
      }
      campaign_carry.reset();
      loaded_mission.reset();
      mission = nullptr;
      PsyX_Log_Info("Pause mission selection: mission=%u\n", selected + 1U);
      phase = Phase::prepare_mission;
      return;
    }
    if (result.reason == detail::SceneExitReason::mission_complete) {
      const auto mission_index = campaign->missionIndex();
      const auto next_mission = mission_index + 1U;
      carry_for_next = game::campaignMissionsShareCarry(mission_index, next_mission)
                           ? result.carry
                           : std::nullopt;
      if (game::campaignMissionsShareCarry(mission_index, next_mission) &&
          !carry_for_next) {
        PsyX_Log_Warning(
            "Campaign transition has no coherent player carry; "
            "continuing with mission defaults\n");
      }
      completion_is_saved = false;
      campaign_save.emplace(*mission, input_);
      phase = Phase::campaign_save;
      return;
    }
    if (result.reason == detail::SceneExitReason::exit_application) {
      finish();
    } else {
      returnToTitle();
    }
  }

  void finishCampaignSave(const game::CampaignSaveResult &result) {
    if (result.decision == game::CampaignSaveDecision::save && result.slot) {
      candidate_campaign = *campaign;
      candidate_slots = save_slots;
      if (!candidate_campaign->stageMissionCompletionInSlot(
              candidate_slots, *result.slot, carry_for_next)) {
        PsyX_Log_Error("Campaign save transaction rejected state\n");
        returnToTitle();
        return;
      }
      // Persist the pending EOL before playback so interrupted campaigns
      // replay this handoff when the player resumes their save.
      beginSave(SavePurpose::stage_completion);
    } else {
      beginMovie(MoviePurpose::ending);
    }
  }

  void prepareAdvance(bool recovered_ending) {
    candidate_campaign = *campaign;
    candidate_slots = save_slots;
    candidate_advance = recovered_ending || completion_is_saved
                            ? candidate_campaign->completeMission(candidate_slots)
                            : candidate_campaign->completeMissionWithoutSaving();
    if (candidate_advance == game::CampaignAdvance::invalid) {
      PsyX_Log_Error("Campaign transition rejected inconsistent state\n");
      returnToTitle();
      return;
    }
    if (recovered_ending || completion_is_saved) {
      beginSave(recovered_ending ? SavePurpose::recovered_advance
                                : SavePurpose::completion_advance);
    } else {
      save_purpose = SavePurpose::completion_advance;
      finishSave(SaveStoreDecision::continue_without_saving);
    }
  }

  void beginSave(SavePurpose purpose) {
    save_purpose = purpose;
    if (storeTitleSaveSlots(save_path, candidate_slots)) {
      finishSave(SaveStoreDecision::stored);
    } else {
      save_recovery.emplace(save_path, candidate_slots, input_);
      phase = Phase::save_recovery;
    }
  }

  void finishSave(SaveStoreDecision decision) {
    if (decision == SaveStoreDecision::return_to_title) {
      returnToTitle();
      return;
    }
    if (save_purpose == SavePurpose::stage_completion) {
      if (decision == SaveStoreDecision::stored) {
        campaign = candidate_campaign;
        save_slots = candidate_slots;
        completion_is_saved = true;
      }
      beginMovie(MoviePurpose::ending);
      return;
    }
    campaign = candidate_campaign;
    save_slots = candidate_slots;
    if (save_purpose == SavePurpose::recovered_advance) {
      campaign_carry = campaign->saveSlot()
                           ? save_slots[*campaign->saveSlot()].carry
                           : std::nullopt;
    } else {
      campaign_carry = candidate_advance == game::CampaignAdvance::next_mission
                           ? carry_for_next
                           : std::nullopt;
    }
    if (candidate_advance == game::CampaignAdvance::campaign_complete) {
      PsyX_Log_Info("Campaign complete\n");
      returnToTitle();
    } else {
      phase = Phase::prepare_mission;
    }
  }

  void clearCampaign() {
    movie.reset();
    campaign_save.reset();
    save_recovery.reset();
    scene_viewer.reset();
    // Join any preload before releasing the mission it reads.
    mission_start.reset();
    loaded_mission.reset();
    mission = nullptr;
    campaign.reset();
    candidate_campaign.reset();
  }

  void returnToTitle(bool complete_search = true) {
    clearCampaign();
    uploadTitleAssets(assets_);
    configureTitleNotice();
    if (complete_search) {
      menu_.completeSearch();
    }
    menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    beginTitle();
  }

  void finish() {
    clearCampaign();
    title_movie.reset();
    // Release run-local resources while the browser keeps the host alive.
    title_load_renderer.reset();
    ui_audio.reset();
    PadStopCom();
    window_mode.reset();
    phase = Phase::done;
  }

#ifdef __EMSCRIPTEN__
  static void pump(void *host) {
    pumpBrowserFrame([host] {
      return static_cast<PsyCrossTitleHost *>(host)->poll();
    });
  }
#endif

  std::vector<char> title_;
  game::TitleAssets assets_;
  game::TitleMovies movies_;
  game::MissionPackage initial_mission_;
  std::filesystem::path cue_path_;
  std::string supported_game_serial_;
  game::TitleMenu menu_;
  game::TitleCommand selected_command_{game::TitleCommand::none};
  ControllerMenuNavigator title_menu_navigation_;
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
  ControllerSettingsCommitCallback controller_settings_commit_;

  ControllerSettingsPersistence controller_settings_persistence;
  PADRAW pad{};
  std::uint16_t previous_buttons{0xffffU};
  std::filesystem::path save_path;
  bool title_keyboard_initialized{};
  bool title_interact_was_down{};
  bool title_pause_was_down{};
  InputPromptBindings active_title_prompt_bindings;
  int previous_title_controller_instance{-1};
  std::optional<detail::PsyCrossUiAudio> ui_audio;
  std::optional<detail::PsyCrossCampaignSaveRenderer> title_load_renderer;
  detail::MovieOverlayCallbacks overlay;
  std::optional<detail::TitleMovieSession> title_movie;
  std::optional<game::CampaignProgress> campaign;
  game::TitleSaveSlots save_slots{};
  std::optional<game::CampaignCarryState> campaign_carry;
  std::optional<game::CampaignCarryState> carry_for_next;
  std::optional<game::CampaignProgress> candidate_campaign;
  game::TitleSaveSlots candidate_slots{};
  game::CampaignAdvance candidate_advance{game::CampaignAdvance::invalid};
  bool completion_is_saved{};
  // Mission ownership precedes all sessions that reference it, so member
  // destruction releases renderers and joins preloads before freeing data.
  std::optional<game::MissionPackage> loaded_mission;
  game::MissionPackage *mission{};
  std::optional<detail::PsyCrossMissionStart> mission_start;
  std::optional<detail::PsyCrossSceneViewer> scene_viewer;
  std::optional<detail::StandaloneMovieSession> movie;
  std::optional<CampaignSaveSession> campaign_save;
  std::optional<TitleSaveRecovery> save_recovery;
  std::optional<detail::PsyCrossWindowMode> window_mode;
  Phase phase{Phase::done};
  MoviePurpose movie_purpose{MoviePurpose::opening};
  SavePurpose save_purpose{SavePurpose::stage_completion};
};

class PsyCrossSceneHost final : public Host {
public:
  PsyCrossSceneHost(std::string title, game::MissionPackage mission,
                    std::filesystem::path cue_path, GraphicsSettings graphics,
                    KeyboardMouseBindings input, game::RetailCheatState cheats,
                    ControllerSettingsCommitCallback controller_settings_commit)
      : title_(title.begin(), title.end()), mission_(std::move(mission)),
        cue_path_(std::move(cue_path)), graphics_(graphics), input_(input),
        cheats_(cheats),
        controller_settings_commit_(std::move(controller_settings_commit)),
        persistence_(graphics_, controller_settings_commit_) {
    title_.push_back('\0');
  }

  void run() override {
    begin();
#ifdef __EMSCRIPTEN__
    // The browser owns the frame cadence; run() returns after scheduling it.
    emscripten_set_main_loop_arg(&PsyCrossSceneHost::pump, this, 0, 0);
#else
    while (!poll()) {
    }
#endif
  }

private:
  enum class ScenePhase {
    opening_movie,
    mission_start,
    scene,
    ending_movie,
    done,
  };

  void begin() {
    movie_.reset();
    scene_viewer_.reset();
    configureGraphics(graphics_);
    configureControllerProtocol(graphics_.controller_protocol);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    // The window-mode guard's SDL event filter must outlive run() in the browser.
    window_mode_.emplace(graphics_.fullscreen);
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    PadInitDirect(reinterpret_cast<unsigned char *>(&pad_), nullptr);
    PadStartCom();
    previous_buttons_ = 0xffffU;
    if (!mission_.openingMovie().path.empty()) {
      movie_.emplace(mission_.openingMovie(), previous_buttons_);
      phase_ = ScenePhase::opening_movie;
    } else {
      beginMissionStart();
    }
  }

  // Advances at most one presented frame. Returns true once the mission flow
  // (including a possible ending movie) has fully played out.
  bool poll() {
    switch (phase_) {
    case ScenePhase::opening_movie:
      if (const auto buttons = movie_->poll(pad_)) {
        previous_buttons_ = *buttons;
        movie_.reset();
        beginMissionStart();
      }
      return false;
    case ScenePhase::mission_start:
      if (const auto buttons = mission_start_.poll(pad_)) {
        previous_buttons_ = *buttons;
        beginScene();
      }
      return false;
    case ScenePhase::scene:
      if (const auto result = scene_viewer_->poll(pad_)) {
        finishScene(*result);
      }
      return false;
    case ScenePhase::ending_movie:
      if (const auto buttons = movie_->poll(pad_)) {
        previous_buttons_ = *buttons;
        movie_.reset();
        finish();
      }
      return false;
    case ScenePhase::done:
      return true;
    }
    return true;
  }

  void beginMissionStart() {
    mission_start_.begin(mission_, previous_buttons_, input_);
    phase_ = ScenePhase::mission_start;
  }

  void beginScene() {
    persistence_.retry();
    scene_viewer_.emplace(
        input_, cheats_, game::CampaignDifficulty::original,
        graphics_.controller_bindings, graphics_.controller_vibration,
        [this](const ControllerButtonBindings &bindings, bool vibration) {
          return persistence_.commit(bindings, vibration);
        },
        graphics_.mission_skyboxes);
    scene_viewer_->begin(mission_, previous_buttons_, cue_path_,
                         mission_.definition().index,
                         mission_start_.takePreloadedGameplay(),
                         mission_start_.takePreloadedAudio());
    phase_ = ScenePhase::scene;
  }

  void finishScene(const detail::SceneViewerResult &result) {
    persistence_.retry();
    previous_buttons_ = result.previous_buttons;
    if (result.reason == detail::SceneExitReason::mission_complete &&
        !mission_.endingMovie().path.empty()) {
      movie_.emplace(mission_.endingMovie(), previous_buttons_,
                     endingMovieSkipPolicy(mission_.definition()));
      phase_ = ScenePhase::ending_movie;
    } else {
      finish();
    }
  }

  void finish() {
    persistence_.retry();
    movie_.reset();
    scene_viewer_.reset();
    PadStopCom();
    window_mode_.reset();
    phase_ = ScenePhase::done;
  }

#ifdef __EMSCRIPTEN__
  static void pump(void *host) {
    pumpBrowserFrame(
        [host] { return static_cast<PsyCrossSceneHost *>(host)->poll(); });
  }
#endif

  std::vector<char> title_;
  game::MissionPackage mission_;
  std::filesystem::path cue_path_;
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
  ControllerSettingsCommitCallback controller_settings_commit_;
  ControllerSettingsPersistence persistence_;
  PADRAW pad_{};
  std::uint16_t previous_buttons_{0xffffU};
  ScenePhase phase_{ScenePhase::done};
  std::optional<detail::StandaloneMovieSession> movie_;
  detail::PsyCrossMissionStart mission_start_;
  std::optional<detail::PsyCrossSceneViewer> scene_viewer_;
  std::optional<detail::PsyCrossWindowMode> window_mode_;
};

} // namespace

std::unique_ptr<Host> createPsyCrossHost(std::string title,
                                         GraphicsSettings graphics) {
  return std::make_unique<PsyCrossHost>(std::move(title), graphics);
}

std::unique_ptr<Host> createPsyCrossTitleHost(
    std::string title, game::TitleAssets assets, game::TitleMovies movies,
    game::MissionPackage initial_mission, std::filesystem::path cue_path,
    std::string supported_game_serial, GraphicsSettings graphics,
    KeyboardMouseBindings input, game::RetailCheatState cheats,
    ControllerSettingsCommitCallback controller_settings_commit) {
  return std::make_unique<PsyCrossTitleHost>(
      std::move(title), std::move(assets), std::move(movies),
      std::move(initial_mission), std::move(cue_path),
      std::move(supported_game_serial), graphics, input, cheats,
      std::move(controller_settings_commit));
}

std::unique_ptr<Host> createPsyCrossSceneHost(
    std::string title, game::MissionPackage mission,
    std::filesystem::path cue_path, GraphicsSettings graphics,
    KeyboardMouseBindings input, game::RetailCheatState cheats,
    ControllerSettingsCommitCallback controller_settings_commit) {
  return std::make_unique<PsyCrossSceneHost>(
      std::move(title), std::move(mission), std::move(cue_path), graphics,
      input, cheats, std::move(controller_settings_commit));
}

} // namespace sf::platform
