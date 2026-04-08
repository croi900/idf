//
// Created by croi on 08.04.2026.
//

#ifndef IDF_GUI_H
#define IDF_GUI_H

#include "httplib.h"
#include "json.hpp"
#include "index.h"
#include "search.h"
#include "config.hpp"
#include "reporter.hpp"
#include <thread>
#include <iostream>
#include <cstdlib>
#include <algorithm>

namespace idf {
    inline void gui() {
        std::cout << "Starting python frontend in background..." << std::endl;
        std::thread([]() {
            int ret = std::system("cd gui && uv run app.py");
            if (ret != 0) {
                std::cerr << "Frontend UI exited with code " << ret << std::endl;
            }
        }).detach();

        httplib::Server svr;

        svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
            nlohmann::json j;
            j["indexing"] = !fp::done.load();
            j["files_completed"] = fp::files_completed.load();
            j["bytes_completed"] = fp::bytes_completed.load();
            j["active_chunks"] = fp::active_chunks.load();
            
            j["config"] = {
                {"root_path", config::root_path},
                {"num_workers", config::num_workers},
                {"chunk_size", config::chunk_size},
                {"max_active_chunks", config::max_active_chunks},
                {"num_shards", config::num_shards}
            };
            res.set_content(j.dump(), "application/json");
        });

        svr.Post("/api/reindex", [&](const httplib::Request& req, httplib::Response& res) {
            if (!fp::done.load()) {
                nlohmann::json j;
                j["error"] = "Indexing already in progress";
                res.status = 400;
                res.set_content(j.dump(), "application/json");
                return;
            }

            try {
                auto args = nlohmann::json::parse(req.body);
                if (args.contains("root_path")) config::root_path = args["root_path"].get<std::string>();
                if (args.contains("num_workers")) config::num_workers = args["num_workers"].get<size_t>();
                if (args.contains("chunk_size")) config::chunk_size = args["chunk_size"].get<size_t>();
                if (args.contains("max_active_chunks")) config::max_active_chunks = args["max_active_chunks"].get<size_t>();
                if (args.contains("num_shards")) config::num_shards = args["num_shards"].get<size_t>();
                
                fp::files_completed = 0;
                fp::bytes_completed = 0;
                fp::active_chunks = 0;

                std::thread([]() {
                    idf::reindex_db(true);
                }).detach();

                nlohmann::json out;
                out["status"] = "started";
                res.set_content(out.dump(), "application/json");
            } catch (std::exception& e) {
                res.status = 400;
                res.set_content("{\"error\": \"invalid json settings payload\"}", "application/json");
            }
        });

        svr.Post("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto json_req = nlohmann::json::parse(req.body);
                std::string q = json_req["query"].get<std::string>();
                bool is_phrase = true;
                if (json_req.contains("mode") && json_req["mode"].get<std::string>() == "consecutive") {
                    is_phrase = false;
                }

                std::vector<std::string> search_words;
                std::stringstream ss(q);
                std::string w;
                while (ss >> w) search_words.push_back(w);

                duckdb::DuckDB db("idf.duckdb");
                duckdb::Connection con(db);
                idf::search::Query query_obj(std::move(con), q, is_phrase);
                
                nlohmann::json results = nlohmann::json::array();
                
                query_obj.execute([&](const idf::search::SearchResult& match) -> bool {
                    if (results.size() >= 150) return false;
                    
                    nlohmann::json m;
                    m["path"] = match.path;
                    
                    std::string snip = match.snippet;
                    std::vector<std::pair<size_t, size_t>> intervals;
                    for (const auto& wd : search_words) {
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

                        std::string final_snip = "";
                        size_t merged_idx = 0;
                        bool in_mark = false;

                        for (size_t i = 0; i <= snip.length(); ++i) {
                            while (in_mark && merged_idx < merged.size() && i == merged[merged_idx].second) {
                                final_snip += "</mark>";
                                in_mark = false;
                                merged_idx++;
                            }
                            
                            if (!in_mark && merged_idx < merged.size() && i == merged[merged_idx].first) {
                                final_snip += "<mark>";
                                in_mark = true;
                            }
                            
                            if (i == snip.length()) break;

                            char c = snip[i];
                            if (c == '<') final_snip += "&lt;";
                            else if (c == '>') final_snip += "&gt;";
                            else if (c == '&') final_snip += "&amp;";
                            else final_snip += c;
                        }

                        snip = final_snip;
                    } else {
                        std::string final_snip = "";
                        for (char c : snip) {
                            if (c == '<') final_snip += "&lt;";
                            else if (c == '>') final_snip += "&gt;";
                            else if (c == '&') final_snip += "&amp;";
                            else final_snip += c;
                        }
                        snip = final_snip;
                    }

                    m["snippet"] = snip;
                    results.push_back(m);
                    return true;
                });

                res.set_content(results.dump(), "application/json");
            } catch(...) {
                res.status = 500;
                res.set_content("{\"error\": \"Search sequence execution failure\"}", "application/json");
            }
        });


        fp::done = true; 

        std::cout << "Starting C++ Backend HTTP API on localhost:8814..." << std::endl;
        svr.listen("0.0.0.0", 8814);
    }
}

#endif
