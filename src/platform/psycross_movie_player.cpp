#include "psycross_movie_player.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_video_mode.hpp"

#include "sf/core/error.hpp"
#include "sf/game/disc_movie.hpp"
#include "sf/game/title.hpp"
#include "sf/media/str_decoder.hpp"
#include "sf/platform/audio_output_policy.hpp"

#include <AL/al.h>
#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL_timer.h>
#include <psx/libetc.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>
#include <psx/libpad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sf::platform::detail {
namespace {

constexpr std::uint16_t skip_buttons =
    0x8000U | 0x4000U | 0x2000U | 0x1000U | 0x08U | 0x01U;

std::uint16_t readButtons(const PADRAW& pad) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(pad.buttons[0]) |
        (static_cast<std::uint16_t>(pad.buttons[1]) << 8U));
}

void requireAl(const char* operation) {
    const auto error = alGetError();
    if (error != AL_NO_ERROR) {
        throw core::Error{
            core::ErrorCode::io,
            std::string{operation} + " (OpenAL error " + std::to_string(error) + ')'};
    }
}

class MovieAudioPlayer final {
public:
    MovieAudioPlayer() {
        buffers_.reserve(maximum_queued_buffers);
        available_.reserve(maximum_queued_buffers);
        staged_samples_.reserve(
            maximum_queued_buffers * sample_frames_per_buffer * 2U);
        alGetError();
        alGenSources(1, &source_);
        try {
            requireAl("Cannot create movie audio source");
            alSourcei(source_, AL_SOURCE_RELATIVE, AL_TRUE);
            requireAl("Cannot configure movie audio source");
        } catch (...) {
            if (source_ != 0U) {
                alDeleteSources(1, &source_);
                source_ = 0U;
            }
            throw;
        }
    }

    ~MovieAudioPlayer() {
        if (source_ == 0) {
            return;
        }
        reset();
        alDeleteSources(1, &source_);
        if (!buffers_.empty()) {
            alDeleteBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
        }
    }

    MovieAudioPlayer(const MovieAudioPlayer&) = delete;
    MovieAudioPlayer& operator=(const MovieAudioPlayer&) = delete;

    void queue(const media::MovieAudioChunk& chunk) {
        if (chunk.stereo_samples.empty()) {
            return;
        }
        if (chunk.sample_rate <= 0 ||
            (chunk.stereo_samples.size() % 2U) != 0U ||
            chunk.stereo_samples.size() >
                static_cast<std::size_t>(std::numeric_limits<ALsizei>::max()) /
                    sizeof(std::int16_t)) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid movie audio chunk"};
        }

        if (input_finished_) {
            throw core::Error{
                core::ErrorCode::invalid_argument,
                "Movie audio was queued after end of stream"};
        }
        if (sample_rate_ == 0) {
            sample_rate_ = chunk.sample_rate;
        } else if (sample_rate_ != chunk.sample_rate) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "STR audio sample rate changed during playback"};
        }

        compactStaging();
        staged_samples_.insert(
            staged_samples_.end(),
            chunk.stereo_samples.begin(),
            chunk.stereo_samples.end());
        collectProcessed();
        uploadReadyBuffers(false);
        startIfNeeded();
    }

    void start() {
        start_requested_ = true;
        startIfNeeded();
    }

    void finishInput() {
        if (input_finished_) {
            return;
        }
        input_finished_ = true;
        collectProcessed();
        uploadReadyBuffers(true);
        startIfNeeded();
    }

    void update() {
        collectProcessed();
        uploadReadyBuffers(input_finished_);
        startIfNeeded();
    }

    [[nodiscard]] bool empty() const {
        ALint queued = 0;
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        requireAl("Cannot query movie audio queue");
        return queued == 0 && staged_offset_ == staged_samples_.size();
    }

    void reset() noexcept {
        alSourceStop(source_);
        ALint queued = 0;
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        while (queued-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source_, 1, &buffer);
            if (alGetError() != AL_NO_ERROR) {
                break;
            }
            if (available_.size() < available_.capacity()) {
                available_.push_back(buffer);
            }
        }
        staged_samples_.clear();
        staged_offset_ = 0U;
        sample_rate_ = 0;
        start_requested_ = false;
        input_finished_ = false;
        start_policy_.reset();
        alGetError();
    }

