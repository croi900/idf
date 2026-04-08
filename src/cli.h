//
// Created by croi on 08.04.2026.
//

#ifndef IDF_CLI_H
#define IDF_CLI_H
#include <iostream>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>

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

            idf::search::Query q(std::move(con), query, is_phrase);
            q.execute([&](const idf::search::SearchResult& res) {
                std::string snip = res.snippet;
                for (const auto& wd : words) {
                    size_t pos = 0;
                    while ((pos = snip.find(wd, pos)) != std::string::npos) {
                        snip.replace(pos, wd.length(), "\x1b[32m" + wd + "\x1b[0m");
                        pos += 9 + wd.length();
                    }
                }
                std::cout << "\x1b[36m" << res.path << "\x1b[0m\n" << snip << "\n\n";
            });
        }
    }
}
#endif //IDF_CLI_H
