#ifndef IDF_HISTORY_HPP
#define IDF_HISTORY_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>
#include <deque>
#include <filesystem>

#include "src/config.hpp"

namespace idf::history {

struct history_entry {
    std::string query;
    std::vector<std::string> top_paths;
};

inline int levenshtein(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.size(), len2 = s2.size();
    std::vector<std::vector<int>> d(len1 + 1, std::vector<int>(len2 + 1));
    d[0][0] = 0;
    for(size_t i = 1; i <= len1; ++i) d[i][0] = i;
    for(size_t i = 1; i <= len2; ++i) d[0][i] = i;
    for(size_t i = 1; i <= len1; ++i) {
        for(size_t j = 1; j <= len2; ++j) {
            d[i][j] = std::min({ d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (s1[i - 1] == s2[j - 1] ? 0 : 1) });
        }
    }
    return d[len1][len2];
}

class history_store {
    std::string file_path;
    std::deque<history_entry> entries; 
    mutable std::mutex mtx;

    void load() {
        std::ifstream ifs(file_path);
        if (!ifs.is_open()) return;

        std::string line;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            history_entry entry;
            entry.query = line;
            while (std::getline(ifs, line) && !line.empty()) {
                entry.top_paths.push_back(line);
            }
            entries.push_back(std::move(entry));
            if (entries.size() > 10000) {
                entries.pop_front();
            }
        }
    }

    void save_append(const history_entry& entry) {
        std::ofstream ofs(file_path, std::ios::app);
        if (!ofs.is_open()) return;
        ofs << entry.query << "\n";
        for (const auto& p : entry.top_paths) {
            ofs << p << "\n";
        }
        ofs << "\n";
    }

public:
    void open(const std::string& path = "idf_history.txt") {
        std::lock_guard<std::mutex> lock(mtx);
        file_path = std::filesystem::absolute(path).string();
        load();
    }

    void record(const std::string& query, const std::vector<std::string>& top_paths) {
        if (query.empty()) return;
        std::lock_guard<std::mutex> lock(mtx);
        
        history_entry entry{query, top_paths};
        entries.push_back(entry);
        if (entries.size() > 10000) {
            entries.pop_front();
            std::ofstream ofs(file_path);
            for (const auto& e : entries) {
                ofs << e.query << "\n";
                for (const auto& p : e.top_paths) ofs << p << "\n";
                ofs << "\n";
            }
        } else {
            save_append(entry);
        }
    }

    float get_path_bonus(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mtx);
        float bonus = 0.0f;
        int max_check = std::min(10, static_cast<int>(entries.size()));
        int start_idx = entries.size() - 1;
        
        for (int i = 0; i < max_check; ++i) {
            const auto& entry = entries[start_idx - i];
            if (std::find(entry.top_paths.begin(), entry.top_paths.end(), path) != entry.top_paths.end()) {
                bonus += 10.0f / static_cast<float>(i + 1);
            }
        }
        return bonus;
    }

    std::vector<std::string> suggest(const std::string& query, size_t n = 10) const {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::pair<int, std::string>> scored;
        
        
        std::unordered_map<std::string, int> best_dist;
        for (const auto& e : entries) {
            if (e.query == query) continue;
            int dist = levenshtein(query, e.query);
            if (best_dist.find(e.query) == best_dist.end() || dist < best_dist[e.query]) {
                best_dist[e.query] = dist;
            }
        }
        
        for (const auto& [q, dist] : best_dist) {
            scored.push_back({dist, q});
        }
        
        std::sort(scored.begin(), scored.end());
        std::vector<std::string> out;
        for (size_t i = 0; i < std::min(n, scored.size()); ++i) {
            out.push_back(scored[i].second);
        }
        return out;
    }

    bool is_open() const { return !file_path.empty(); }
};

class search_observer {
    history_store& store;
public:
    explicit search_observer(history_store& s) : store(s) {}
    void on_query_executed(const std::string& query, const std::vector<std::string>& top_paths) {
        store.record(query, top_paths);
    }
};

}

#endif