private:
    static constexpr std::size_t sample_frames_per_buffer = 1024U;
    static constexpr std::size_t maximum_queued_buffers = 16U;

    void uploadReadyBuffers(bool flush_partial) {
        while (queuedBufferCount() < maximum_queued_buffers) {
            const auto remaining_samples =
                staged_samples_.size() - staged_offset_;
            const auto remaining_frames = remaining_samples / 2U;
            if (remaining_frames < sample_frames_per_buffer &&
                (!flush_partial || remaining_frames == 0U)) {
                break;
            }
            const auto frame_count =
                std::min(remaining_frames, sample_frames_per_buffer);
            uploadBuffer(std::span<const std::int16_t>{staged_samples_}.subspan(
                staged_offset_, frame_count * 2U));
            staged_offset_ += frame_count * 2U;
        }
        compactStaging();
    }

    void uploadBuffer(std::span<const std::int16_t> samples) {
        ALuint buffer = 0;
        if (available_.empty()) {
            if (buffers_.size() >= maximum_queued_buffers) {
                throw core::Error{
                    core::ErrorCode::io,
                    "Movie audio buffer pool lost a recycled buffer"};
            }
            alGenBuffers(1, &buffer);
            try {
                requireAl("Cannot create movie audio buffer");
                buffers_.push_back(buffer);
            } catch (...) {
                if (buffer != 0U) {
                    alDeleteBuffers(1, &buffer);
                }
                throw;
            }
        } else {
            buffer = available_.back();
            available_.pop_back();
        }

        const auto byte_count = static_cast<ALsizei>(
            samples.size() * sizeof(std::int16_t));
        alBufferData(
            buffer,
            AL_FORMAT_STEREO16,
            samples.data(),
            byte_count,
            sample_rate_);
        requireAl("Cannot upload movie audio");
        alSourceQueueBuffers(source_, 1, &buffer);
        requireAl("Cannot queue movie audio");
    }

    void compactStaging() {
        if (staged_offset_ == 0U) {
            return;
        }
        if (staged_offset_ == staged_samples_.size()) {
            staged_samples_.clear();
            staged_offset_ = 0U;
            return;
        }
        if (staged_offset_ >= sample_frames_per_buffer * 2U &&
            staged_offset_ * 2U >= staged_samples_.size()) {
            staged_samples_.erase(
                staged_samples_.begin(),
                staged_samples_.begin() +
                    static_cast<std::ptrdiff_t>(staged_offset_));
            staged_offset_ = 0U;
        }
    }

    [[nodiscard]] std::size_t queuedBufferCount() const {
        ALint queued = 0;
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        requireAl("Cannot query movie audio queue");
        return queued > 0 ? static_cast<std::size_t>(queued) : 0U;
    }

    void collectProcessed() {
        ALint processed = 0;
        alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
        requireAl("Cannot query processed movie audio");
        while (processed-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source_, 1, &buffer);
            requireAl("Cannot recycle movie audio buffer");
            if (available_.size() >= available_.capacity()) {
                throw core::Error{
                    core::ErrorCode::io,
                    "Movie audio recycle queue exceeded its bound"};
            }
            available_.push_back(buffer);
        }
    }

    void startIfNeeded() {
        if (!start_requested_) {
            return;
        }
        ALint queued = 0;
        ALint state = AL_STOPPED;
        alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
        alGetSourcei(source_, AL_SOURCE_STATE, &state);
        requireAl("Cannot query movie audio state");
        const auto queued_count =
            queued > 0 ? static_cast<std::size_t>(queued) : 0U;
        const auto force_short_stream =
            input_finished_ && !start_policy_.started() && queued_count != 0U;
        if (state != AL_PLAYING &&
            (force_short_stream ||
             start_policy_.shouldStart(queued_count, false))) {
            alSourcePlay(source_);
            requireAl("Cannot start movie audio");
        } else {
            static_cast<void>(
                start_policy_.shouldStart(queued_count, state == AL_PLAYING));
        }
    }

    ALuint source_{};
    std::vector<ALuint> buffers_;
    std::vector<ALuint> available_;
    std::vector<std::int16_t> staged_samples_;
    std::size_t staged_offset_{};
    int sample_rate_{};
    bool start_requested_{};
    bool input_finished_{};
    platform::AudioOutputStartPolicy start_policy_{2U};
};

class BufferedMovieStream final {
public:
    explicit BufferedMovieStream(std::span<const std::byte> sectors)
        : decoder_(media::StrDecoder::open(sectors)) {}

    [[nodiscard]] double framesPerSecond() const noexcept {
        return decoder_.framesPerSecond();
    }

    void fill(std::size_t target_video_frames, MovieAudioPlayer& audio) {
        while (!finished_ && video_frames_.size() < target_video_frames) {
            auto event = decoder_.next();
            if (!event) {
                finished_ = true;
                audio.finishInput();
                break;
            }
            if (auto* frame = std::get_if<media::MovieVideoFrame>(&*event)) {
                video_frames_.emplace_back(std::move(*frame));
            } else {
                audio.queue(std::get<media::MovieAudioChunk>(*event));
            }
        }
    }

    [[nodiscard]] bool hasVideoFrame() const noexcept {
        return !video_frames_.empty();
    }

    [[nodiscard]] std::optional<double> nextVideoTimestamp() const noexcept {
        if (video_frames_.empty()) {
            return std::nullopt;
        }
        return video_frames_.front().timestamp_seconds;
    }

    [[nodiscard]] media::MovieVideoFrame takeVideoFrame() {
        auto result = std::move(video_frames_.front());
        video_frames_.pop_front();
        return result;
    }

private:
    media::StrDecoder decoder_;
    std::deque<media::MovieVideoFrame> video_frames_;
    bool finished_{};
};

struct MoviePlaybackClock {
    std::uint64_t frequency{};
    std::uint64_t started{};
    bool running{};

    void start() noexcept {
        frequency = SDL_GetPerformanceFrequency();
        started = SDL_GetPerformanceCounter();
        running = true;
    }

