#include "taskforge/thread_pool.hpp"
#include "taskforge/wav_analyzer.hpp"

#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::size_t workers = 0; // Zero selects the original serial execution path.
    bool help = false;
    bool version = false;
    std::vector<std::filesystem::path> files;
};

void usage() {
    std::cout << "TaskForge v0.2.0\n"
              << "Usage: taskforge [--serial | --threads N] [--] <file.wav> [more.wav ...]\n"
              << "  Default: serial. N must be an integer from 1 to 64.\n"
              << "  --help     Show this help\n"
              << "  --version  Show the application version\n";
}

Options parse(const std::vector<std::filesystem::path>& arguments) {
    Options options;
    bool parse_options = true;
    bool mode_selected = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string token = arguments[index].u8string();
        if (parse_options && token == "--") {
            parse_options = false;
        } else if (parse_options && token == "--help") {
            options.help = true;
        } else if (parse_options && token == "--version") {
            options.version = true;
        } else if (parse_options && token == "--serial") {
            if (mode_selected) {
                throw std::invalid_argument("Specify an execution mode only once");
            }
            mode_selected = true;
        } else if (parse_options && token == "--threads") {
            if (mode_selected || index + 1 >= arguments.size()) {
                throw std::invalid_argument("--threads requires one worker count and one mode");
            }
            mode_selected = true;
            const std::string count = arguments[++index].u8string();
            const auto parsed = std::from_chars(count.data(), count.data() + count.size(),
                                                options.workers);
            if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
                options.workers < 1 || options.workers > 64) {
                throw std::invalid_argument("Worker count must be an integer from 1 to 64");
            }
        } else if (parse_options && !token.empty() && token.front() == '-') {
            throw std::invalid_argument("Unknown option: " + token);
        } else {
            options.files.push_back(arguments[index]);
        }
    }
    if (!options.help && !options.version && options.files.empty()) {
        throw std::invalid_argument("Provide at least one WAV file");
    }
    return options;
}

void print_stats(const std::filesystem::path& path, const taskforge::AudioStats& stats) {
    std::cout << path.u8string() << "\n"
              << "  sample_rate_hz=" << stats.sample_rate_hz
              << " channels=" << stats.channels << " frames=" << stats.frames << "\n"
              << "  duration_s=" << stats.duration_seconds << " peak=" << stats.peak
              << " rms=" << stats.rms << " energy=" << stats.energy << "\n";
}

int analyze(const Options& options) {
    bool any_failure = false;
    std::cout << std::fixed << std::setprecision(6)
              << "TaskForge v0.2.0 mode=" << (options.workers == 0 ? "serial" : "parallel")
              << " workers=" << (options.workers == 0 ? 1 : options.workers)
              << " files=" << options.files.size() << "\n";

    if (options.workers == 0) {
        for (const auto& file : options.files) {
            try {
                print_stats(file, taskforge::analyze_wav(file));
            } catch (const std::exception& error) {
                any_failure = true;
                std::cerr << file.u8string() << ": " << error.what() << "\n";
            }
        }
    } else {
        taskforge::ThreadPool pool(options.workers);
        std::vector<std::future<taskforge::AudioStats>> results;
        results.reserve(options.files.size());
        for (const auto& file : options.files) {
            // submit owns a copy of file. Do not capture the loop variable by reference.
            results.push_back(pool.submit(taskforge::analyze_wav, file));
        }
        // Print only from the main thread, preserving input order.
        for (std::size_t index = 0; index < results.size(); ++index) {
            try {
                print_stats(options.files[index], results[index].get());
            } catch (const std::exception& error) {
                any_failure = true;
                std::cerr << options.files[index].u8string() << ": " << error.what() << "\n";
            }
        }
    }
    return any_failure ? 1 : 0;
}

int run(const std::vector<std::filesystem::path>& arguments) {
    Options options;
    try {
        options = parse(arguments);
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << "\n";
        usage();
        return 2;
    }
    if (options.help) {
        usage();
        return 0;
    }
    if (options.version) {
        std::cout << "TaskForge v0.2.0\n";
        return 0;
    }
    try {
        return analyze(options);
    } catch (const std::exception& error) {
        std::cerr << "Cannot execute batch: " << error.what() << "\n";
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return run(arguments);
}
