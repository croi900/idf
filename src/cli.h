#ifndef IDF_CLI_H
#define IDF_CLI_H

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <iomanip>
#include <sstream>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "src/search.h"
#include "src/config.hpp"
#include "src/index.h"
#include "src/db_manager.hpp"
#include "src/query_parser.h"
#include "src/history.hpp"

namespace idf {

    inline ftxui::Element highlight_line(const std::string& line, const idf::qp::compiled_query& cq) {
        using namespace ftxui;
        if (cq.highlight_words.empty()) return text(line) | color(Color::GrayLight);

        std::vector<std::pair<size_t, size_t>> intervals;
        for (const auto& w : cq.highlight_words) {
            if (w.empty()) continue;
            size_t pos = 0;
            while ((pos = line.find(w, pos)) != std::string::npos) {
                intervals.push_back({pos, pos + w.size()});
                pos += w.size();
            }
        }
        if (intervals.empty()) return text(line) | color(Color::GrayLight);

        std::sort(intervals.begin(), intervals.end());
        std::vector<std::pair<size_t, size_t>> merged;
        for (auto& iv : intervals) {
            if (merged.empty() || merged.back().second < iv.first) merged.push_back(iv);
            else merged.back().second = std::max(merged.back().second, iv.second);
        }

        Elements parts;
        size_t pos = 0;
        for (const auto& [lo, hi] : merged) {
            if (lo > pos) parts.push_back(text(line.substr(pos, lo - pos)) | color(Color::GrayLight));
            parts.push_back(text(line.substr(lo, hi - lo)) | color(Color::Yellow) | bold);
            pos = hi;
        }
        if (pos < line.size()) parts.push_back(text(line.substr(pos)) | color(Color::GrayLight));
        return hbox(std::move(parts));
    }

