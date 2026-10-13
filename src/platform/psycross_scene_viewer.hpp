#pragma once

#include "sf/game/campaign.hpp"
#include "sf/game/pause_menu.hpp"
#include "sf/platform/host.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

struct PADRAW;

namespace sf::game {
class GameplaySession;
class MissionPackage;
} // namespace sf::game

namespace sf::platform::detail {

class PsyCrossAudioOutput;

enum class SceneExitReason {
  exit_application,
  return_to_title,
  mission_complete,
  mission_selected,
};
[[nodiscard]] InputPromptBindings
titleControllerInputPromptBindings(int controller_family);

struct SceneViewerResult {
  std::uint16_t previous_buttons{0xffffU};
  SceneExitReason reason{SceneExitReason::exit_application};
  std::optional<std::uint32_t> selected_mission;
  std::optional<game::CampaignCarryState> carry;
};

// Uses the same retail INTRFACE font page and ACD primitive path as the
// in-game pause screen. Keeping mission-complete UI on this renderer avoids
// PsyCross's debug-font VRAM page, which gameplay legitimately overwrites.
class PsyCrossCampaignSaveRenderer final {
public:
  PsyCrossCampaignSaveRenderer(const game::MissionPackage &mission,
                               KeyboardMouseBindings input);
  ~PsyCrossCampaignSaveRenderer();

  PsyCrossCampaignSaveRenderer(const PsyCrossCampaignSaveRenderer &) = delete;
  PsyCrossCampaignSaveRenderer &
  operator=(const PsyCrossCampaignSaveRenderer &) = delete;

  void draw(const game::CampaignSaveMenu &menu,
            const game::TitleSaveSlots &slots);
  void drawLoadSlots(const game::TitleSaveSlots &slots, std::size_t selection);
  void drawDifficultySelection(const game::TitleMenu &menu);
  void drawAgentModeWarning();
  void setInputPromptBindings(InputPromptBindings bindings);

private:
  struct State;
  std::unique_ptr<State> state_;
};

class PsyCrossSceneViewer final {
public:
  PsyCrossSceneViewer(
      KeyboardMouseBindings input, game::RetailCheatState &cheats,
      game::CampaignDifficulty difficulty,
      ControllerButtonBindings controller_bindings = {},
      bool controller_vibration = true,
      ControllerSettingsCommitCallback controller_settings_commit = {},
      bool mission_skyboxes = true) noexcept;

  // Starts (or restarts) a mission view. Replaces any previous session, so
  // the viewer object itself stays reusable across missions.
  void begin(const game::MissionPackage &mission,
             std::uint16_t previous_buttons,
             const std::filesystem::path &cue_path,
             std::uint32_t maximum_unlocked_mission,
             std::unique_ptr<game::GameplaySession> preloaded_gameplay = {},
             std::unique_ptr<PsyCrossAudioOutput> preloaded_audio = {});

  // Presents at most one frame. Resolves to a result when the mission ends,
  // returns to the title, or selects another mission.
  [[nodiscard]] std::optional<SceneViewerResult> poll(PADRAW &pad);

  [[nodiscard]] SceneViewerResult
  run(const game::MissionPackage &mission, PADRAW &pad,
      std::uint16_t previous_buttons, const std::filesystem::path &cue_path,
      std::uint32_t maximum_unlocked_mission,
      std::unique_ptr<game::GameplaySession> preloaded_gameplay = {},
      std::unique_ptr<PsyCrossAudioOutput> preloaded_audio = {});

  ~PsyCrossSceneViewer();

private:
  // Pollable per-run state. Rebuilt by every begin(); the viewer holds only
  // cross-mission configuration. Declared here, defined in the runtime
  // translation unit where the renderer types are complete.
  struct Session;
  std::unique_ptr<Session> session_;
  KeyboardMouseBindings input_;
  game::RetailCheatState &cheats_;
  game::CampaignDifficulty difficulty_{game::CampaignDifficulty::original};
  ControllerButtonBindings controller_bindings_;
  bool controller_vibration_{true};
  ControllerSettingsCommitCallback controller_settings_commit_;
  bool mission_skyboxes_{true};
  game::PauseSettings pause_settings_;
  bool pause_settings_initialized_{};
};

} // namespace sf::platform::detail
