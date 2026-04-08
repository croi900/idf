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
#include "src/sharding.hpp"
#include "src/db_manager.hpp"
#include "src/search.h"
#include "src/config.hpp"
#include <limits>
#include <sstream>
#include <fstream>

#ifndef IDF_INDEX_H
#define IDF_INDEX_H

namespace idf {

    idf::im_shard_map global_tokens;
    std::vector<dirtree::FileEntry> global_fl;
    
    void reindex_db() {
        auto run_start_time = std::chrono::system_clock::now();
        std::time_t unix_time = std::chrono::system_clock::to_time_t(run_start_time);
        
        std::string input;
        
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        std::cout << "Root path (default '" << idf::config::root_path << "'): ";
        std::getline(std::cin, input);
        if (!input.empty()) idf::config::root_path = input;

        size_t default_workers = hpx::get_os_thread_count();
        std::cout << "Number of workers (default " << default_workers << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) idf::config::num_workers = std::stoull(input);
        else idf::config::num_workers = default_workers;

        std::cout << "Chunk size (default " << idf::config::chunk_size << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) idf::config::chunk_size = std::stoull(input);

        std::cout << "Max active chunks (default " << idf::config::max_active_chunks << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) idf::config::max_active_chunks = std::stoull(input);

        std::cout << "Allowed extensions space separated: ";
        std::getline(std::cin, input);
        if (!input.empty()) {
            idf::config::allowed_extensions.clear();
            std::stringstream ss(input);
            std::string ext;
            while (ss >> ext) idf::config::allowed_extensions.push_back(ext);
        }

        std::cout << "Number of shards (default " << idf::config::num_shards << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) idf::config::num_shards = std::stoull(input);

        std::cout << "HPX Worker Threads: " << idf::config::num_workers << std::endl;

        global_fl = dirtree::file_list(idf::config::root_path, 16000).to_vector();

        std::cout << "Found " << global_fl.size() << " files." << std::endl;

        duckdb::DuckDB db("idf.duckdb");
        auto existing_mtimes = idf::get_existing_mtimes(db);
        uint64_t start_token_id = idf::get_next_token_id(db);

        std::vector<dirtree::FileEntry> files_to_process;
        std::vector<uint64_t> hashes_to_delete;
        
        for (const auto& f : global_fl) {
            uint64_t file_hash = absl::HashOf(f.path);
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
            std::cout << "Removing old records for " << hashes_to_delete.size() << " files...\n";
            idf::delete_file_records(db, hashes_to_delete);
        }

        if (files_to_process.empty()) {
            std::cout << "Index is up to date. " << global_fl.size() << " files untouched.\n";
            return;
        }

        std::cout << "Indexing " << files_to_process.size() << " modified/new files..." << std::endl;

        auto start_time = std::chrono::high_resolution_clock::now();
        idf::ShardManager shard_manager(idf::config::num_shards, "tokens");
        fp::start_reporter(start_time, idf::config::chunk_size);

        fp::process_file_list(files_to_process, shard_manager);
        shard_manager.flush();
        fp::done = true;

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        double tokenize_time = elapsed.count();
        std::cout << "Finished " << fp::files_completed.load() << " files in " << tokenize_time << "s" << std::endl;


        start_time = std::chrono::high_resolution_clock::now();
        std::mutex map_mutex;
        hpx::for_each(hpx::execution::par,
                      hpx::util::counting_iterator<uint32_t>(0),
                      hpx::util::counting_iterator<uint32_t>(shard_manager.num_shards),
                      [&](uint32_t i) {
                          auto local_shard_data = shard_manager.read_tokens(i);
                          std::lock_guard<std::mutex> lock(map_mutex);
                          for (auto &[word, vec]: local_shard_data) {
                              auto &dest_vec = global_tokens[word];
                              dest_vec.insert(dest_vec.end(),
                                              std::make_move_iterator(vec.begin()),
                                              std::make_move_iterator(vec.end()));
                          }
                      });

        end_time = std::chrono::high_resolution_clock::now();
        double concat_time = static_cast<std::chrono::duration<double>>(end_time - start_time).count();
        std::cout << "Concatenation done in: " << concat_time << "s" << std::endl;
        std::cout << global_tokens.size() << " unique tokens." << std::endl;
        idf::serialize_processed_tokens(global_tokens, "tokens_processed.pb.bin");
        idf::delete_shards(shard_manager);


        idf::load_into_duckdb(db, global_tokens, files_to_process, start_token_id);


        std::ofstream report("report_" + std::to_string(unix_time) + ".txt");
        if (report.is_open()) {
            report << "IDF Indexing Report\n=======================\n";
            report << "Root path:                 " << idf::config::root_path << "\n";
            report << "HPX Worker Threads:        " << idf::config::num_workers << "\n";
            report << "Chunk size:                " << idf::config::chunk_size << "\n";
            report << "Max active chunks:         " << idf::config::max_active_chunks << "\n";
            report << "Number of shards:          " << idf::config::num_shards << "\n";
            report << "Total files tracked:       " << global_fl.size() << "\n";
            report << "Old records removed:       " << hashes_to_delete.size() << "\n";
            report << "Files processed (new/mod): " << files_to_process.size() << "\n";
            report << "Tokenization time:         " << tokenize_time << "s\n";
            report << "Concatenation time:        " << concat_time << "s\n";
            report << "Unique tokens processed:   " << global_tokens.size() << "\n";

            duckdb::Connection con(db);
            auto res = con.Query("SELECT count(*) FROM tokens;");
            if (!res->HasError()) {
                auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
                report << "DuckDB Total Tokens:       " << m_res.GetValue(0, 0).ToString() << "\n";
            }
            res = con.Query("SELECT count(*) FROM token_occurrences;");
            if (!res->HasError()) {
                auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
                report << "DuckDB Total Occurrences:  " << m_res.GetValue(0, 0).ToString() << "\n";
            }
            report << "=======================\n";
            report.close();
        }
    }
}

#define IDF_INDEX_H

#endif //IDF_INDEX_H
