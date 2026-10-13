#include "sf/media/str_decoder.hpp"

#include "sf/core/error.hpp"

namespace sf::media {

struct StrDecoder::Impl {
    // The stub never produces frames; the type only satisfies the pimpl.
};

StrDecoder::StrDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StrDecoder::~StrDecoder() = default;

StrDecoder::StrDecoder(StrDecoder&&) noexcept = default;

StrDecoder& StrDecoder::operator=(StrDecoder&&) noexcept = default;

StrDecoder StrDecoder::open(std::vector<std::byte> /*raw_sectors*/) {
    throw core::Error{
        core::ErrorCode::unsupported,
        "Movie decoding is unavailable in this build (FFmpeg is not ported)"};
}

StrDecoder StrDecoder::open(std::span<const std::byte> /*raw_sectors*/) {
    throw core::Error{
        core::ErrorCode::unsupported,
        "Movie decoding is unavailable in this build (FFmpeg is not ported)"};
}

std::optional<MovieEvent> StrDecoder::next() {
    return std::nullopt;
}

double StrDecoder::framesPerSecond() const noexcept {
    return 0.0;
}

bool StrDecoder::hasAudio() const noexcept {
    return false;
}

} // namespace sf::media