    [[nodiscard]] double elapsedSeconds() const noexcept {
        if (frequency == 0U) {
            return std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(SDL_GetPerformanceCounter() - started) /
               static_cast<double>(frequency);
    }
};

class MovieVideoTexture final {
public:
    ~MovieVideoTexture() { release(); }

    MovieVideoTexture(const MovieVideoTexture&) = delete;
    MovieVideoTexture& operator=(const MovieVideoTexture&) = delete;
    MovieVideoTexture() = default;

    void upload(const media::MovieVideoFrame& frame) {
        const auto expected_bytes =
            static_cast<std::size_t>(frame.width) *
            static_cast<std::size_t>(frame.height) * 4U;
        if (frame.width <= 0 || frame.height <= 0 ||
            frame.width > movie_video_mode.width ||
            frame.height > movie_video_mode.height ||
            frame.rgba8888.size() != expected_bytes) {
            throw core::Error{
                core::ErrorCode::unsupported,
                "Unsupported STR video dimensions"};
        }

        if (frame.width != width_ || frame.height != height_) {
            release();
            width_ = frame.width;
            height_ = frame.height;
            slice_count_ =
                (frame.width + maximum_slice_width - 1) / maximum_slice_width;
            for (int index = 0; index < slice_count_; ++index) {
                auto& slice = slices_[static_cast<std::size_t>(index)];
                slice.source_x = index * maximum_slice_width;
                slice.width =
                    std::min(maximum_slice_width, frame.width - slice.source_x);
                slice.pixels.resize(
                    static_cast<std::size_t>(slice.width) *
                    static_cast<std::size_t>(frame.height) * 4U);
            }
        }

        for (int index = 0; index < slice_count_; ++index) {
            auto& slice = slices_[static_cast<std::size_t>(index)];
            const auto row_bytes = static_cast<std::size_t>(slice.width) * 4U;
            for (int row = 0; row < frame.height; ++row) {
                const auto source_offset =
                    (static_cast<std::size_t>(row) *
                         static_cast<std::size_t>(frame.width) +
                     static_cast<std::size_t>(slice.source_x)) *
                    4U;
                const auto destination_offset =
                    static_cast<std::size_t>(row) * row_bytes;
                std::memcpy(
                    slice.pixels.data() + destination_offset,
                    frame.rgba8888.data() + source_offset,
                    row_bytes);
            }
            if (slice.texture == 0U) {
                slice.texture = GR_CreateRGBATexture(
                    slice.width, frame.height, slice.pixels.data());
            } else {
                GR_UpdateRGBATexture(
                    slice.texture,
                    slice.width,
                    frame.height,
                    slice.pixels.data());
            }
        }
    }

    void draw() const {
        if (slice_count_ == 0) {
            return;
        }
        const auto screen_x =
            static_cast<short>((movie_video_mode.width - width_) / 2);
        const auto screen_y =
            static_cast<short>((movie_video_mode.height - height_) / 2);

        DR_TPAGE page{};
        // Native RGBA video stays outside the 15-bit PSX VRAM path. DFE remains
        // enabled so title overlays share the same 320x240 presentation target.
        SetDrawTPage(&page, 1, 0, GetTPage(2, 0, 0, 0));
        DrawPrim(&page);

        for (int index = 0; index < slice_count_; ++index) {
            const auto& slice = slices_[static_cast<std::size_t>(index)];
            DR_PSYX_TEX native_texture{};
            SetPsyXTexture(
                &native_texture, slice.texture, slice.width, height_);
            DrawPrim(&native_texture);

            SPRT sprite{};
            SetSprt(&sprite);
            setRGB0(&sprite, 128, 128, 128);
            setXY0(
                &sprite,
                static_cast<short>(screen_x + slice.source_x),
                screen_y);
            setWH(
                &sprite,
                static_cast<short>(slice.width),
                static_cast<short>(height_));
            sprite.u0 = 0;
            sprite.v0 = 0;
            DrawPrim(&sprite);
        }

        DR_PSYX_TEX restore_vram{};
        SetPsyXTexture(&restore_vram, 0U, 0, 0);
        DrawPrim(&restore_vram);
    }

private:
    static constexpr int maximum_slice_width = 160;

    struct Slice {
        TextureID texture{};
        int source_x{};
        int width{};
        std::vector<std::uint8_t> pixels;
    };

    void release() noexcept {
        for (auto& slice : slices_) {
            if (slice.texture != 0U) {
                GR_DestroyTexture(slice.texture);
            }
            slice = {};
        }
        width_ = 0;
        height_ = 0;
        slice_count_ = 0;
    }

