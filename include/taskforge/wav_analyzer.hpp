#pragma once

#include "taskforge/task.hpp"

#include <cstdint>
#include <filesystem>

namespace taskforge {

// v0.1.0: statistics combine all channels and normalize signed PCM16 by 32768.
struct AudioStats {
    std::uint32_t sample_rate_hz = 0;
    std::uint16_t channels = 0;
    std::uint64_t frames = 0;
    double duration_seconds = 0.0;
    double peak = 0.0;
    double rms = 0.0;
    double energy = 0.0; // Sum of squares of normalized samples.
};

// Accepts RIFF/WAVE with one PCM16 fmt chunk and one data chunk, mono or stereo.
// Throws std::runtime_error for missing, malformed, oversized or unsupported files.
AudioStats analyze_wav(const std::filesystem::path& path);

// Checks cancellation before I/O, between 64 KiB reads, during chunk parsing,
// and every 4096 samples. A single blocking OS I/O call is not interruptible.
AudioStats analyze_wav_cancellable(CancellationToken token,
                                   const std::filesystem::path& path);

} // namespace taskforge
