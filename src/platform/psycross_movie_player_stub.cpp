#include "psycross_movie_player.hpp"
#include "psycross_video_mode.hpp"

#include "sf/core/error.hpp"
#include "sf/game/disc_movie.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <psx/libpad.h>

#include <utility>

namespace sf::platform::detail {

// With media disabled, standalone movies are omitted. Keep the same session
// interface so hosts can use the same flow with or without FFmpeg.
struct StandaloneMovieSession::Impl {
    std::uint16_t previous_buttons;
};

StandaloneMovieSession::StandaloneMovieSession(
    const game::DiscMovie& movie, std::uint16_t previous_buttons,
    StandaloneMovieSkipPolicy /*skip_policy*/)
    : impl_(std::make_unique<Impl>(Impl{previous_buttons})) {
    PsyX_Log_Info("Movie omitted (media disabled): %s\n", movie.path.c_str());
}

StandaloneMovieSession::~StandaloneMovieSession() = default;
StandaloneMovieSession::StandaloneMovieSession(StandaloneMovieSession&&) noexcept = default;
StandaloneMovieSession& StandaloneMovieSession::operator=(StandaloneMovieSession&&) noexcept = default;

std::optional<std::uint16_t> StandaloneMovieSession::poll(PADRAW& /*pad*/) {
    return impl_->previous_buttons;
}

struct TitleMovieSession::Impl {
    Impl(std::uint16_t buttons, const MovieOverlayCallbacks& callbacks)
        : previous_buttons(buttons), overlay(callbacks),
          video_mode(movie_video_mode, gameplay_video_mode) {
        if (!overlay.update || !overlay.draw) {
            throw core::Error{core::ErrorCode::invalid_argument,
                              "Title overlay callbacks are missing"};
        }
    }

    std::optional<std::uint16_t> poll(PADRAW& pad) {
        PsyX_UpdateInput();
        const auto buttons = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(pad.buttons[0]) |
            (static_cast<std::uint16_t>(pad.buttons[1]) << 8U));
        const auto pressed = static_cast<std::uint16_t>(~buttons & previous_buttons);
        previous_buttons = buttons;
        // A fixed movie frame keeps the menu visible without the authored
        // TITLE.STR fade; TitleMenu still advances its search and input state.
        if (!overlay.update(pressed, 0U)) {
            return previous_buttons;
        }
        if (PsyX_BeginScene() != 0) {
            GR_Clear(0, 0, 320, 240, 0, 0, 0);
            overlay.draw();
            // Title sprites queue DrawPrim commands. Font-backed submenus
            // flush themselves, but the main menu needs the same flush as
            // the full movie player's presentation path.
            DrawSync(0);
            PsyX_EndScene();
        }
        return std::nullopt;
    }

    std::uint16_t previous_buttons;
    MovieOverlayCallbacks overlay;
    ScopedPsyCrossVideoMode video_mode;
};

TitleMovieSession::TitleMovieSession(
    game::TitleMovies& /*movies*/, std::uint16_t previous_buttons,
    const MovieOverlayCallbacks& overlay, bool /*play_startup_movies*/)
    : impl_(std::make_unique<Impl>(previous_buttons, overlay)) {}

TitleMovieSession::~TitleMovieSession() = default;
TitleMovieSession::TitleMovieSession(TitleMovieSession&&) noexcept = default;
TitleMovieSession& TitleMovieSession::operator=(TitleMovieSession&&) noexcept = default;

std::optional<std::uint16_t> TitleMovieSession::poll(PADRAW& pad) {
    return impl_->poll(pad);
}

std::uint16_t PsyCrossMoviePlayer::playStandalone(
    const game::DiscMovie& movie, PADRAW& pad, std::uint16_t previous_buttons,
    StandaloneMovieSkipPolicy skip_policy) {
    StandaloneMovieSession session{movie, previous_buttons, skip_policy};
    return *session.poll(pad);
}

std::uint16_t PsyCrossMoviePlayer::play(
    game::TitleMovies& movies, PADRAW& pad, std::uint16_t previous_buttons,
    const MovieOverlayCallbacks& overlay, bool play_startup_movies) {
    TitleMovieSession session{movies, previous_buttons, overlay, play_startup_movies};
    for (;;) {
        if (const auto buttons = session.poll(pad)) {
            return *buttons;
        }
    }
}

} // namespace sf::platform::detail
