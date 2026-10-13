// WASM shell (Emscripten only): boots the title host from a disc staged in
// MEMFS and moves the native campaign save file in and out for the page.
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"
#include "sf/platform/host.hpp"

#include <emscripten.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

constexpr char kStageDir[] = "/sf_disc";
constexpr char kSaveSerial[] = "SCUS-94240";
constexpr char kImportPath[] = "/sf_save_transfer/import.sav";
constexpr char kExportPath[] = "/sf_save_transfer/export.sav";

// The active host must outlive the browser frame callbacks.
std::unique_ptr<sf::platform::Host> g_host;

void setStatus(const char *message) {
  EM_ASM({ document.getElementById('sf-status').textContent = UTF8ToString($0); },
         message);
}

std::filesystem::path campaignSavePath() {
  // The serial selects the same user-data save path used by the title host.
  // The CUE name only affects the legacy migration path, not this primary.
  return sf::game::defaultTitleSaveLocation(
             std::filesystem::path{kStageDir} / "game.cue", kSaveSerial).primary;
}

bool usableSave(const sf::game::TitleSaveLoadResult &save) {
  return save.status == sf::game::TitleSaveLoadStatus::loaded ||
         save.status == sf::game::TitleSaveLoadStatus::recovered;
}

std::filesystem::path stagedCue() {
  for (const auto &entry : std::filesystem::directory_iterator(kStageDir)) {
    auto extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (extension == ".cue") {
      return entry.path();
    }
  }
  return {};
}

bool canBoot() {
  if (g_host) {
    setStatus("Already running. Reload to restart.");
    return false;
  }
  return true;
}

} // namespace

extern "C" {

// The page keeps a copy of the campaign save in localStorage. These two move
// the native save file to and from /sf_save_transfer for it.
// Import (before launch): the shared loader enforces its size limit and
// validates every slot; only a complete primary file is accepted.
EMSCRIPTEN_KEEPALIVE int sf_import_save() {
  try {
    if (g_host) {
      return 0;
    }
    const auto imported = sf::game::loadTitleSaveSlotsFile(kImportPath);
    return imported.status == sf::game::TitleSaveLoadStatus::loaded &&
                   sf::game::storeTitleSaveSlotsFile(campaignSavePath(),
                                                     imported.slots)
               ? 1
               : 0;
  } catch (const std::exception &error) {
    std::cerr << "save import failed: " << error.what() << '\n';
    return 0;
  }
}

// Export: writes a canonical copy of the current save (all slots, backup
// recovered if needed) and returns 1 when a usable save exists.
EMSCRIPTEN_KEEPALIVE int sf_export_save() {
  try {
    const auto save = sf::game::loadTitleSaveSlotsFile(campaignSavePath());
    return usableSave(save) &&
                   sf::game::storeTitleSaveSlotsFile(kExportPath, save.slots)
               ? 1
               : 0;
  } catch (const std::exception &error) {
    std::cerr << "save export failed: " << error.what() << '\n';
    return 0;
  }
}

// Starts the campaign frontend; returns once the browser main loop is scheduled.
EMSCRIPTEN_KEEPALIVE int sf_boot_game() {
  if (!canBoot()) {
    return 0;
  }
  try {
    const auto cue = stagedCue();
    if (cue.empty()) {
      setStatus("Select the CUE file.");
      return 0;
    }
    auto disc = sf::game::GameDisc::open(cue);
    if (!disc.game()) {
      setStatus("Unsupported disc. USA v1.1 (SCUS-94240) required.");
      return 0;
    }
    auto assets = sf::game::TitleAssets::load(disc);
    auto movies = sf::game::TitleMovies::load(disc);
    auto mission = sf::game::MissionPackage::load(disc, 0U);
    g_host = sf::platform::createPsyCrossTitleHost(
        "Syphon Filter PC (WASM)", std::move(assets), std::move(movies),
        std::move(mission), cue, std::string{disc.game()->serial});
    setStatus("");
    g_host->run();
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "title boot failed: " << error.what() << '\n';
    setStatus("Launch failed.");
    return 0;
  }
}

} // extern "C"

int main() {
  std::error_code stage_error;
  std::filesystem::create_directories(kStageDir, stage_error);
  if (stage_error) {
    std::cerr << "cannot create stage dir: " << stage_error.message() << '\n';
    setStatus("Filesystem unavailable.");
    return 1;
  }
  return 0;
}
