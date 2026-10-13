#include "taskforge/wav_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace taskforge {
namespace {

constexpr std::uint64_t kMaxFileBytes = 256ULL * 1024ULL * 1024ULL;

std::uint16_t read_u16(const std::vector<unsigned char>& bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) {
        throw std::runtime_error("Truncated WAV field");
    }
    return static_cast<std::uint16_t>(bytes[at]) |
           static_cast<std::uint16_t>(bytes[at + 1] << 8);
}

std::uint32_t read_u32(const std::vector<unsigned char>& bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) {
        throw std::runtime_error("Truncated WAV field");
    }
    return static_cast<std::uint32_t>(bytes[at]) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

bool has_id(const std::vector<unsigned char>& bytes, std::size_t at,
            const char (&id)[5]) {
    return at <= bytes.size() && bytes.size() - at >= 4 &&
           std::memcmp(bytes.data() + at, id, 4) == 0;
}

std::vector<unsigned char> load_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Cannot open input file");
    }
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("Cannot determine file length");
    }
    if (static_cast<std::uint64_t>(length) > kMaxFileBytes) {
        throw std::runtime_error("WAV exceeds the 256 MiB v0.1.0 limit");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot read input file completely");
    }
    return bytes;
}

} // namespace

AudioStats analyze_wav(const std::filesystem::path& path) {
    const auto bytes = load_file(path);
    if (bytes.size() < 12 || !has_id(bytes, 0, "RIFF") ||
        !has_id(bytes, 8, "WAVE")) {
        throw std::runtime_error("Expected a RIFF/WAVE file");
    }

    const std::uint32_t riff_size = read_u32(bytes, 4);
    if (riff_size < 4) {
        throw std::runtime_error("Invalid RIFF size");
    }
    const std::uint64_t riff_end = 8ULL + riff_size;
    if (riff_end > bytes.size()) {
        throw std::runtime_error("Truncated RIFF container");
    }

    bool found_fmt = false;
    bool found_data = false;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t block_align = 0;
    std::size_t data_at = 0;
    std::size_t data_size = 0;

    std::size_t at = 12;
    while (at < riff_end) {
        if (riff_end - at < 8) {
            throw std::runtime_error("Truncated WAV chunk header");
        }
        const std::uint32_t chunk_size = read_u32(bytes, at + 4);
        const std::size_t content_at = at + 8;
        if (chunk_size > riff_end - content_at) {
            throw std::runtime_error("Truncated WAV chunk");
        }

        if (has_id(bytes, at, "fmt ")) {
            if (found_fmt || chunk_size < 16) {
                throw std::runtime_error("Invalid or duplicate fmt chunk");
            }
            found_fmt = true;
            const std::uint16_t encoding = read_u16(bytes, content_at);
            channels = read_u16(bytes, content_at + 2);
            sample_rate = read_u32(bytes, content_at + 4);
            const std::uint32_t byte_rate = read_u32(bytes, content_at + 8);
            block_align = read_u16(bytes, content_at + 12);
            const std::uint16_t bits_per_sample = read_u16(bytes, content_at + 14);
            if (encoding != 1 || (channels != 1 && channels != 2) ||
                bits_per_sample != 16 || sample_rate == 0 ||
                block_align != channels * 2 ||
                static_cast<std::uint64_t>(byte_rate) !=
                    static_cast<std::uint64_t>(sample_rate) * block_align) {
                throw std::runtime_error(
                    "Only mono/stereo little-endian PCM16 WAV is supported");
            }
        } else if (has_id(bytes, at, "data")) {
            if (found_data) {
                throw std::runtime_error("Duplicate data chunk");
            }
            found_data = true;
            data_at = content_at;
            data_size = chunk_size;
        }

        std::size_t next = content_at + chunk_size;
        if ((chunk_size & 1U) != 0U) {
            if (next >= riff_end) {
                throw std::runtime_error("Missing WAV chunk padding byte");
            }
            ++next;
        }
        at = next;
    }
    if (!found_fmt || !found_data) {
        throw std::runtime_error("Missing fmt or data chunk");
    }
    if (data_size == 0 || data_size % block_align != 0) {
        throw std::runtime_error("Empty or incomplete PCM frame data");
    }

    AudioStats stats;
    stats.sample_rate_hz = sample_rate;
    stats.channels = channels;
    stats.frames = data_size / block_align;
    stats.duration_seconds = static_cast<double>(stats.frames) / sample_rate;
    const std::uint64_t sample_count = data_size / 2;
    for (std::size_t offset = 0; offset < data_size; offset += 2) {
        const std::uint16_t raw = read_u16(bytes, data_at + offset);
        const int signed_sample = raw >= 0x8000U
                                      ? static_cast<int>(raw) - 65536
                                      : static_cast<int>(raw);
        const double normalized = signed_sample / 32768.0;
        stats.peak = std::max(stats.peak, std::abs(normalized));
        stats.energy += normalized * normalized;
    }
    stats.rms = std::sqrt(stats.energy / static_cast<double>(sample_count));
    return stats;
}

} // namespace taskforge
