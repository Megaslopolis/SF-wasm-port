#pragma once

#include "sf/game/gameplay.hpp"
#include "sf/platform/player_input.hpp"

#include <cstdint>
#include <memory>
#include <optional>

struct PADRAW;

namespace sf::game {
class MissionPackage;
}

namespace sf::platform::detail {

class PsyCrossAudioOutput;

// Pollable mission-briefing session. A frame scheduler that cannot block calls
// begin() once, then owns the session and polls it once per presented frame;
// the blocking run() below is a thin wrapper over the same calls.
//
// Lifetime: the referenced mission must outlive the session, and on hosts the
// session member must be declared after the mission it reads, so an in-flight
// preload join (desktop thread, or nothing on WASM) can never observe a dead
// mission.
class PsyCrossMissionStart final {
public:
    PsyCrossMissionStart();
    ~PsyCrossMissionStart();

    // Starts (or restarts) the briefing for a mission. Resets all per-run
    // state; safe to call again for the next mission.
    void begin(const game::MissionPackage &mission,
               std::uint16_t previous_buttons,
               const KeyboardMouseBindings &bindings,
               std::optional<game::CampaignCarryState> carry = std::nullopt,
               bool initial_agent_difficulty = false);

    // Presents at most one briefing frame. Returns the updated pad buttons
    // once the briefing is confirmed and the preload is ready.
    [[nodiscard]] std::optional<std::uint16_t> poll(PADRAW &pad);

    [[nodiscard]] std::uint16_t
    run(const game::MissionPackage &mission, PADRAW &pad,
        std::uint16_t previous_buttons, const KeyboardMouseBindings &bindings,
        std::optional<game::CampaignCarryState> carry = std::nullopt,
        bool initial_agent_difficulty = false);

    [[nodiscard]] std::unique_ptr<game::GameplaySession>
    takePreloadedGameplay() noexcept;
    [[nodiscard]] std::unique_ptr<PsyCrossAudioOutput>
    takePreloadedAudio() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sf::platform::detail
