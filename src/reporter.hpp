#ifndef IDF_REPORTER_HPP
#define IDF_REPORTER_HPP

#include <atomic>
#include <chrono>

namespace fp {
    inline std::atomic<size_t> files_completed{0};
    inline std::atomic<size_t> bytes_completed{0};
    inline std::atomic<bool> done{false};

    inline void reset_stats() {
        files_completed = 0;
        bytes_completed = 0;
        done = false;
    }
}

#endif
