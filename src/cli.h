//
// Created by croi on 08.04.2026.
//

#ifndef IDF_CLI_H
#define IDF_CLI_H
#include <iostream>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>

#include <algorithm>
#include "search.h"

namespace idf {
    inline void cli() {
        duckdb::DuckDB db("idf.duckdb");
        std::cout << "Type \\q to exit, \\p for phrase search, \\c for consecutive search.\n";
        bool is_phrase = true;
        
        while (true) {
            std::string query;
            std::cout << (is_phrase ? "[Phrase]" : "[Consecutive]") << " Q> ";
            std::getline(std::cin, query);

            if (query == "\\q") return;
            if (query == "\\p") { is_phrase = true; continue; }
            if (query == "\\c") { is_phrase = false; continue; }
            if (query.empty()) continue;

            std::vector<std::string> words;
            std::stringstream ss(query);
            std::string w;
            while (ss >> w) words.push_back(w);

            duckdb::Connection con(db);

            try {
                idf::search::Query q(std::move(con), query, is_phrase);
                size_t count = 0;
                q.execute([&](const idf::search::SearchResult& res) {
                    if (++count > 100) return false;
                    std::string snip = res.snippet;
                    std::vector<std::pair<size_t, size_t>> intervals;
                    for (const auto& wd : words) {
                        size_t pos = 0;
                        while ((pos = snip.find(wd, pos)) != std::string::npos) {
                            intervals.push_back({pos, pos + wd.length()});
                            pos += wd.length();
                        }
                    }
                    if (!intervals.empty()) {
                        std::sort(intervals.begin(), intervals.end());
                        std::vector<std::pair<size_t, size_t>> merged;
                        for (auto& iv : intervals) {
                            if (merged.empty() || merged.back().second < iv.first) {
                                merged.push_back(iv);
                            } else {
                                merged.back().second = std::max(merged.back().second, iv.second);
                            }
                        }
                        for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
                            snip.insert(it->second, "\x1b[0m");
                            snip.insert(it->first, "\x1b[32m");
                        }
                    }
                    
                    std::cout << "\x1b[36m" << res.path << "\x1b[0m\n" << snip << "\n\n";
                    return true;
                });
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
    }
}
#endif //IDF_CLI_H
