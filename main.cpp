#include <hpx/hpx_main.hpp>
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


int main(int argc, char *argv[]) {
    size_t num_hpx_threads = hpx::get_os_thread_count();
    std::cout << "HPX Worker Threads: " << num_hpx_threads << std::endl;

    auto fl = dirtree::file_list("/home/croi/code/cpp/", 16000).to_vector();
    std::cout << "Found " << fl.size() << " files." << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    idf::ShardManager shard_manager(256, "tokens");
    fp::start_reporter(start_time, fp::CHUNK_SIZE);

    fp::process_file_list(std::move(fl), shard_manager);
    fp::done = true;

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "Finished " << fp::files_completed.load() << " files in " << elapsed.count() << "s" << std::endl;

    start_time = std::chrono::high_resolution_clock::now();

    idf::im_shard_map tokens;
    std::mutex map_mutex;

    hpx::for_each(hpx::execution::par,
        hpx::util::counting_iterator<uint32_t>(0),
        hpx::util::counting_iterator<uint32_t>(shard_manager.num_shards),
        [&](uint32_t i) {
            auto local_shard_data = shard_manager.read_tokens(i);

            std::lock_guard<std::mutex> lock(map_mutex);
            for (auto& [word, vec] : local_shard_data) {
                auto& dest_vec = tokens[word];
                dest_vec.insert(dest_vec.end(),
                                std::make_move_iterator(vec.begin()),
                                std::make_move_iterator(vec.end()));
            }
        });

    //
    // absl::flat_hash_map<std::string, std::vector<std::pair<size_t, size_t>>> tokens;
    //
    // // Optional: If you know roughly how many unique words there are,
    // // reserving space here will prevent expensive re-hashes.
    // // tokens.reserve(1000000);
    //
    // for (uint32_t i = 0; i < shard_manager.num_shards; ++i) {
    //     // 1. Get the data from one shard
    //     auto shard_data = shard_manager.read_tokens(i);
    //
    //     // 2. Insert directly into the final map
    //     for (auto& [word, vec] : shard_data) {
    //         auto& dest_vec = tokens[word];
    //
    //         // 3. Move the data to avoid copies
    //         dest_vec.insert(dest_vec.end(),
    //                         std::make_move_iterator(vec.begin()),
    //                         std::make_move_iterator(vec.end()));
    //     }
    // }
    end_time = std::chrono::high_resolution_clock::now();

    std::cout   << "Concatenation done in: "
                << static_cast<std::chrono::duration<double>>(end_time - start_time).count()
                << "s" << std::endl;


    // for (auto it = tokens.begin(); it != tokens.end(); ++it) {
    //     std::cout << it->first << " -> " ;
    //     for (auto & elem : it->second) {
    //         std::cout << '(' << elem.first << " | " << elem.second  << ") " <<  std::endl;
    //     }
    //     std::cout << std::endl;
    // }
    return 0;
}
