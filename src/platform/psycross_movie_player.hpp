#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct PADRAW;

namespace sf::game {
struct DiscMovie;
class TitleMovies;
}

namespace sf::platform::detail {

enum class StandaloneMovieSkipPolicy : std::uint8_t {
    allow,
    prevent,
};

struct MovieOverlayCallbacks {
    std::function<bool(std::uint16_t, std::uint32_t)> update;
    std::function<void()> draw;
    std::function<const game::DiscMovie*()> transition_movie;
};

// Pollable single-movie playback. A frame scheduler that cannot block owns one
// and polls it once per presented frame; the blocking playStandalone below is
// a thin wrapper over it.
class StandaloneMovieSession final {
public:
    StandaloneMovieSession(const game::DiscMovie& movie,
                           std::uint16_t previous_buttons,
                           StandaloneMovieSkipPolicy skip_policy =
                               StandaloneMovieSkipPolicy::allow);
    ~StandaloneMovieSession();
    StandaloneMovieSession(StandaloneMovieSession&&) noexcept;
    StandaloneMovieSession& operator=(StandaloneMovieSession&&) noexcept;
    StandaloneMovieSession(const StandaloneMovieSession&) = delete;
    StandaloneMovieSession& operator=(const StandaloneMovieSession&) = delete;

    // Advances playback by at most one presented frame. Returns the updated
    // pad buttons once the movie finishes or is skipped.
    [[nodiscard]] std::optional<std::uint16_t> poll(PADRAW& pad);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pollable title-movie sequence (startup movies, looping background, optional
// transition). Same ownership model as StandaloneMovieSession; the blocking
// play below is a thin wrapper over it.
class TitleMovieSession final {
public:
    TitleMovieSession(game::TitleMovies& movies,
                      std::uint16_t previous_buttons,
                      const MovieOverlayCallbacks& overlay,
                      bool play_startup_movies = true);
    ~TitleMovieSession();
    TitleMovieSession(TitleMovieSession&&) noexcept;
    TitleMovieSession& operator=(TitleMovieSession&&) noexcept;
    TitleMovieSession(const TitleMovieSession&) = delete;
    TitleMovieSession& operator=(const TitleMovieSession&) = delete;

    // Advances playback by at most one presented frame. Returns the updated
    // pad buttons once the overlay dismisses the sequence.
    [[nodiscard]] std::optional<std::uint16_t> poll(PADRAW& pad);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PsyCrossMoviePlayer final {
public:
    [[nodiscard]] std::uint16_t playStandalone(
        const game::DiscMovie& movie,
        PADRAW& pad,
        std::uint16_t previous_buttons,
        StandaloneMovieSkipPolicy skip_policy =
            StandaloneMovieSkipPolicy::allow);

    [[nodiscard]] std::uint16_t play(
        game::TitleMovies& movies,
        PADRAW& pad,
        std::uint16_t previous_buttons,
        const MovieOverlayCallbacks& overlay,
        bool play_startup_movies = true);
};

} // namespace sf::platform::detail