    std::array<Slice, 2U> slices_{};
    int width_{};
    int height_{};
    int slice_count_{};
};

constexpr double gameplay_movie_fade_seconds = 0.12;

void drawMovieFade(std::uint8_t intensity) {
    if (intensity == 0U) {
        return;
    }
    GR_SetBlendMode(BM_SUBTRACT);
    DR_TPAGE page{};
    SetDrawTPage(&page, 1, 0, GetTPage(0, 2, 0, 0));
    DrawPrim(&page);
    TILE tile{};
    setTile(&tile);
    setSemiTrans(&tile, 1);
    setRGB0(&tile, intensity, intensity, intensity);
    setXY0(&tile, 0, 0);
    setWH(&tile, static_cast<float>(movie_video_mode.width),
          static_cast<float>(movie_video_mode.height));
    DrawPrim(&tile);
    GR_SetBlendMode(BM_NONE);
}

void presentMovieFrame(
    const MovieVideoTexture& video_texture,
    const std::function<void()>& draw_overlay = {},
    std::uint8_t fade_intensity = 0U) {
    if (PsyX_BeginScene() == 0) {
        // Recover a stale implicit scene instead of silently dropping every
        // decoded video frame while its audio continues.
        PsyX_EndScene();
        if (PsyX_BeginScene() == 0) {
            return;
        }
    }
    video_texture.draw();
    if (draw_overlay) {
        draw_overlay();
    }
    drawMovieFade(fade_intensity);
    DrawSync(0);
    PsyX_EndScene();
}

std::uint16_t updateInput(PADRAW& pad, std::uint16_t& previous_buttons) {
    PsyX_UpdateInput();
    const auto buttons = readButtons(pad);
    const auto pressed = static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    return pressed;
}

// Presents one video frame per poll until its end time, without blocking.
class VideoFrameWait {
public:
    VideoFrameWait(const media::MovieVideoFrame& frame,
                   MovieVideoTexture& video_texture,
                   double end_time_seconds,
                   MoviePlaybackClock& clock,
                   MovieAudioPlayer& audio,
                   bool allow_skip,
                   std::uint8_t& fade_intensity,
                   bool fade_gameplay_movie)
        : frame_(frame), video_texture_(video_texture),
          end_time_seconds_(end_time_seconds), clock_(clock), audio_(audio),
          allow_skip_(allow_skip), fade_intensity_(fade_intensity),
          fade_gameplay_movie_(fade_gameplay_movie) {}

    // Uploads on first poll, then presents one scene per poll. Returns true
    // when the frame's end time is reached, false on a skip edge, or
    // std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!uploaded_) {
            video_texture_.upload(frame_);
            if (!clock_.running) {
                clock_.start();
                audio_.start();
            }
            uploaded_ = true;
        }
        const auto pressed = updateInput(pad, previous_buttons);
        audio_.update();
        const auto fade_progress = std::clamp(
            clock_.elapsedSeconds() / gameplay_movie_fade_seconds, 0.0, 1.0);
        fade_intensity_ = static_cast<std::uint8_t>(
            fade_gameplay_movie_
                ? std::clamp<long>(
                      std::lround((1.0 - fade_progress) * 255.0), 0L, 255L)
                : 0L);
        presentMovieFrame(video_texture_, {}, fade_intensity_);
        if (allow_skip_ && (pressed & skip_buttons) != 0) {
            return false;
        }
        if (clock_.elapsedSeconds() >= end_time_seconds_) {
            return true;
        }
        return std::nullopt;
    }

private:
    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    double end_time_seconds_;
    MoviePlaybackClock& clock_;
    MovieAudioPlayer& audio_;
    bool allow_skip_;
    std::uint8_t& fade_intensity_;
    bool fade_gameplay_movie_;
    bool uploaded_{};
};


// Presents one overlay frame per poll until its end time, without blocking.
class OverlayVideoFrameWait {
public:
    OverlayVideoFrameWait(const media::MovieVideoFrame& frame,
                          MovieVideoTexture& video_texture,
                          std::uint32_t movie_frame,
                          double end_time_seconds,
                          MoviePlaybackClock& clock,
                          MovieAudioPlayer& audio,
                          const MovieOverlayCallbacks& overlay)
        : frame_(frame), video_texture_(video_texture),
          movie_frame_(movie_frame), end_time_seconds_(end_time_seconds),
          clock_(clock), audio_(audio), overlay_(overlay) {}

    // Uploads on first poll, then presents one scene per poll. Returns true
    // when the frame's end time is reached, false when the overlay dismisses
    // playback, or std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!uploaded_) {
            video_texture_.upload(frame_);
            if (!clock_.running) {
                clock_.start();
                audio_.start();
            }
            uploaded_ = true;
        }
        const auto pressed = updateInput(pad, previous_buttons);
        audio_.update();
        if (!overlay_.update(pressed, movie_frame_)) {
            return false;
        }
        presentMovieFrame(video_texture_, overlay_.draw);
        if (clock_.elapsedSeconds() >= end_time_seconds_) {
            return true;
        }
        return std::nullopt;
    }

private:
    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    std::uint32_t movie_frame_;
    double end_time_seconds_;
    MoviePlaybackClock& clock_;
    MovieAudioPlayer& audio_;
    const MovieOverlayCallbacks& overlay_;
    bool uploaded_{};
};


// Holds one video frame on screen per poll for a fixed duration, without
// blocking.
class VideoFrameHold {
public:
    VideoFrameHold(const media::MovieVideoFrame& frame,
                   MovieVideoTexture& video_texture,
                   double duration_seconds,
                   MovieAudioPlayer& audio,
                   bool allow_skip,
                   std::uint8_t fade_intensity = 0U)
        : frame_(frame), video_texture_(video_texture),
          duration_seconds_(duration_seconds), audio_(audio),
          allow_skip_(allow_skip), fade_intensity_(fade_intensity) {}

