#include "psycross_mission_start.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_retail_briefing.hpp"
#include "psycross_window_mode.hpp"

#include "sf/core/error.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/mission_start.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libpad.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace sf::platform::detail {
namespace {

constexpr std::uint16_t confirm_buttons = 0x4000U | 0x08U;

std::uint16_t readButtons(const PADRAW &pad) noexcept {
  return static_cast<std::uint16_t>(pad.buttons[0]) |
         (static_cast<std::uint16_t>(pad.buttons[1]) << 8U);
}

ControllerPromptFamily briefingControllerFamily(int family) noexcept {
  switch (family) {
  case PSYX_CONTROLLER_FAMILY_XBOX:
    return ControllerPromptFamily::xbox;
  case PSYX_CONTROLLER_FAMILY_PLAYSTATION:
    return ControllerPromptFamily::playstation;
  case PSYX_CONTROLLER_FAMILY_NINTENDO:
    return ControllerPromptFamily::nintendo;
  default:
    return ControllerPromptFamily::generic;
  }
}

InputPromptBindings
briefingControllerPrompts(const PsyXControllerSnapshot &snapshot) {
  constexpr std::uint16_t start_button = 0x0008U;
  constexpr std::uint16_t triangle_button = 0x1000U;
  constexpr std::uint16_t cross_button = 0x4000U;
  constexpr std::uint16_t square_button = 0x8000U;
  const auto family = briefingControllerFamily(snapshot.family);
  const auto name = [family](std::uint16_t button) {
    return controllerButtonPromptName(family, button);
  };
  return controllerInputPromptBindings(ControllerInputProtocol::unknown,
                                       InputPromptBindingNames{
                                           .confirm = name(cross_button),
                                           .cancel = name(triangle_button),
                                           .pause = name(start_button),
                                           .interact = name(triangle_button),
                                           .fire = name(square_button),
                                       });
}

void drawMissionStartFade(std::uint8_t intensity) {
  if (intensity == 0U) {
    return;
  }
  GR_SetBlendMode(BM_SUBTRACT);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, 2, 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, intensity, intensity, intensity);
  setXY0(&tile, 0.0F, 0.0F);
  setWH(&tile, 384.0F, 240.0F);
  DrawPrim(&tile);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
}

} // namespace

std::unique_ptr<game::GameplaySession>
loadPreloadedGameplay(const game::MissionPackage &mission,
                      std::optional<game::CampaignCarryState> carry,
                      bool initial_agent_difficulty) {
  auto gameplay = std::make_unique<game::GameplaySession>(
      mission, initial_agent_difficulty);
  if (carry && !gameplay->applyCampaignCarryState(*carry)) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Campaign carry could not be applied to retail RAM"};
  }
  return gameplay;
}

struct PsyCrossMissionStart::Impl {
  // Owned outputs, harvested by takePreloadedGameplay/takePreloadedAudio.
  std::unique_ptr<game::GameplaySession> preloaded_gameplay_;
  std::unique_ptr<PsyCrossAudioOutput> preloaded_audio_;

  // begin() parameters.
  const game::MissionPackage *mission_{};
  std::uint16_t previous_buttons_{};
  KeyboardMouseBindings bindings_{};
  std::optional<game::CampaignCarryState> carry_;
  bool initial_agent_difficulty_{};

  // Per-run briefing state (rebuilt by every begin()).
  std::optional<PsyCrossRetailBriefing> briefing_;
  std::optional<game::MissionStartGate> gate_;
#ifndef __EMSCRIPTEN__
  std::future<std::unique_ptr<game::GameplaySession>> preload_;
#else
  int loading_frames_remaining_{};
  bool sync_load_done_{};
#endif
  std::uint64_t performance_frequency_{};
  std::optional<std::uint64_t> animation_start_;
  std::optional<std::uint64_t> audio_callback_tick_;
  bool audio_clock_started_{};
#ifdef __EMSCRIPTEN__
  double audio_start_clock_seconds_{};
#endif
  std::optional<std::uint64_t> fade_out_start_;
  std::array<psx::SpuPcmFrame, 4096U> briefing_pcm_{};
  std::uint64_t audio_slices_completed_{};
  std::uint64_t audio_pcm_frames_pumped_{};
  std::uint64_t audio_pcm_blocks_pumped_{};
  std::uint64_t audio_diagnostic_sequence_{};
  InputPromptBindings active_prompt_bindings_{};
  std::uint16_t previous_controller_buttons_{};
  int previous_controller_instance_{};
  KeyboardMouseActionSnapshot previous_bound_actions_{};
  bool periodic_audio_diagnostics_{};
  std::uint64_t next_audio_diagnostic_counter_{};

