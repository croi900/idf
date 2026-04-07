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
#ifndef IDF_INDEX_H

namespace idf {

    idf::im_shard_map global_tokens;
    std::vector<std::string> global_fl;
    void reindex_db() {

        std::string root_path;

        std::cout << "Root path(default '.'): ";
        std::cin >> root_path;

        if (root_path == "") {
            root_path = ".";
        }

        size_t num_hpx_threads = hpx::get_os_thread_count();
        std::cout << "HPX Worker Threads: " << num_hpx_threads << std::endl;

        global_fl = dirtree::file_list(root_path, 16000).to_vector();

        std::cout << "Found " << global_fl.size() << " files." << std::endl;

        auto start_time = std::chrono::high_resolution_clock::now();
        idf::ShardManager shard_manager(256, "tokens");
        fp::start_reporter(start_time, fp::CHUNK_SIZE);

        fp::process_file_list(global_fl, shard_manager);
        shard_manager.flush();

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end_time - start_time;
        std::cout << "Finished " << fp::files_completed.load() << " files in " << elapsed.count() << "s" << std::endl;


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
        std::cout << "Concatenation done in: "
                << static_cast<std::chrono::duration<double>>(end_time - start_time).count()
                << "s" << std::endl;
        std::cout << global_tokens.size() << std::endl;
        idf::serialize_processed_tokens(global_tokens, "tokens_processed.pb.bin");
        idf::delete_shards(shard_manager);


        duckdb::DuckDB db("idf.duckdb");
        idf::load_into_duckdb(db, global_tokens, global_fl);
    }
}

#define IDF_INDEX_H

#endif //IDF_INDEX_H
