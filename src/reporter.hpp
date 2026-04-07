



#ifndef IDF_REPORTER_HPP
#define IDF_REPORTER_HPP

#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>

namespace fp {
    inline std::atomic<size_t> files_completed{0};
    inline std::atomic<size_t> chunks_completed{0};
    inline std::atomic<size_t> active_chunks{0};
    inline std::atomic<bool> done{false};

    inline void start_reporter(std::chrono::high_resolution_clock::time_point start_time, size_t chunk_size) {
        std::thread([start_time, chunk_size]() {
            size_t last_chunks = 0;
            auto last_time = start_time;
            while (!done) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto now = std::chrono::high_resolution_clock::now();
                size_t current_chunks = chunks_completed.load();
                size_t current_files = files_completed.load();

                std::chrono::duration<double> delta_elapsed = now - last_time;

                double mib_total = (current_chunks * chunk_size) / (1024.0 * 1024.0);
                double mib_s = delta_elapsed.count() > 0 ? ((current_chunks - last_chunks) * chunk_size) / (1024.0 * 1024.0 * delta_elapsed.count()) : 0;

                std::cout << "\rFiles: " << current_files
                        << " | Chunks: " << current_chunks
                        << " | In-Flight: " << active_chunks.load()
                        << " | " << std::fixed << std::setprecision(1) << mib_s << " MiB/s"
                        << " | Total: " << std::fixed << std::setprecision(1) << mib_total << " MiB" << std::flush;

                last_chunks = current_chunks;
                last_time = now;
            }
            std::cout << std::endl;
        }).detach();
    }
}

#endif
