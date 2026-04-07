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
        std::cout << "Type \\q to exit.\n";
        while (true) {
            std::string query;
            std::cout << "Q> ";
            std::getline(std::cin, query);

            if (query == "\\q") return;
            duckdb::Connection con(db);

            idf::search::Query q(std::move(con), query);
            q.execute();

            for (auto & res : q.path_result) {
                std::cout << res << "\n";
            }

        }

    }
}
#endif //IDF_CLI_H
