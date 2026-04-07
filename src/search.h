#ifndef IDF_SEARCH_H
#define IDF_SEARCH_H

#include <duckdb.hpp>
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <map>

namespace idf::search {

class Query {
public:
    duckdb::Connection con;
    std::string query; 
    std::vector<std::string> path_result;

    Query(duckdb::Connection&& conn, const std::string& query)
        : con(std::move(conn)), query(query) {};

    Query& execute() {
        path_result.clear();

        
        std::stringstream ss(query);
        std::string word;
        std::vector<std::string> words;
        while (ss >> word) {
            words.push_back(word);
        }

        if (words.empty()) return *this;

        
        
        std::string sql = "SELECT DISTINCT f.path FROM idf.main.files f ";
        std::string joins = "";
        std::string filters = " WHERE ";

        for (size_t i = 0; i < words.size(); ++i) {
            std::string idx = std::to_string(i);

            
            joins += " JOIN idf.main.token_occurrences o" + idx + " ON f.hash = o" + idx + ".file_hash ";
            joins += " JOIN idf.main.tokens t" + idx + " ON o" + idx + ".token_id = t" + idx + ".id ";

            
            if (i > 0) filters += " AND ";
            filters += "t" + idx + ".token = '" + words[i] + "'";

            
            if (i > 0) {
                filters += " AND o" + idx + ".\"position\" = o" + std::to_string(i - 1) + ".\"position\" + 1";
            }
        }

        std::string final_query = sql + joins + filters + " LIMIT 100;";

        
        auto result = con.Query(final_query);
        if (result->HasError()) {
            std::cerr << "Search Error: " << result->GetError() << std::endl;
            return *this;
        }

        
        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;
            for (idx_t i = 0; i < chunk->size(); i++) {
                path_result.push_back(chunk->GetValue(0, i).ToString());
            }
        }

        return *this;
    }

    std::vector<std::string> path_resutls() {
        return this->path_result;
    }
};
}

#endif 
