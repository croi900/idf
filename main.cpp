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

    auto fl = dirtree::file_list("/home/croi", 16000).to_vector();
    std::cout << "Found " << fl.size() << " files." << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();
    idf::ShardManager shard_manager(256, "tokens");
    fp::start_reporter(start_time, fp::CHUNK_SIZE);



    fp::process_file_list(std::move(fl), shard_manager);
    fp::done = true;

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    std::cout << "Finished " << fp::files_completed.load() << " files in " << elapsed.count() << "s" << std::endl;


    return 0;
}
