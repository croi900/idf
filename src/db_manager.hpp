#pragma once
#ifndef DB_MANAGER_HPP
#define DB_MANAGER_HPP

#include <string>
#include <filesystem>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <absl/hash/hash.h>
#include <hpx/hpx.hpp>

#include "lmdb_wrapper.hpp"
#include "dirtree.hpp"

namespace idf {
    namespace fs = std::filesystem;

    struct file_record {
        char     path[1024];
        uint64_t size;
        uint64_t mtime;
        uint64_t atime;
        uint64_t inode;
        uint64_t dev;
        char     lang[16];
        float    score;
    };

    struct text_occurrence {
        uint64_t file_hash;
        uint32_t position;
    };

    struct lang_occurrence {
        uint64_t file_hash;
        uint32_t begin;
        uint32_t end;
    };

    inline void init_lmdb_dbs(lmdb_env& env, lmdb_dbi& files_db, lmdb_dbi& text_tokens_db, lmdb_dbi& lang_tokens_db, lmdb_dbi& file_tokens_index) {
        lmdb_txn txn(env);
        files_db.open(txn, "files", MDB_CREATE);
        text_tokens_db.open(txn, "text_tokens", MDB_CREATE | MDB_DUPSORT);
        lang_tokens_db.open(txn, "lang_tokens", MDB_CREATE | MDB_DUPSORT);
        file_tokens_index.open(txn, "file_tokens_index", MDB_CREATE | MDB_DUPSORT);
        txn.commit();
    }

    inline std::unordered_map<uint64_t, uint64_t> get_existing_mtimes(lmdb_env& env, lmdb_dbi& files_db, const std::string& prefix) {
        std::unordered_map<uint64_t, uint64_t> mtimes;
        lmdb_txn txn(env, MDB_RDONLY);
        lmdb_cursor cursor(txn, files_db);
        
        MDB_val key, data;
        while (cursor.get(key, data, MDB_NEXT)) {
            if (key.mv_size == sizeof(uint64_t) && data.mv_size >= 1076) {
                uint64_t hash = *static_cast<uint64_t*>(key.mv_data);
                file_record* rec = static_cast<file_record*>(data.mv_data);
                
                std::string path(rec->path);
                if (path.rfind(prefix, 0) == 0 || prefix.rfind(path, 0) == 0 || path.front() == '.') {
                    mtimes[hash] = rec->mtime;
                }
            }
        }
        return mtimes;
    }

    inline void delete_file_records(lmdb_env& env, lmdb_dbi& files_db, lmdb_dbi& text_tokens_db, lmdb_dbi& lang_tokens_db, lmdb_dbi& file_tokens_index, const std::vector<uint64_t>& file_hashes) {
        if (file_hashes.empty()) return;
        
        lmdb_txn txn(env);
        
        for (uint64_t hash : file_hashes) {
            MDB_val fh_key{sizeof(uint64_t), &hash};

            lmdb_cursor idx_cursor(txn, file_tokens_index);
            MDB_val idx_data;
            if (idx_cursor.get(fh_key, idx_data, MDB_SET)) {
                do {
                    std::string token_str(static_cast<char*>(idx_data.mv_data), idx_data.mv_size);
                    MDB_val tok_key{idx_data.mv_size, idx_data.mv_data};
                    
                    lmdb_cursor tok_cursor(txn, text_tokens_db);
                    MDB_val tok_data;
                    if (tok_cursor.get(tok_key, tok_data, MDB_SET)) {
                        do {
                            if (tok_data.mv_size >= sizeof(text_occurrence)) {
                                text_occurrence* occ = static_cast<text_occurrence*>(tok_data.mv_data);
                                if (occ->file_hash == hash) {
                                    tok_cursor.del();
                                }
                            }
                        } while (tok_cursor.get(tok_key, tok_data, MDB_NEXT_DUP));
                    }
                } while (idx_cursor.get(fh_key, idx_data, MDB_NEXT_DUP));
            }
            
            files_db.del(txn, fh_key);
            file_tokens_index.del(txn, fh_key); // this removes all dups for this hash
        }
        txn.commit();
        std::cout << "[db_manager] Deleted records for " << file_hashes.size() << " files.\n";
    }

    struct parse_batch {
        dirtree::file_entry file;
        std::string        lang;
        float              score = 0.0f;
        std::vector<std::pair<std::string, text_occurrence>> text_tokens;
        std::vector<std::pair<std::string, lang_occurrence>> lang_tokens;
    };

    class lmdb_writer {
        lmdb_env& env;
        lmdb_dbi& files_db;
        lmdb_dbi& text_db;
        lmdb_dbi& lang_db;
        lmdb_dbi& index_db;

        std::queue<parse_batch> queue;
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> stop{false};
        std::thread worker;

        void flush_batch(std::vector<parse_batch>& batches) {
            if (batches.empty()) return;
            lmdb_txn txn(env);
            for (const auto& b : batches) {
                uint64_t fhash = std::hash<std::string>{}(b.file.path);
                file_record fr;
                std::memset(&fr, 0, sizeof(fr));
                std::strncpy(fr.path, b.file.path.c_str(), sizeof(fr.path) - 1);
                fr.size  = b.file.size;
                fr.mtime = b.file.mtime;
                fr.atime = b.file.atime;
                fr.inode = b.file.inode;
                fr.dev   = b.file.dev;
                std::strncpy(fr.lang, b.lang.c_str(), sizeof(fr.lang) - 1);
                fr.score = b.score;

                MDB_val key_fhash{sizeof(fhash), &fhash};
                MDB_val data_fr{sizeof(fr), &fr};
                files_db.put(txn, key_fhash, data_fr);

                for (const auto& t : b.text_tokens) {
                    if (t.first.empty() || t.first.size() > 255) continue;
                    text_db.put(txn, t.first, std::string_view(reinterpret_cast<const char*>(&t.second), sizeof(text_occurrence)));
                    index_db.put(txn, key_fhash, MDB_val{t.first.size(), const_cast<char*>(t.first.data())});
                }
                for (const auto& l : b.lang_tokens) {
                    if (l.first.empty() || l.first.size() > 255) continue;
                    lang_db.put(txn, l.first, std::string_view(reinterpret_cast<const char*>(&l.second), sizeof(lang_occurrence)));
                }
            }
            txn.commit();
        }

        void run() {
            std::vector<parse_batch> local_batch;
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [this] { return !queue.empty() || stop; });
                    
                    if (stop && queue.empty()) break;

                    while (!queue.empty() && local_batch.size() < 1000) {
                        local_batch.push_back(std::move(queue.front()));
                        queue.pop();
                    }
                }
                try {
                    flush_batch(local_batch);
                } catch (const std::exception& e) {
                    std::cerr << "[lmdb_writer] Exception in flush_batch: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[lmdb_writer] Unknown exception in flush_batch\n";
                }
                local_batch.clear();
            }
        }

    public:
        lmdb_writer(lmdb_env& env, lmdb_dbi& files, lmdb_dbi& texts, lmdb_dbi& langs, lmdb_dbi& index)
            : env(env), files_db(files), text_db(texts), lang_db(langs), index_db(index) {
            worker = std::thread(&lmdb_writer::run, this);
        }

        ~lmdb_writer() {
            {
                std::lock_guard<std::mutex> lock(mtx);
                stop = true;
            }
            cv.notify_one();
            if (worker.joinable()) worker.join();
        }

        void push(parse_batch&& batch) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                queue.push(std::move(batch));
            }
            cv.notify_one();
        }
    };

} // namespace idf

#endif