    inline ftxui::Element highlight_snippet(const std::string& snippet, const idf::qp::compiled_query& cq) {
        using namespace ftxui;
        Elements lines;
        std::istringstream ss(snippet);
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(highlight_line(line, cq));
        }
        return vbox(std::move(lines));
    }

    inline void cli() {
        auto screen = ftxui::ScreenInteractive::Fullscreen();

        std::string query_text  = "";
        std::string root_path   = idf::config::root_path;
        bool substring_search   = idf::config::enable_substring_search;
        bool is_phrase          = true;
        int  rank_selected      = 0;

        idf::history::history_store hist_store;
        try { hist_store.open("idf_history"); } catch (...) {}
        idf::history::search_observer hist_observer(hist_store);

        struct result_entry {
            std::string path;
            std::string snippet;
        };

        std::vector<result_entry>   results;
        std::vector<std::string>    result_labels;
        int                         selected_result = 0;
        std::atomic<bool>           is_searching(false);
        std::atomic<bool>           is_indexing(false);
        std::mutex                  results_mtx;
        auto                        indexing_start_time = std::chrono::high_resolution_clock::now();

        auto run_search = [&] {
            if (is_searching) return;
            is_searching = true;
            {
                std::lock_guard<std::mutex> lock(results_mtx);
                results.clear();
                result_labels.clear();
            }
            idf::config::enable_substring_search = substring_search;
            switch (rank_selected) {
                case 1:  idf::config::current_rank_strategy = idf::rank_strategy::Alphabetical; break;
                case 2:  idf::config::current_rank_strategy = idf::rank_strategy::DateModified; break;
                default: idf::config::current_rank_strategy = idf::rank_strategy::Score;        break;
            }

            std::thread([&] {
                try {
                    idf::lmdb_env env;
                    env.open("idf_db", 0, 0664);
                    idf::lmdb_dbi files_db, text_db, lang_db, index_db;
                    idf::init_lmdb_dbs(env, files_db, text_db, lang_db, index_db);

                    idf::search::query q(env, files_db, text_db, lang_db, query_text, is_phrase);
                    if (hist_store.is_open()) {
                        q.set_observer(&hist_observer);
                        q.set_history_store(&hist_store);
                    }

                    q.execute([&](const idf::search::search_result& res) {
                        if (!res.snippet.empty()) {
                            std::lock_guard<std::mutex> lock(results_mtx);
                            results.push_back({res.path, res.snippet});
                            result_labels.push_back(res.path);
                        }
                        return true;
                    });
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(results_mtx);
                    results.push_back({"Error", std::string(e.what())});
                    result_labels.push_back("Error");
                }
                if (results.empty()) {
                    results.push_back({"", "No matches found."});
                    result_labels.push_back("No matches found.");
                }
                is_searching = false;
                selected_result = 0;
                screen.PostEvent(ftxui::Event::Custom);
            }).detach();
        };

        auto input_opts = ftxui::InputOption();
        input_opts.on_enter = run_search;
        ftxui::Component q_input = ftxui::Input(&query_text, "path:src/ lang:cpp def:execute content:foo", input_opts);

        ftxui::Component btn_search  = ftxui::Button(" SEARCH ",   run_search, ftxui::ButtonOption::Animated(ftxui::Color::Green));
        ftxui::Component path_input  = ftxui::Input(&root_path,    "Workspace Root");

        ftxui::Component btn_reindex = ftxui::Button(" REINDEX ", [&] {
            if (is_indexing) return;
            is_indexing = true;
            fp::reset_stats();
            indexing_start_time = std::chrono::high_resolution_clock::now();
            idf::config::root_path                = root_path;
            idf::config::enable_substring_search  = substring_search;
            
            std::thread([&] {
                while (is_indexing) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    screen.PostEvent(ftxui::Event::Custom);
                }
            }).detach();

            std::thread([&] {
                try { idf::reindex_db(); } catch (...) {}
                is_indexing = false;
                screen.PostEvent(ftxui::Event::Custom);
            }).detach();
        }, ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

        auto phrase_chk = ftxui::Checkbox("PHRASE",    &is_phrase);
        auto sub_chk    = ftxui::Checkbox("SUBSTRING", &substring_search);

        std::vector<std::string> rank_labels = {"SCORE", "ALPHA", "DATE"};
        auto rank_rad = ftxui::Radiobox(&rank_labels, &rank_selected);

        auto result_list = ftxui::Menu(&result_labels, &selected_result);

        auto container = ftxui::Container::Vertical({
            ftxui::Container::Horizontal({ q_input, btn_search }),
            ftxui::Container::Horizontal({ path_input, phrase_chk, sub_chk, rank_rad, btn_reindex }),
            result_list
        });

        std::vector<std::string> known_types;
        auto update_known_types = [&] {
            try {
                idf::lmdb_env env;
                env.open("idf_db", 0, 0664);
                idf::lmdb_dbi files_db, text_db, lang_db, index_db;
                idf::init_lmdb_dbs(env, files_db, text_db, lang_db, index_db);
                idf::lmdb_txn txn(env, MDB_RDONLY);
                idf::lmdb_cursor cursor(txn, lang_db);
                MDB_val key, data;
                known_types.clear();
                if (cursor.get(key, data, MDB_FIRST)) {
                    do {
                        known_types.emplace_back(static_cast<const char*>(key.mv_data), key.mv_size);
                    } while (cursor.get(key, data, MDB_NEXT_NODUP));
                }
            } catch (...) {}
        };
        update_known_types();

        static const std::vector<std::string> semantic_prefixes = {
            "def:", "ref:", "call:", "import:", "comment:", "symbol:",
        };

        auto renderer = ftxui::Renderer(container, [&] {
            using namespace ftxui;

            auto cq = idf::qp::compile(query_text, is_phrase);

            Elements sugs;
            if (query_text.size() >= 2) {
                auto recent = hist_store.suggest(query_text, 5);
                for (const auto& h : recent) {
                    sugs.push_back(text(h) | color(Color::Blue) | border | dim);
                }
            }
            size_t tp_idx = query_text.rfind("type:");
            if (tp_idx != std::string::npos) {
                std::string partial = query_text.substr(tp_idx + 5);
                size_t added_types = 0;
                for (const auto& t : known_types) {
                    if (t.find(partial) == 0) {
                        sugs.push_back(text(t) | color(Color::Green) | border);
                        added_types++;
                    }
                    if (added_types >= 5) break;
                }
            }

            Element suggestions_box = sugs.empty() ? text("") : hbox({ text(" SUGGEST: ") | dim, hflow(std::move(sugs)) });

            Element results_display = text(" No results. ") | dim;
            {
                std::lock_guard<std::mutex> lock(results_mtx);
                if (!results.empty()) {
                    if (selected_result < 0) selected_result = 0;
                    if (selected_result >= (int)results.size()) selected_result = (int)results.size() - 1;
                    
                    const auto& r = results[selected_result];
                    results_display = vbox({
                        text(r.path) | color(Color::Cyan) | bold,
                        separator(),
                        highlight_snippet(r.snippet, cq)
                    }) | flex;
                }
            }

            Element indexing_status = text("");
            if (is_indexing) {
                auto now = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed = now - indexing_start_time;
                size_t current_bytes = fp::bytes_completed.load();
                size_t current_files = fp::files_completed.load();
                double mib_total = current_bytes / (1024.0 * 1024.0);
                double mib_s = elapsed.count() > 0 ? (mib_total / elapsed.count()) : 0;

                std::stringstream ss;
                ss << std::fixed << std::setprecision(1) << mib_s << " MiB/s";

                indexing_status = hbox({
                    text(" PROGRESS: ") | bold | color(Color::Yellow),
                    text(std::to_string(current_files) + " files") | color(Color::Cyan),
                    separator(),
                    text(ss.str()) | color(Color::Green),
                    separator(),
                    text("Total: " + std::to_string((int)mib_total) + " MiB") | dim
                }) | border | color(Color::Yellow);
            }

            return vbox({
                hbox({
                    text(" IDF ") | bold | bgcolor(Color::Cyan) | color(Color::Black),
                    text(" indexer ") | dim,
                    filler(),
                    text(is_indexing ? " INDEXING DB... " : is_searching ? " SEARCHING... " : " READY ") 
                        | bold | color(is_indexing ? Color::Yellow : is_searching ? Color::Green : Color::GrayDark)
                }),
                separator(),
                hbox({
                    text(" QUERY: ") | bold | color(Color::Green),
                    q_input->Render() | flex,
                    btn_search->Render()
                }),
                suggestions_box,
                separator(),
                hbox({
                    text(" PATH: ") | dim,
                    path_input->Render() | size(WIDTH, LESS_THAN, 40),
                    separator(),
                    phrase_chk->Render(),
                    separator(),
                    sub_chk->Render(),
                    separator(),
                    text(" SORT: ") | dim,
                    rank_rad->Render(),
                    filler(),
                    btn_reindex->Render()
                }),
                separator(),
                hbox({
                    result_list->Render() | size(WIDTH, EQUAL, 40) | vscroll_indicator | frame | border,
                    results_display | border | flex
                }) | flex,
                indexing_status
            }) | border;
        });

        screen.Loop(renderer);
    }
}

#endif
