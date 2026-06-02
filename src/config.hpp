#ifndef IDF_CONFIG_HPP
#define IDF_CONFIG_HPP

#include <string>
#include <vector>
#include <sstream>
#include <hpx/hpx.hpp>
#include "src/extensions.hpp"

namespace idf {
    enum class rank_strategy { Score, Alphabetical, DateModified };
}

namespace idf::config {
    inline size_t num_workers = 0;
    inline size_t chunk_size = 32 * 1024 * 1024;
    inline size_t max_active_chunks = 1024;
    inline std::vector<std::string> allowed_extensions = [] {
        std::vector<std::string> exts(dirtree::text_extensions.begin(), dirtree::text_extensions.end());
        for (auto e : dirtree::image_extensions) exts.emplace_back(e);
        return exts;
    }();
    inline size_t num_shards = 256;
    inline std::string root_path = ".";
    inline bool enable_substring_search = false;
    inline idf::rank_strategy current_rank_strategy = idf::rank_strategy::Score;
    inline size_t io_batch_size = 1000;
    inline size_t max_ts_file_bytes = 512 * 1024;
    inline size_t max_ts_nodes_per_file = 200'000;

    inline bool is_allowed_extension(const std::string_view& ext) {
        if (allowed_extensions.empty()) return true;
        for (const auto& allowed : allowed_extensions) {
            if (ext == allowed) return true;
        }
        return false;
    }
}

#endif //IDF_CONFIG_HPP
