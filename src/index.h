//
// Created by croi on 08.04.2026.
//
#include <hpx/hpx_init.hpp>
#include <hpx/include/parallel_for_loop.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/async.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/include/threads.hpp>
#include <hpx/semaphore.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <iomanip>
#include <thread>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <memory>

#include "src/dirtree.hpp"
#include "src/file_processor.hpp"
#include "src/tokenizer.hpp"
#include "src/db_manager.hpp"
#include "src/search.h"
#include "src/config.hpp"
#include <limits>
#include <sstream>
#include <fstream>

#ifndef IDF_INDEX_H
#define IDF_INDEX_H

namespace idf {

    std::vector<dirtree::file_entry> global_fl;
    
    void reindex_db(bool skip_prompts = true) {
        auto run_start_time = std::chrono::system_clock::now();
        std::time_t unix_time = std::chrono::system_clock::to_time_t(run_start_time);
        
        if (idf::config::num_workers == 0) {
            idf::config::num_workers = hpx::get_os_thread_count();
        }

        idf::config::root_path = std::filesystem::absolute(idf::config::root_path).lexically_normal().string();

        global_fl = dirtree::file_list(idf::config::root_path, 16000).to_vector();

        std::filesystem::create_directories("idf_db");
        idf::lmdb_env env;
        env.open("idf_db", 0, 0664);
        
        idf::lmdb_dbi files_db, text_db, lang_db, index_db;
        idf::init_lmdb_dbs(env, files_db, text_db, lang_db, index_db);

        auto existing_mtimes = idf::get_existing_mtimes(env, files_db, idf::config::root_path);

        std::vector<dirtree::file_entry> files_to_process;
        std::vector<uint64_t> hashes_to_delete;
        
        for (const auto& f : global_fl) {
            uint64_t file_hash = std::hash<std::string>{}(f.path);
            auto it = existing_mtimes.find(file_hash);
            if (it != existing_mtimes.end()) {
                if (f.mtime > it->second) {
                    files_to_process.push_back(f);
                    hashes_to_delete.push_back(file_hash);
                }
                existing_mtimes.erase(it);
            } else {
                files_to_process.push_back(f);
            }
        }
        
        for (const auto& [hash, mtime] : existing_mtimes) {
            hashes_to_delete.push_back(hash);
        }

        if (!hashes_to_delete.empty()) {
            idf::delete_file_records(env, files_db, text_db, lang_db, index_db, hashes_to_delete);
        }

        if (files_to_process.empty()) {
            return;
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        
        fp::reset_stats();

        {
            idf::lmdb_writer writer(env, files_db, text_db, lang_db, index_db);
            fp::process_file_list(files_to_process, writer);
        }
        
        fp::done = true;

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        double tokenize_time = elapsed.count();


        std::ofstream report("report_" + std::to_string(unix_time) + ".txt");
        if (report.is_open()) {
            report << "IDF Indexing Report\n=======================\n";
            report << "Root path:                 " << idf::config::root_path << "\n";
            report << "HPX Worker Threads:        " << idf::config::num_workers << "\n";
            report << "Chunk size:                " << idf::config::chunk_size << "\n";
            report << "Max active chunks:         " << idf::config::max_active_chunks << "\n";
            report << "Total files tracked:       " << global_fl.size() << "\n";
            report << "Old records removed:       " << hashes_to_delete.size() << "\n";
            report << "Files processed (new/mod): " << files_to_process.size() << "\n";
            report << "Tokenization and DB insertion time: " << tokenize_time << "s\n";
            report << "=======================\n";
            report.close();
        }
    }
}

#endif //IDF_INDEX_H
