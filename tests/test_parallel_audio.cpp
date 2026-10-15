#include "taskforge/thread_pool.hpp"
#include "taskforge/wav_analyzer.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& why) {
    if (!condition) {
        throw std::runtime_error(why);
    }
}

void add_u16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
}

void add_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<unsigned char>((value >> (index * 8)) & 0xffU));
    }
}

void add_id(std::vector<unsigned char>& bytes, const char* id) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<unsigned char>(id[index]));
    }
}

void write_wave(const std::filesystem::path& path, std::uint16_t channels,
                const std::vector<std::int16_t>& samples) {
    std::vector<unsigned char> bytes;
    add_id(bytes, "RIFF");
    add_u32(bytes, 36U + static_cast<std::uint32_t>(samples.size() * 2));
    add_id(bytes, "WAVE");
    add_id(bytes, "fmt ");
    add_u32(bytes, 16);
    add_u16(bytes, 1);
    add_u16(bytes, channels);
    add_u32(bytes, 16000);
    add_u32(bytes, 16000U * channels * 2);
    add_u16(bytes, static_cast<std::uint16_t>(channels * 2));
    add_u16(bytes, 16);
    add_id(bytes, "data");
    add_u32(bytes, static_cast<std::uint32_t>(samples.size() * 2));
    for (const auto sample : samples) {
        add_u16(bytes, static_cast<std::uint16_t>(sample));
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "cannot write audio fixture");
}

struct TemporaryFiles {
    TemporaryFiles() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() /
                    ("taskforge-parallel-" + std::to_string(stamp));
        require(std::filesystem::create_directory(directory), "temporary directory collision");
    }
    ~TemporaryFiles() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    TemporaryFiles(const TemporaryFiles&) = delete;
    TemporaryFiles& operator=(const TemporaryFiles&) = delete;
    std::filesystem::path directory;
};

struct Outcome {
    std::optional<taskforge::AudioStats> stats;
    std::string error;
};

template <class Get>
Outcome outcome(Get&& get) {
    try {
        return {get(), {}};
    } catch (const std::exception& error) {
        return {std::nullopt, error.what()};
    }
}

bool same(const Outcome& serial, const Outcome& parallel) {
    if (serial.stats.has_value() != parallel.stats.has_value() || serial.error != parallel.error) {
        return false;
    }
    if (!serial.stats) {
        return true;
    }
    const auto& left = *serial.stats;
    const auto& right = *parallel.stats;
    return left.sample_rate_hz == right.sample_rate_hz && left.channels == right.channels &&
           left.frames == right.frames &&
           std::abs(left.duration_seconds - right.duration_seconds) < 1e-12 &&
           std::abs(left.peak - right.peak) < 1e-12 &&
           std::abs(left.rms - right.rms) < 1e-12 &&
           std::abs(left.energy - right.energy) < 1e-12;
}

void parallel_audio_matches_serial() {
    TemporaryFiles files;
    const auto mono = files.directory / "mono.wav";
    const auto stereo = files.directory / "stereo.wav";
    const auto missing = files.directory / "missing.wav";
    write_wave(mono, 1, {-32768, 0, 32767, 16384, -16384});
    write_wave(stereo, 2, {100, -100, 30000, -30000, 0, 10000});

    std::vector<std::filesystem::path> inputs;
    for (unsigned index = 0; index < 24; ++index) {
        inputs.push_back(mono);
        inputs.push_back(missing);
        inputs.push_back(stereo);
    }
    std::vector<Outcome> serial;
    for (const auto& input : inputs) {
        serial.push_back(outcome([&input] { return taskforge::analyze_wav(input); }));
    }

    for (const std::size_t workers : {std::size_t{1}, std::size_t{4}}) {
        taskforge::ThreadPool pool(workers);
        std::vector<std::future<taskforge::AudioStats>> results;
        for (const auto& input : inputs) {
            results.push_back(pool.submit(taskforge::analyze_wav, input));
        }
        std::size_t failures = 0;
        for (std::size_t index = 0; index < results.size(); ++index) {
            const auto parallel = outcome([&] { return results[index].get(); });
            require(same(serial[index], parallel), "serial/parallel result differs");
            if (!parallel.stats) {
                ++failures;
            }
        }
        require(failures == 24, "missing inputs must fail independently");
    }
}

} // namespace

int main() {
    try {
        parallel_audio_matches_serial();
        std::cout << "72-file serial/parallel comparison with independent failures: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Parallel audio test failed: " << error.what() << "\n";
        return 1;
    }
}