  static constexpr std::uint32_t retail_audio_callback_hz = 120U;
  static constexpr std::uint64_t maximum_audio_updates_per_iteration = 30U;
#ifdef __EMSCRIPTEN__
  // Single-threaded WASM cannot run the preload on a worker thread. Present
  // this many briefing frames first so the loading screen paints, then load
  // synchronously (one unavoidable freeze) and continue briefing.
  static constexpr int loading_frames_before_sync_load = 3;
#endif

  void pumpAudio() {
    while (const auto count = preloaded_gameplay_->takePcm(briefing_pcm_)) {
      audio_pcm_frames_pumped_ += count;
      ++audio_pcm_blocks_pumped_;
      preloaded_audio_->queue(
          std::span<const psx::SpuPcmFrame>{briefing_pcm_}.first(count));
    }
    preloaded_audio_->flush();
    preloaded_audio_->update();
  }
};

PsyCrossMissionStart::PsyCrossMissionStart()
    : impl_(std::make_unique<Impl>()) {}
PsyCrossMissionStart::~PsyCrossMissionStart() = default;

std::unique_ptr<game::GameplaySession>
PsyCrossMissionStart::takePreloadedGameplay() noexcept {
  return std::move(impl_->preloaded_gameplay_);
}

std::unique_ptr<PsyCrossAudioOutput>
PsyCrossMissionStart::takePreloadedAudio() noexcept {
  return std::move(impl_->preloaded_audio_);
}

void PsyCrossMissionStart::begin(
    const game::MissionPackage &mission, std::uint16_t previous_buttons,
    const KeyboardMouseBindings &bindings,
    std::optional<game::CampaignCarryState> carry,
    bool initial_agent_difficulty) {
  auto &state = *impl_;
  // The retail briefing is also the level-loading boundary. Remove the STR
  // framebuffer and its texture-page residue before presenting it; the
  // scene viewer uploads a fresh mission working set after confirmation.
  RECT16 whole_vram{0, 0, 1024, 512};
  ClearImage(&whole_vram, 0, 0, 0);
  DrawSync(0);
  state.mission_ = &mission;
  state.previous_buttons_ = previous_buttons;
  state.bindings_ = bindings;
  state.carry_ = std::move(carry);
  state.initial_agent_difficulty_ = initial_agent_difficulty;
  state.briefing_.emplace(mission);
  state.preloaded_gameplay_.reset();
  state.preloaded_audio_ = std::make_unique<PsyCrossAudioOutput>();
#ifndef __EMSCRIPTEN__
  // A previous run always harvests its future before returning, but never
  // launch over a live worker: the lambda below borrows the mission.
  if (state.preload_.valid()) {
    static_cast<void>(state.preload_.wait());
  }
  state.preload_ = std::async(
      std::launch::async,
      [mission = state.mission_, carry = state.carry_,
       initial_agent_difficulty = state.initial_agent_difficulty_] {
        return loadPreloadedGameplay(*mission, carry, initial_agent_difficulty);
      });
#else
  state.sync_load_done_ = false;
  state.loading_frames_remaining_ = Impl::loading_frames_before_sync_load;
#endif
  state.gate_.emplace();
  static_cast<void>(state.gate_->update(
      (static_cast<std::uint16_t>(~previous_buttons) & confirm_buttons) != 0U,
      false));
  state.performance_frequency_ = SDL_GetPerformanceFrequency();
  state.animation_start_.reset();
  state.audio_callback_tick_.reset();
  state.audio_clock_started_ = false;
  state.fade_out_start_.reset();
  state.audio_slices_completed_ = 0U;
  state.audio_pcm_frames_pumped_ = 0U;
  state.audio_pcm_blocks_pumped_ = 0U;
  state.audio_diagnostic_sequence_ = 0U;
  state.active_prompt_bindings_ = keyboardMouseInputPromptBindings(bindings);
  state.previous_controller_buttons_ = 0xffffU;
  state.previous_controller_instance_ = -1;
  state.previous_bound_actions_ = KeyboardMouseActionSnapshot{};
  state.periodic_audio_diagnostics_ = psyCrossAudioDiagnosticsEnabled();
  state.next_audio_diagnostic_counter_ =
      SDL_GetPerformanceCounter() + state.performance_frequency_;
  PsyX_Log_Info(
      "Mission briefing: retail transition and gameplay preload started\n");
}