    // Uploads on first poll, then presents one scene per poll. Returns true
    // when the hold duration elapses (or the performance counter is
    // unavailable), false on a skip edge, or std::nullopt to hold another
    // frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!uploaded_) {
            video_texture_.upload(frame_);
            frequency_ = SDL_GetPerformanceFrequency();
            started_ = SDL_GetPerformanceCounter();
            uploaded_ = true;
        }
        const auto pressed = updateInput(pad, previous_buttons);
        audio_.update();
        presentMovieFrame(video_texture_, {}, fade_intensity_);
        if (allow_skip_ && (pressed & skip_buttons) != 0) {
            return false;
        }
        if (frequency_ == 0U ||
            static_cast<double>(SDL_GetPerformanceCounter() - started_) /
                    static_cast<double>(frequency_) >=
                duration_seconds_) {
            return true;
        }
        return std::nullopt;
    }

private:
    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    double duration_seconds_;
    MovieAudioPlayer& audio_;
    bool allow_skip_;
    std::uint8_t fade_intensity_;
    std::uint64_t frequency_{};
    std::uint64_t started_{};
    bool uploaded_{};
};


// Holds one overlay frame on screen per poll for a fixed duration, without
// blocking.
class OverlayVideoFrameHold {
public:
    OverlayVideoFrameHold(const media::MovieVideoFrame& frame,
                          MovieVideoTexture& video_texture,
                          std::uint32_t movie_frame,
                          double duration_seconds,
                          MovieAudioPlayer& audio,
                          const MovieOverlayCallbacks& overlay)
        : frame_(frame), video_texture_(video_texture),
          movie_frame_(movie_frame), duration_seconds_(duration_seconds),
          audio_(audio), overlay_(overlay) {}

    // Uploads on first poll, then presents one scene per poll. Returns true
    // when the hold duration elapses (or the performance counter is
    // unavailable), false when the overlay dismisses playback, or
    // std::nullopt to hold another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!uploaded_) {
            video_texture_.upload(frame_);
            frequency_ = SDL_GetPerformanceFrequency();
            started_ = SDL_GetPerformanceCounter();
            uploaded_ = true;
        }
        const auto pressed = updateInput(pad, previous_buttons);
        audio_.update();
        if (!overlay_.update(pressed, movie_frame_)) {
            return false;
        }
        presentMovieFrame(video_texture_, overlay_.draw);
        if (frequency_ == 0U ||
            static_cast<double>(SDL_GetPerformanceCounter() - started_) /
                    static_cast<double>(frequency_) >=
                duration_seconds_) {
            return true;
        }
        return std::nullopt;
    }

private:
    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    std::uint32_t movie_frame_;
    double duration_seconds_;
    MovieAudioPlayer& audio_;
    const MovieOverlayCallbacks& overlay_;
    std::uint64_t frequency_{};
    std::uint64_t started_{};
    bool uploaded_{};
};


// Drains trailing movie audio one hold at a time, without blocking.
//
// Each poll presents at most one frame; the exit condition is evaluated only at
// hold boundaries.
class AudioDrain {
public:
    AudioDrain(const media::MovieVideoFrame& frame,
               MovieVideoTexture& video_texture,
               MovieAudioPlayer& audio,
               bool allow_skip,
               bool fade_gameplay_movie,
               std::uint8_t initial_fade_intensity = 0U)
        : frame_(frame), video_texture_(video_texture), audio_(audio),
          allow_skip_(allow_skip), fade_gameplay_movie_(fade_gameplay_movie),
          initial_fade_intensity_(initial_fade_intensity) {}

    // Returns true when the drain completes, false on a skip edge, or
    // std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!hold_) {
            if (index_ >= maximum_drain_frames ||
                (audio_.empty() &&
                 !(fade_gameplay_movie_ && index_ < fade_frames))) {
                return true;
            }
            const auto fade_delta =
                255 - static_cast<int>(initial_fade_intensity_);
            const auto fade_step = std::min(
                fade_delta,
                ((index_ + 1) * fade_delta + fade_frames - 1) / fade_frames);
            const auto fade_intensity = static_cast<std::uint8_t>(
                fade_gameplay_movie_
                    ? static_cast<int>(initial_fade_intensity_) + fade_step
                    : 0);
            hold_.emplace(frame_, video_texture_, drain_frame_seconds, audio_,
                          allow_skip_, fade_intensity);
        }
        if (const auto done = hold_->poll(pad, previous_buttons)) {
            hold_.reset();
            if (!*done) {
                return false;
            }
            ++index_;
        }
        return std::nullopt;
    }

private:
    static constexpr int maximum_drain_frames = 120;
    static constexpr double drain_frame_seconds = 1.0 / 60.0;
    static constexpr int fade_frames = static_cast<int>(
        gameplay_movie_fade_seconds / drain_frame_seconds + 0.5);

    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    MovieAudioPlayer& audio_;
    bool allow_skip_;
    bool fade_gameplay_movie_;
    std::uint8_t initial_fade_intensity_;
    int index_{};
    std::optional<VideoFrameHold> hold_;
};


