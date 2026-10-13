#include "taskforge/wav_analyzer.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

int run(const std::vector<std::filesystem::path>& files) {
    if (files.empty()) {
        std::cerr << "Usage: taskforge <file.wav> [more.wav ...]\n";
        return 2;
    }
    bool any_failure = false;
    std::cout << std::fixed << std::setprecision(6);
    for (const auto& file : files) {
        try {
            const auto stats = taskforge::analyze_wav(file);
            std::cout << file.u8string() << "\n"
                      << "  sample_rate_hz=" << stats.sample_rate_hz
                      << " channels=" << stats.channels
                      << " frames=" << stats.frames << "\n"
                      << "  duration_s=" << stats.duration_seconds
                      << " peak=" << stats.peak
                      << " rms=" << stats.rms
                      << " energy=" << stats.energy << "\n";
        } catch (const std::exception& error) {
            any_failure = true;
            std::cerr << file.u8string() << ": " << error.what() << "\n";
        }
    }
    return any_failure ? 1 : 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    std::vector<std::filesystem::path> files;
    for (int index = 1; index < argc; ++index) {
        files.emplace_back(argv[index]);
    }
    return run(files);
}
