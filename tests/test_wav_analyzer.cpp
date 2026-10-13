#include "taskforge/wav_analyzer.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

void add_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
}

void add_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<unsigned char>((value >> (index * 8)) & 0xffU));
    }
}

void add_id(Bytes& bytes, const char* id) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<unsigned char>(id[index]));
    }
}

void add_chunk(Bytes& payload, const char* id, const Bytes& data) {
    add_id(payload, id);
    add_u32(payload, static_cast<std::uint32_t>(data.size()));
    payload.insert(payload.end(), data.begin(), data.end());
    if ((data.size() & 1U) != 0U) {
        payload.push_back(0);
    }
}

Bytes make_wave(std::uint16_t channels, std::uint16_t bits,
                const std::vector<std::int16_t>& samples,
                bool add_odd_metadata = false) {
    Bytes payload;
    add_id(payload, "WAVE");
    Bytes fmt;
    add_u16(fmt, 1);
    add_u16(fmt, channels);
    add_u32(fmt, 8000);
    add_u32(fmt, 8000U * channels * (bits / 8));
    add_u16(fmt, static_cast<std::uint16_t>(channels * (bits / 8)));
    add_u16(fmt, bits);
    add_chunk(payload, "fmt ", fmt);
    if (add_odd_metadata) {
        add_chunk(payload, "JUNK", Bytes{42});
    }
    Bytes pcm;
    for (const auto sample : samples) {
        add_u16(pcm, static_cast<std::uint16_t>(sample));
    }
    add_chunk(payload, "data", pcm);
    Bytes file;
    add_id(file, "RIFF");
    add_u32(file, static_cast<std::uint32_t>(payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

struct TempWave {
    explicit TempWave(const Bytes& bytes) {
        static unsigned next = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("taskforge-v010-" + std::to_string(stamp) + "-" +
                std::to_string(next++) + ".wav");
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("Cannot write test fixture");
        }
    }
    ~TempWave() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    TempWave(const TempWave&) = delete;
    TempWave& operator=(const TempWave&) = delete;
    std::filesystem::path path;
};

void require(bool condition, const std::string& why) {
    if (!condition) {
        throw std::runtime_error(why);
    }
}

void near(double actual, double expected, const std::string& why) {
    require(std::abs(actual - expected) < 1e-9, why);
}

void rejects(const Bytes& bytes, const std::string& why) {
    TempWave file(bytes);
    bool rejected = false;
    try {
        static_cast<void>(taskforge::analyze_wav(file.path));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, why);
}

void test_mono_and_odd_chunk() {
    TempWave file(make_wave(1, 16, {-32768, 0, 32767}, true));
    const auto stats = taskforge::analyze_wav(file.path);
    const double positive = 32767.0 / 32768.0;
    const double energy = 1.0 + positive * positive;
    require(stats.channels == 1 && stats.sample_rate_hz == 8000 && stats.frames == 3,
            "mono metadata");
    near(stats.duration_seconds, 3.0 / 8000.0, "mono duration");
    near(stats.peak, 1.0, "mono peak");
    near(stats.energy, energy, "mono energy");
    near(stats.rms, std::sqrt(energy / 3.0), "mono rms");
}

void test_stereo() {
    TempWave file(make_wave(2, 16, {0, 16384, 0, -16384}));
    const auto stats = taskforge::analyze_wav(file.path);
    require(stats.channels == 2 && stats.frames == 2, "stereo frames");
    near(stats.duration_seconds, 2.0 / 8000.0, "stereo duration");
    near(stats.peak, 0.5, "stereo peak");
    near(stats.energy, 0.5, "stereo energy");
    near(stats.rms, std::sqrt(0.5 / 4.0), "stereo rms");
}

void test_invalid_inputs() {
    rejects(Bytes{'n', 'o', 't', ' ', 'w', 'a', 'v', 'e'}, "bad signature");
    auto truncated = make_wave(1, 16, {1, 2});
    truncated.pop_back();
    rejects(truncated, "truncated RIFF");
    rejects(make_wave(1, 8, {1}), "unsupported bit depth");
    rejects(make_wave(1, 16, {}), "empty audio");
    bool missing_rejected = false;
    try {
        static_cast<void>(taskforge::analyze_wav(
            std::filesystem::temp_directory_path() /
            "taskforge-v010-does-not-exist.wav"));
    } catch (const std::runtime_error&) {
        missing_rejected = true;
    }
    require(missing_rejected, "missing file");
}

} // namespace

int main() {
    try {
        test_mono_and_odd_chunk();
        test_stereo();
        test_invalid_inputs();
        std::cout << "WAV parsing, PCM statistics and invalid input: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << "\n";
        return 1;
    }
}