// Plays one STR's frames per poll without blocking.
class MovieDataPlayback {
public:
    MovieDataPlayback(std::string_view path,
                      std::span<const std::byte> sectors,
                      bool allow_skip,
                      bool fade_gameplay_movie)
        : stream_(sectors), timing_(stream_.framesPerSecond()),
          decoded_ahead_video_frames_(fade_gameplay_movie ? 1U : 5U),
          gameplay_fade_intensity_(fade_gameplay_movie ? 255U : 0U),
          allow_skip_(allow_skip), fade_gameplay_movie_(fade_gameplay_movie) {
        std::cout << "Playing " << path << '\n';
        if (!timing_.valid()) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "STR stream has an invalid video frame rate"};
        }
        // Scripted STR data is already resident in MissionPackage. Present its
        // first decoded frame immediately; title playback retains the wider
        // safety queue used by the looping frontend stream.
        stream_.fill(decoded_ahead_video_frames_, audio_);
    }

    // Returns true when playback finishes, false on a skip edge, or
    // std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (phase_ == Phase::frames) {
            if (!wait_) {
                if (!stream_.hasVideoFrame()) {
                    if (!has_frame_) {
                        audio_.reset();
                        return true;
                    }
                    drain_.emplace(last_frame_, video_texture_, audio_,
                                   allow_skip_, fade_gameplay_movie_,
                                   gameplay_fade_intensity_);
                    phase_ = Phase::draining;
                } else {
                    last_frame_ = stream_.takeVideoFrame();
                    has_frame_ = true;
                    wait_.emplace(
                        last_frame_, video_texture_,
                        timing_.frameEndSeconds(
                            last_frame_.timestamp_seconds,
                            stream_.nextVideoTimestamp()),
                        clock_, audio_, allow_skip_, gameplay_fade_intensity_,
                        fade_gameplay_movie_);
                }
            }
            if (phase_ == Phase::frames && wait_) {
                if (const auto done = wait_->poll(pad, previous_buttons)) {
                    wait_.reset();
                    if (!*done) {
                        audio_.reset();
                        // A skipped gameplay STR still closes through the same
                        // brief authored blackout instead of exposing the
                        // resumed world on the input edge.
                        if (fade_gameplay_movie_) {
                            drain_.emplace(last_frame_, video_texture_, audio_,
                                           false, true,
                                           gameplay_fade_intensity_);
                            phase_ = Phase::skip_draining;
                        } else {
                            return false;
                        }
                    } else {
                        stream_.fill(decoded_ahead_video_frames_, audio_);
                        return std::nullopt;
                    }
                } else {
                    return std::nullopt;
                }
            }
        }
        if (const auto done = drain_->poll(pad, previous_buttons)) {
            drain_.reset();
            if (phase_ == Phase::draining) {
                audio_.reset();
                return *done;
            }
            return false;
        }
        return std::nullopt;
    }

private:
    enum class Phase {
        frames,
        draining,
        skip_draining,
    };

    // OpenAL source state is stream-local.  Reusing a stopped source across
    // consecutive STR files can retain an implementation-defined queue/
    // cursor state and made every title movie after 989LOGO silent.  Keep the
    // shared device/context alive, but give every STR its own source and
    // buffer pool just like a fresh retail CD stream.
    MovieAudioPlayer audio_;
    BufferedMovieStream stream_;
    platform::MovieFrameTimingPolicy timing_;
    MovieVideoTexture video_texture_;
    MoviePlaybackClock clock_{};
    media::MovieVideoFrame last_frame_;
    std::size_t decoded_ahead_video_frames_;
    std::uint8_t gameplay_fade_intensity_;
    bool allow_skip_;
    bool fade_gameplay_movie_;
    bool has_frame_{};
    Phase phase_{Phase::frames};
    std::optional<VideoFrameWait> wait_;
    std::optional<AudioDrain> drain_;
};

bool playMovieData(
    std::string_view path,
    std::span<const std::byte> sectors,
    PADRAW& pad,
    std::uint16_t& previous_buttons,
    bool allow_skip = true,
    bool fade_gameplay_movie = false) {
    MovieDataPlayback playback{path, sectors, allow_skip, fade_gameplay_movie};
    for (;;) {
        if (const auto done = playback.poll(pad, previous_buttons)) {
            return *done;
        }
    }
}


// Drains trailing overlay audio one hold at a time, without blocking.
//
// Each poll presents at most one frame; the exit condition is evaluated only at
// hold boundaries.
class OverlayAudioDrain {
public:
    OverlayAudioDrain(const media::MovieVideoFrame& frame,
                      MovieVideoTexture& video_texture,
                      std::uint32_t movie_frame,
                      MovieAudioPlayer& audio,
                      const MovieOverlayCallbacks& overlay)
        : frame_(frame), video_texture_(video_texture),
          movie_frame_(movie_frame), audio_(audio), overlay_(overlay) {}

    // Returns true when the drain completes, false when the overlay dismisses
    // playback, or std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (!hold_) {
            if (index_ >= maximum_drain_frames || audio_.empty()) {
                return true;
            }
            hold_.emplace(frame_, video_texture_, movie_frame_,
                          drain_frame_seconds, audio_, overlay_);
        }
        if (const auto done = hold_->poll(pad, previous_buttons)) {
            hold_.reset();
            if (!*done) {
                return false;
            }
            ++index_;
        }
        return std::nullopt;
    }

