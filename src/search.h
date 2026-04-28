#ifndef IDF_SEARCH_H
#define IDF_SEARCH_H

#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <map>
#include <mutex>
#include <atomic>
#include <cmath>

#include <stringzilla/stringzilla.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/execution.hpp>

#include "src/lmdb_wrapper.hpp"
#include "src/db_manager.hpp"
#include "src/config.hpp"
#include "src/query_parser.h"
#include "src/history.hpp"

namespace idf::search {

struct search_result {
    std::string path;
    std::string snippet;
};

class query {
public:
    idf::lmdb_env&  env;
    idf::lmdb_dbi&  files_db;
    idf::lmdb_dbi&  text_db;
    idf::lmdb_dbi&  lang_db;

    std::string query_text;
    bool        is_phrase_mode;

    idf::history::search_observer* observer = nullptr;

    query(idf::lmdb_env& env, idf::lmdb_dbi& files_db, idf::lmdb_dbi& text_db, idf::lmdb_dbi& lang_db,
          const std::string& query_text, bool is_phrase = true)
        : env(env), files_db(files_db), text_db(text_db), lang_db(lang_db),
          query_text(query_text), is_phrase_mode(is_phrase) {}

    void set_observer(idf::history::search_observer* obs) { observer = obs; }

private:
    template<typename Callback>
    bool drain_sorted(std::vector<idf::qp::scored_result>& results, Callback& callback) {
        auto cmp = idf::qp::make_comparator(idf::config::current_rank_strategy);
        std::sort(results.begin(), results.end(), cmp);
        for (const auto& r : results) {
            bool proceed = true;
            if constexpr (std::is_same_v<std::invoke_result_t<Callback, const search_result&>, bool>) {
                proceed = callback(search_result{r.path, r.snippet});
            } else {
                callback(search_result{r.path, r.snippet});
            }
            if (!proceed) return false;
        }
        return true;
    }

    template<typename Callback>
    bool execute_structural(const idf::qp::compiled_query& cq, Callback callback) {
        std::vector<idf::lang_occurrence> type_occs;
        {
            idf::lmdb_txn txn(env, MDB_RDONLY);
            for (const auto& type_prefix : cq.type_prefixes) {
                idf::lmdb_cursor cursor(txn, lang_db);
                MDB_val key{type_prefix.size(), const_cast<char*>(type_prefix.data())};
                MDB_val data;
                if (cursor.get(key, data, MDB_SET_RANGE)) {
                    do {
                        std::string_view k_sv(static_cast<const char*>(key.mv_data), key.mv_size);
                        if (k_sv.size() < type_prefix.size() || k_sv.substr(0, type_prefix.size()) != type_prefix) break;
                        if (data.mv_size >= sizeof(idf::lang_occurrence)) {
                            do {
                                type_occs.push_back(*static_cast<idf::lang_occurrence*>(data.mv_data));
                            } while (cursor.get(key, data, MDB_NEXT_DUP));
                        }
                    } while (cursor.get(key, data, MDB_NEXT_NODUP));
                }
            }
        }
        if (type_occs.empty()) return true;

        std::map<uint64_t, std::vector<idf::lang_occurrence>> file_map;
        for (const auto& occ : type_occs) file_map[occ.file_hash].push_back(occ);

        std::vector<uint64_t> file_hashes;
        for (const auto& kv : file_map) file_hashes.push_back(kv.first);

        std::mutex                          collect_mtx;
        std::vector<idf::qp::scored_result>  collected;
        std::atomic<bool>                   limit_reached(false);
        namespace sz = ashvardanian::stringzilla;

        hpx::for_each(hpx::execution::par, file_hashes.begin(), file_hashes.end(), [&](uint64_t fhash) {
            if (limit_reached) return;

            idf::lmdb_txn local_txn(env, MDB_RDONLY);
            MDB_val file_key{sizeof(fhash), (void*)&fhash};
            MDB_val file_data;
            if (!files_db.get(local_txn, file_key, file_data) || file_data.mv_size < sizeof(idf::file_record)) return;

            idf::file_record* rec = static_cast<idf::file_record*>(file_data.mv_data);
            std::string res_path(rec->path);

            std::ifstream ifs(res_path, std::ios::binary);
            if (!ifs.is_open()) return;

            ifs.seekg(0, std::ios::end);
            size_t file_size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            std::string content(file_size, '\0');
            ifs.read(&content[0], file_size);
            ifs.close();

            sz::string_view content_view(content);

            for (const auto& occ : file_map[fhash]) {
                if (limit_reached) return;
                if (occ.begin >= file_size || occ.end > file_size || occ.end < occ.begin) continue;

                sz::string_view node_view = content_view.substr(occ.begin, occ.end - occ.begin);

                bool matches_all = cq.content_words.empty();
                if (!cq.content_words.empty()) {
                    matches_all = true;
                    for (const auto& w : cq.content_words) {
                        if (node_view.find(sz::string_view(w.data(), w.size())) == sz::string_view::npos) {
                            matches_all = false;
                            break;
                        }
                    }
                }

                if (matches_all) {
                    long snip_begin = std::max(0L, static_cast<long>(occ.begin) - 64L);
                    long snip_end   = std::min(static_cast<long>(file_size), static_cast<long>(occ.end) + 64L);
                    std::string snippet = content.substr(snip_begin, snip_end - snip_begin);
                    snippet = "... " + snippet + " ...";

                    if (!cq.passes(res_path, *rec, snippet)) continue;

                    std::lock_guard<std::mutex> lock(collect_mtx);
                    if (limit_reached) return;
                    collected.push_back({res_path, snippet, rec->score, rec->mtime});
                    if (collected.size() >= 150) limit_reached = true;
                }
            }
        });

        return drain_sorted(collected, callback);
    }

