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

struct SearchResult {
    std::string path;
    std::string snippet;
};

class Query {
public:
    duckdb::Connection con;
    std::string query; 
    bool is_phrase_mode; 

    Query(duckdb::Connection&& conn, const std::string& query, bool is_phrase = true)
        : con(std::move(conn)), query(query), is_phrase_mode(is_phrase) {}


    template <typename Callback>
    bool execute(Callback callback) {

        std::stringstream ss(query);
        std::string word;
        std::vector<std::string> words;
        while (ss >> word) {
            words.push_back(word);
        }

        if (words.empty()) return false;

        std::string sql = "SELECT f.path, o0.position, o" + std::to_string(words.size() - 1) + ".position FROM main.files f ";
        std::string joins = "";
        std::string filters = " WHERE ";

        for (size_t i = 0; i < words.size(); ++i) {
            std::string idx = std::to_string(i);

            joins += " JOIN main.token_occurrences o" + idx + " ON f.hash = o" + idx + ".file_hash ";
            joins += " JOIN main.tokens t" + idx + " ON o" + idx + ".token_id = t" + idx + ".id ";

            if (i > 0) filters += " AND ";
            filters += "t" + idx + ".token = '" + words[i] + "'";

            if (i > 0) {
                if (is_phrase_mode) {
                    filters += " AND o" + idx + ".\"position\" > o" + std::to_string(i - 1) + ".\"position\"";
                    filters += " AND o" + idx + ".\"position\" - o" +  std::to_string(i - 1) + ".\"position\" < 128";
                } else {
                    filters += " AND o" + idx + ".\"position\" > o" + std::to_string(i - 1) + ".\"position\"";
                    filters += " AND o" + idx + ".\"position\" - o" +  std::to_string(i - 1) + ".\"position\" <= " + std::to_string(words[i-1].length() + 8);
                }
            }
        }

        std::string final_query = sql + joins + filters + ";";

        auto result = con.SendQuery(final_query);
        if (result->HasError()) {
            std::cerr << "Search Error: " << result->GetError() << std::endl;
            return false;
        }

        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;
            for (duckdb::idx_t i = 0; i < chunk->size(); i++) {
                std::string res_path = chunk->GetValue(0, i).ToString();
                uint64_t res_pos_start = chunk->GetValue(1, i).GetValue<uint64_t>();
                uint64_t res_pos_end = chunk->GetValue(2, i).GetValue<uint64_t>();


                std::ifstream ifs(res_path, std::ios::binary);
                std::string snippet;
                if (ifs.is_open()) {
                    long start_pos = std::max(0L, static_cast<long>(res_pos_start) - 32L);
                    ifs.seekg(start_pos);
                    
                    long phrase_span = res_pos_end - res_pos_start + words.back().length();
                    long fetch_len = std::min(2048L, 64L + phrase_span);
                    
                    std::vector<char> buf(fetch_len + 1, 0);
                    ifs.read(buf.data(), fetch_len);
                    std::string raw(buf.data(), ifs.gcount());
                    

                    for(auto& c : raw) if(c == '\n' || c == '\r') c = ' ';
                    snippet = "... " + raw + " ...";
                    ifs.close();
                }

                bool proceed = true;
                if constexpr (std::is_invocable_v<Callback, const SearchResult&>) {
                    using Ret = std::invoke_result_t<Callback, const SearchResult&>;
                    if constexpr (std::is_same_v<Ret, bool>) {
                        proceed = callback(SearchResult{res_path, snippet});
                    } else {
                        callback(SearchResult{res_path, snippet});
                    }
                }
                
                if (!proceed) return true;
            }
        }

        return true;
    }
};
}

#endif