private:
    static constexpr int maximum_drain_frames = 120;
    static constexpr double drain_frame_seconds = 1.0 / 60.0;

    const media::MovieVideoFrame& frame_;
    MovieVideoTexture& video_texture_;
    std::uint32_t movie_frame_;
    MovieAudioPlayer& audio_;
    const MovieOverlayCallbacks& overlay_;
    int index_{};
    std::optional<OverlayVideoFrameHold> hold_;
};

// Plays one looping background pass per poll without blocking.
class BackgroundPassPlayback {
public:
    BackgroundPassPlayback(const game::TitleMovie& movie,
                           const MovieOverlayCallbacks& overlay)
        : stream_(movie.sectors.bytes), timing_(stream_.framesPerSecond()),
          overlay_(overlay) {
        if (!timing_.valid()) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "STR stream has an invalid video frame rate"};
        }
        stream_.fill(decoded_ahead_video_frames_, audio_);
    }

    // Returns true when the pass finishes, false when the overlay dismisses
    // playback, or std::nullopt to present another frame.
    std::optional<bool> poll(PADRAW& pad, std::uint16_t& previous_buttons) {
        if (phase_ == Phase::frames) {
            if (!wait_) {
                if (!stream_.hasVideoFrame()) {
                    if (!has_frame_) {
                        audio_.reset();
                        return true;
                    }
                    drain_.emplace(last_frame_, video_texture_,
                                   last_movie_frame_, audio_, overlay_);
                    phase_ = Phase::draining;
                } else {
                    last_frame_ = stream_.takeVideoFrame();
                    stream_.fill(decoded_ahead_video_frames_, audio_);
                    has_frame_ = true;
                    wait_.emplace(
                        last_frame_, video_texture_, movie_frame_,
                        timing_.frameEndSeconds(
                            last_frame_.timestamp_seconds,
                            stream_.nextVideoTimestamp()),
                        clock_, audio_, overlay_);
                }
            }
            if (phase_ == Phase::frames && wait_) {
                if (const auto done = wait_->poll(pad, previous_buttons)) {
                    wait_.reset();
                    if (!*done) {
                        audio_.reset();
                        return false;
                    }
                    last_movie_frame_ = movie_frame_;
                    ++movie_frame_;
                    return std::nullopt;
                }
                return std::nullopt;
            }
        }
        if (const auto done = drain_->poll(pad, previous_buttons)) {
            drain_.reset();
            audio_.reset();
            return *done;
        }
        return std::nullopt;
    }

private:
    enum class Phase {
        frames,
        draining,
    };

    static constexpr std::size_t decoded_ahead_video_frames_ = 5U;

    // TITLE.STR is reopened on every loop, so its audio transport must be
    // reopened too.  This also guarantees sample zero is the loop boundary.
    MovieAudioPlayer audio_;
    BufferedMovieStream stream_;
    platform::MovieFrameTimingPolicy timing_;
    MovieVideoTexture video_texture_;
    MoviePlaybackClock clock_{};
    media::MovieVideoFrame last_frame_;
    std::uint32_t movie_frame_{};
    std::uint32_t last_movie_frame_{};
    const MovieOverlayCallbacks& overlay_;
    bool has_frame_{};
    Phase phase_{Phase::frames};
    std::optional<OverlayVideoFrameWait> wait_;
    std::optional<OverlayAudioDrain> drain_;
};

bool playBackgroundPass(
    const game::TitleMovie& movie,
    PADRAW& pad,
    std::uint16_t& previous_buttons,
    const MovieOverlayCallbacks& overlay) {
    BackgroundPassPlayback playback{movie, overlay};
    for (;;) {
        if (const auto done = playback.poll(pad, previous_buttons)) {
            return *done;
        }
    }
}

} // namespace

struct StandaloneMovieSession::Impl {
    Impl(const game::DiscMovie& movie, std::uint16_t previous_buttons,
         StandaloneMovieSkipPolicy skip_policy)
        : movie_(movie), previous_buttons_(previous_buttons),
          video_mode_(movie_video_mode, gameplay_video_mode),
          playback_(movie.path, movie.sectors.bytes,
                    skip_policy == StandaloneMovieSkipPolicy::allow, true) {
        PsyX_Log_Info("Standalone movie entered: %s\n", movie.path.c_str());
    }

    std::optional<std::uint16_t> poll(PADRAW& pad) {
        if (const auto done = playback_.poll(pad, previous_buttons_)) {
            PsyX_Log_Info(
                "Standalone movie %s: %s\n",
                *done ? "finished" : "skipped",
                movie_.path.c_str());
            return previous_buttons_;
        }
        return std::nullopt;
    }

    const game::DiscMovie& movie_;
    std::uint16_t previous_buttons_;
    ScopedPsyCrossVideoMode video_mode_;
    PsyCrossAudioContext audio_session_;
    MovieDataPlayback playback_;
};

StandaloneMovieSession::StandaloneMovieSession(
    const game::DiscMovie& movie,
    std::uint16_t previous_buttons,
    StandaloneMovieSkipPolicy skip_policy)
    : impl_(std::make_unique<Impl>(movie, previous_buttons, skip_policy)) {}