    template<typename Callback>
    bool execute_text_only(idf::lmdb_txn& txn, const idf::qp::compiled_query& cq, Callback callback) {
        const auto& words = cq.content_words;
        if (words.empty()) return true;

        std::vector<std::vector<idf::text_occurrence>> word_occs(words.size());
        auto comp = [](const idf::text_occurrence& a, const idf::text_occurrence& b) {
            if (a.file_hash != b.file_hash) return a.file_hash < b.file_hash;
            return a.position < b.position;
        };

        for (size_t i = 0; i < words.size(); ++i) {
            idf::lmdb_cursor cursor(txn, text_db);
            MDB_val key; MDB_val data;

            if (idf::config::enable_substring_search) {
                if (cursor.get(key, data, MDB_FIRST)) {
                    do {
                        std::string_view k_view(static_cast<const char*>(key.mv_data), key.mv_size);
                        if (k_view.find(words[i]) != std::string_view::npos) {
                            do {
                                if (data.mv_size >= sizeof(idf::text_occurrence))
                                    word_occs[i].push_back(*static_cast<idf::text_occurrence*>(data.mv_data));
                            } while (cursor.get(key, data, MDB_NEXT_DUP));
                        }
                    } while (cursor.get(key, data, MDB_NEXT_NODUP));
                }
            } else {
                key.mv_data = const_cast<char*>(words[i].data());
                key.mv_size = words[i].size();
                if (cursor.get(key, data, MDB_SET_RANGE)) {
                    do {
                        std::string_view k_view(static_cast<const char*>(key.mv_data), key.mv_size);
                        if (k_view.length() >= words[i].size() && k_view.substr(0, words[i].size()) == words[i]) {
                            do {
                                if (data.mv_size >= sizeof(idf::text_occurrence))
                                    word_occs[i].push_back(*static_cast<idf::text_occurrence*>(data.mv_data));
                            } while (cursor.get(key, data, MDB_NEXT_DUP));
                        } else {
                            break;
                        }
                    } while (cursor.get(key, data, MDB_NEXT_NODUP));
                }
            }

            if (word_occs[i].empty()) return true;
            std::sort(word_occs[i].begin(), word_occs[i].end(), comp);
        }

        std::vector<idf::qp::scored_result> collected;

        for (const auto& o0 : word_occs[0]) {
            if (collected.size() >= 150) break;
            uint64_t current_file = o0.file_hash;
            uint32_t current_pos  = o0.position;
            bool     matchedAll   = true;
            uint32_t end_pos      = current_pos;

            for (size_t i = 1; i < words.size(); ++i) {
                idf::text_occurrence target{current_file, current_pos};
                auto it = std::lower_bound(word_occs[i].begin(), word_occs[i].end(), target, comp);

                bool found_next = false;
                while (it != word_occs[i].end() && it->file_hash == current_file) {
                    if (it->position > current_pos) {
                        uint32_t diff = it->position - current_pos;
                        if (is_phrase_mode) {
                            if (diff < 2) { found_next = true; current_pos = it->position; end_pos = it->position; break; }
                        } else {
                            if (diff <= words[i-1].length() + 8) { found_next = true; current_pos = it->position; end_pos = it->position; break; }
                        }
                    }
                    ++it;
                }
                if (!found_next) { matchedAll = false; break; }
            }

            if (matchedAll) {
                MDB_val file_key{sizeof(current_file), &current_file};
                MDB_val file_data;
                if (files_db.get(txn, file_key, file_data) && file_data.mv_size >= sizeof(idf::file_record)) {
                    idf::file_record* rec = static_cast<idf::file_record*>(file_data.mv_data);
                    std::string res_path(rec->path);

                    std::ifstream ifs(res_path, std::ios::binary);
                    std::string snippet;
                    if (ifs.is_open()) {
                        long start_pos  = std::max(0L, static_cast<long>(o0.position) - 128L);
                        ifs.seekg(start_pos);
                        long phrase_span = end_pos - o0.position + words.back().length();
                        long fetch_len   = std::min(4096L, 256L + phrase_span);
                        std::vector<char> buf(fetch_len + 1, 0);
                        ifs.read(buf.data(), fetch_len);
                        snippet = std::string(buf.data(), ifs.gcount());
                        snippet = "... " + snippet + " ...";
                        ifs.close();
                    }

                    if (!cq.passes(res_path, *rec, snippet)) continue;
                    collected.push_back({res_path, snippet, rec->score, rec->mtime});
                }
            }
        }

        return drain_sorted(collected, callback);
    }

public:
    template<typename Callback>
    bool execute(Callback callback) {
        auto cq = idf::qp::compile(query_text, is_phrase_mode);

        bool ok;
        if (!cq.type_prefixes.empty()) {
            ok = execute_structural(cq, callback);
        } else {
            idf::lmdb_txn txn(env, MDB_RDONLY);
            ok = execute_text_only(txn, cq, callback);
        }

        if (observer) observer->on_query_executed(query_text);
        return ok;
    }
};

}

#endif
