



#ifndef IDF_REPORTER_HPP
#define IDF_REPORTER_HPP

#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>

namespace fp {
    inline std::atomic<size_t> files_completed{0};
    inline std::atomic<size_t> bytes_completed{0};
    inline std::atomic<size_t> active_chunks{0};
    inline std::atomic<bool> done{false};

    inline void start_reporter(std::chrono::high_resolution_clock::time_point start_time, size_t chunk_size) {
        done = false;
        std::thread([start_time]() {
            size_t last_bytes = 0;
            auto last_time = start_time;
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto now = std::chrono::high_resolution_clock::now();
                size_t current_bytes = bytes_completed.load();
                size_t current_files = files_completed.load();

                std::chrono::duration<double> delta_elapsed = now - last_time;

                double mib_total = current_bytes / (1024.0 * 1024.0);
                double mib_s = delta_elapsed.count() > 0 ? ((current_bytes - last_bytes) / (1024.0 * 1024.0)) / delta_elapsed.count() : 0;

                std::cout << "\rFiles: " << current_files
                        << " | In-Flight: " << active_chunks.load()
                        << " | " << std::fixed << std::setprecision(1) << mib_s << " MiB/s"
                        << " | Total: " << std::fixed << std::setprecision(1) << mib_total << " MiB          " << std::flush;

                last_bytes = current_bytes;
                last_time = now;
            }
            std::cout << std::endl;
        }).detach();
    }
}

#endif