StandaloneMovieSession::~StandaloneMovieSession() = default;

StandaloneMovieSession::StandaloneMovieSession(StandaloneMovieSession&&) noexcept =
    default;

StandaloneMovieSession& StandaloneMovieSession::operator=(
    StandaloneMovieSession&&) noexcept = default;

std::optional<std::uint16_t> StandaloneMovieSession::poll(PADRAW& pad) {
    return impl_->poll(pad);
}

std::uint16_t PsyCrossMoviePlayer::playStandalone(
    const game::DiscMovie& movie,
    PADRAW& pad,
    std::uint16_t previous_buttons,
    StandaloneMovieSkipPolicy skip_policy) {
    StandaloneMovieSession session{movie, previous_buttons, skip_policy};
    for (;;) {
        if (const auto buttons = session.poll(pad)) {
            return *buttons;
        }
    }
}

struct TitleMovieSession::Impl {
    Impl(game::TitleMovies& movies, std::uint16_t previous_buttons,
         const MovieOverlayCallbacks& overlay, bool play_startup_movies)
        : movies_(movies), previous_buttons_(previous_buttons),
          overlay_(overlay), startup_movies_(movies.startupMovies()),
          video_mode_(movie_video_mode, gameplay_video_mode) {
        if (!overlay.update || !overlay.draw) {
            throw core::Error{
                core::ErrorCode::invalid_argument,
                "Title overlay callbacks are missing"};
        }
        audio_session_.emplace();
        if (!play_startup_movies || startup_movies_.empty()) {
            enterBackground();
        }
    }

    std::optional<std::uint16_t> poll(PADRAW& pad) {
        for (;;) {
            if (phase_ == Phase::startup) {
                if (!startup_) {
                    if (startup_index_ >= startup_movies_.size()) {
                        enterBackground();
                        continue;
                    }
                    const auto& movie = startup_movies_[startup_index_];
                    startup_.emplace(movie.path, movie.sectors.bytes, true,
                                     false);
                }
                if (const auto done = startup_->poll(pad, previous_buttons_)) {
                    if (!*done) {
                        std::cout << "Skipped "
                                  << startup_movies_[startup_index_].path
                                  << '\n';
                    }
                    startup_.reset();
                    ++startup_index_;
                    return std::nullopt;
                }
                return std::nullopt;
            }
            if (phase_ == Phase::background) {
                if (const auto done = background_->poll(pad, previous_buttons_)) {
                    background_.reset();
                    if (!*done) {
                        phase_ = Phase::transition;
                        return std::nullopt;
                    }
                    background_.emplace(movies_.backgroundMovie(), overlay_);
                    return std::nullopt;
                }
                return std::nullopt;
            }
            if (!transition_) {
                if (overlay_.transition_movie) {
                    if (auto* transition = overlay_.transition_movie();
                        transition != nullptr) {
                        transition_.emplace(transition->path,
                                            transition->sectors.bytes, true,
                                            false);
                    } else {
                        return previous_buttons_;
                    }
                } else {
                    return previous_buttons_;
                }
            }
            if (const auto done = transition_->poll(pad, previous_buttons_)) {
                return previous_buttons_;
            }
            return std::nullopt;
        }
    }

    void enterBackground() {
        std::cout << "Playing looping menu background "
                  << movies_.backgroundMovie().path << '\n';
        background_.emplace(movies_.backgroundMovie(), overlay_);
        phase_ = Phase::background;
    }

    enum class Phase {
        startup,
        background,
        transition,
    };

    game::TitleMovies& movies_;
    std::uint16_t previous_buttons_;
    const MovieOverlayCallbacks& overlay_;
    std::span<game::TitleMovie> startup_movies_;
    ScopedPsyCrossVideoMode video_mode_;
    std::optional<PsyCrossAudioContext> audio_session_;
    std::size_t startup_index_{};
    Phase phase_{Phase::startup};
    std::optional<MovieDataPlayback> startup_;
    std::optional<BackgroundPassPlayback> background_;
    std::optional<MovieDataPlayback> transition_;
};

TitleMovieSession::TitleMovieSession(game::TitleMovies& movies,
                                     std::uint16_t previous_buttons,
                                     const MovieOverlayCallbacks& overlay,
                                     bool play_startup_movies)
    : impl_(std::make_unique<Impl>(movies, previous_buttons, overlay,
                                   play_startup_movies)) {}

TitleMovieSession::~TitleMovieSession() = default;

TitleMovieSession::TitleMovieSession(TitleMovieSession&&) noexcept = default;

TitleMovieSession& TitleMovieSession::operator=(TitleMovieSession&&) noexcept =
    default;

std::optional<std::uint16_t> TitleMovieSession::poll(PADRAW& pad) {
    return impl_->poll(pad);
}

std::uint16_t PsyCrossMoviePlayer::play(
    game::TitleMovies& movies,
    PADRAW& pad,
    std::uint16_t previous_buttons,
    const MovieOverlayCallbacks& overlay,
    bool play_startup_movies) {
    TitleMovieSession session{movies, previous_buttons, overlay,
                              play_startup_movies};
    for (;;) {
        if (const auto buttons = session.poll(pad)) {
            return *buttons;
        }
    }
}

} // namespace sf::platform::detail