std::optional<std::uint16_t>
PsyCrossMissionStart::poll(PADRAW &pad) {
  auto &state = *impl_;
  const auto &mission = *state.mission_;
  const auto &bindings = state.bindings_;
  PsyX_UpdateInput();
  state.previous_buttons_ = readButtons(pad);
  const auto held = static_cast<std::uint16_t>(~state.previous_buttons_);
  int keyboard_count{};
  const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
  const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
  const auto keyboard_state =
      keyboard != nullptr && keyboard_count > 0
          ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                        keyboard_count)}
          : std::span<const std::uint8_t>{};
  const auto bound_actions = sampleKeyboardMouseActions(
      bindings, KeyboardMouseDeviceState{
                    .keyboard = keyboard_state,
                    .mouse_left = (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                    .mouse_right = (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                    .mouse_middle = (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                    .mouse_x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                    .mouse_x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                    .mouse_wheel_delta = consumePsyCrossMouseWheel(),
                });
  PsyXControllerSnapshot controller_snapshot{};
  static_cast<void>(PsyX_Pad_GetControllerSnapshot(0, &controller_snapshot));
  const auto controller_buttons = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(controller_snapshot.buttons[0]) |
      (static_cast<std::uint16_t>(controller_snapshot.buttons[1]) << 8U));
  const auto controller_identity_changed =
      controller_snapshot.connected != 0U &&
      controller_snapshot.instanceId != state.previous_controller_instance_;
  const auto controller_pressed =
      controller_identity_changed
          ? std::uint16_t{}
          : static_cast<std::uint16_t>(state.previous_controller_buttons_ &
                                       ~controller_buttons);
  auto pc_action_edge = false;
  for (std::size_t index = 0U; index < bound_actions.held.size(); ++index) {
    pc_action_edge = pc_action_edge || (bound_actions.held[index] &&
                                        !state.previous_bound_actions_.held[index]);
  }
  if (controller_snapshot.connected == 0U || pc_action_edge) {
    state.active_prompt_bindings_ = keyboardMouseInputPromptBindings(bindings);
  } else if (controller_pressed != 0U || controller_identity_changed ||
             state.active_prompt_bindings_.device == InputPromptDevice::controller) {
    state.active_prompt_bindings_ = briefingControllerPrompts(controller_snapshot);
  }
  state.previous_bound_actions_ = bound_actions;
  state.previous_controller_buttons_ = controller_buttons;
  state.previous_controller_instance_ = controller_snapshot.connected != 0U
                                     ? controller_snapshot.instanceId
                                     : -1;

#ifdef __EMSCRIPTEN__
  if (!state.preloaded_gameplay_ && !state.sync_load_done_) {
    if (state.loading_frames_remaining_ > 0) {
      --state.loading_frames_remaining_;
    } else {
      state.preloaded_gameplay_ = loadPreloadedGameplay(
          *state.mission_, state.carry_, state.initial_agent_difficulty_);
      state.sync_load_done_ = true;
      state.animation_start_ = SDL_GetPerformanceCounter();
      state.audio_clock_started_ = true;
      state.audio_start_clock_seconds_ =
          state.preloaded_audio_->playbackClockSeconds();
      PsyX_Log_Info("Mission briefing: gameplay preload complete\n");
    }
  }
#else
  if (!state.preloaded_gameplay_ &&
      state.preload_.wait_for(std::chrono::seconds{0}) ==
          std::future_status::ready) {
    state.preloaded_gameplay_ = state.preload_.get();
    state.animation_start_ = SDL_GetPerformanceCounter();
    state.audio_clock_started_ = true;
    PsyX_Log_Info("Mission briefing: gameplay preload complete\n");
  }
#endif

  const auto current_counter = SDL_GetPerformanceCounter();
  const auto elapsed =
      state.animation_start_ ? current_counter - *state.animation_start_ : 0U;
  const auto retail_time =
      state.performance_frequency_ == 0U
          ? 0.0
          : static_cast<double>(elapsed) * 20.0 /
                static_cast<double>(state.performance_frequency_);
  if (state.audio_clock_started_) {
#ifdef __EMSCRIPTEN__
    const auto audio_time = std::max(
        0.0, state.preloaded_audio_->playbackClockSeconds() -
                 state.audio_start_clock_seconds_) *
        static_cast<double>(Impl::retail_audio_callback_hz);
#else
    const auto audio_time =
        state.performance_frequency_ == 0U
            ? 0.0
            : static_cast<double>(elapsed) *
                  static_cast<double>(Impl::retail_audio_callback_hz) /
                  static_cast<double>(state.performance_frequency_);
#endif
    const auto callback_tick = static_cast<std::uint64_t>(audio_time);
    if (!state.audio_callback_tick_) {
      state.audio_callback_tick_ = callback_tick;
    }
    const auto pending_audio_updates =
        static_cast<std::size_t>(callback_tick - *state.audio_callback_tick_);
    auto audio_updates = std::uint64_t{};
    while (*state.audio_callback_tick_ < callback_tick &&
           audio_updates < Impl::maximum_audio_updates_per_iteration) {
      if (!state.preloaded_gameplay_->advanceAudioSliceClock()) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Mission briefing audio clock failed"};
      }
      ++*state.audio_callback_tick_;
      ++state.audio_slices_completed_;
      ++audio_updates;
      state.pumpAudio();
    }
    if (state.periodic_audio_diagnostics_ && state.performance_frequency_ != 0U &&
        current_counter >= state.next_audio_diagnostic_counter_) {
      ++state.audio_diagnostic_sequence_;
      state.preloaded_audio_->logDiagnostics("briefing-periodic");
      PsyX_Log_Info(
          "[AudioDiag][briefing-clock] sequence=%llu callback_tick=%llu "
          "pending=%zu slices=%llu pcm_frames=%llu pcm_blocks=%llu "
          "remaining=%llu\n",
          static_cast<unsigned long long>(state.audio_diagnostic_sequence_),
          static_cast<unsigned long long>(*state.audio_callback_tick_),
          pending_audio_updates,
          static_cast<unsigned long long>(state.audio_slices_completed_),
          static_cast<unsigned long long>(state.audio_pcm_frames_pumped_),
          static_cast<unsigned long long>(state.audio_pcm_blocks_pumped_),
          static_cast<unsigned long long>(callback_tick -
                                          *state.audio_callback_tick_));
      if (const auto guest = state.preloaded_gameplay_->audioDiagnostics()) {
        PsyX_Log_Info(
            "[AudioDiag][briefing-guest] sequence=%llu machine_tick=%llu "
            "audio_tick=%llu spu_sample=%llu mixed=%llu pcm_queued=%zu "
            "pcm_dropped=%llu cd_queued=%zu voices=%zu cd_read=%u "
            "cd_lba=%u xa_set=%u xa_file=%u xa_channel=%u\n",
            static_cast<unsigned long long>(state.audio_diagnostic_sequence_),
            static_cast<unsigned long long>(guest->machine_tick),
            static_cast<unsigned long long>(guest->audio_frame_tick),
            static_cast<unsigned long long>(guest->spu_sample_clock),
            static_cast<unsigned long long>(guest->spu_mixed_frames),
            guest->spu_pcm_frames,
            static_cast<unsigned long long>(guest->spu_dropped_pcm_frames),
            guest->spu_cd_frames, guest->active_spu_voices,
            static_cast<unsigned int>(guest->cd_reading), guest->cd_lba,
            static_cast<unsigned int>(guest->xa_stream_set),
            static_cast<unsigned int>(guest->xa_file),
            static_cast<unsigned int>(guest->xa_channel));
      }
      state.next_audio_diagnostic_counter_ = current_counter + state.performance_frequency_;
    }
  }
  const auto fade_out_elapsed =
      !state.fade_out_start_ ? 0.0
      : state.performance_frequency_ == 0U
          ? game::MissionStartGate::fade_out_seconds
          : static_cast<double>(current_counter - *state.fade_out_start_) /
                static_cast<double>(state.performance_frequency_);
  const auto fade_out_intensity =
      game::MissionStartGate::fadeOutIntensity(fade_out_elapsed);
  auto text_animation_complete = false;
  if (PsyX_BeginScene() != 0) {
    text_animation_complete = state.briefing_->draw(
        mission.briefing(), retail_time, state.active_prompt_bindings_);
    drawMissionStartFade(fade_out_intensity);
    PsyX_EndScene();
  }
  if (!state.fade_out_start_ &&
      state.gate_->update((held & confirm_buttons) != 0U ||
                              bound_actions[KeyboardMouseAction::interact],
                          text_animation_complete)) {
    state.fade_out_start_ = current_counter;
  }
  if (state.fade_out_start_ && fade_out_intensity == 0xffU &&
      state.preloaded_gameplay_) {
    PsyX_Log_Info("Mission briefing confirmed; entering gameplay\n");
    return state.previous_buttons_;
  }
  return std::nullopt;
}

std::uint16_t PsyCrossMissionStart::run(
    const game::MissionPackage &mission, PADRAW &pad,
    std::uint16_t previous_buttons, const KeyboardMouseBindings &bindings,
    std::optional<game::CampaignCarryState> carry,
    bool initial_agent_difficulty) {
  begin(mission, previous_buttons, bindings, std::move(carry),
        initial_agent_difficulty);
  for (;;) {
    if (const auto buttons = poll(pad)) {
      return *buttons;
    }
  }
}

} // namespace sf::platform::detail
