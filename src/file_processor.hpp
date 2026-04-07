

#ifndef IDF_FILE_PROCESSOR_HPP
#define IDF_FILE_PROCESSOR_HPP

#include <string>
#include <vector>
#include <hpx/hpx_main.hpp>
#include <hpx/include/parallel_for_loop.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/async.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/include/threads.hpp>
#include <hpx/semaphore.hpp>
#include <hpx/experimental/task_group.hpp>
#include <iostream>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <thread>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <memory>

#include "src/dirtree.hpp"
#include "src/tokenizer.hpp"
#include "src/sharding.hpp"
#include "src/reporter.hpp"

namespace fp {
    constexpr size_t CHUNK_SIZE = 1024;//512 * 1024 * 1024;
    constexpr size_t MAX_ACTIVE_CHUNKS = 10240;




    inline void process_file_list(const std::vector<std::string>& fl, idf::ShardManager& shard_manager) {
        hpx::counting_semaphore<MAX_ACTIVE_CHUNKS> semaphore(MAX_ACTIVE_CHUNKS);
        hpx::experimental::task_group tg;

        hpx::for_each(
            hpx::execution::par.with(hpx::execution::static_chunk_size(1)),
            hpx::util::counting_iterator<uint32_t>(0),
            hpx::util::counting_iterator<uint32_t>(fl.size()),
            [&](uint32_t i) {
                const std::string& path = fl[i];
                int fd = open(path.c_str(), O_RDONLY);
                if (fd == -1) {
                    files_completed.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                struct stat st;
                if (fstat(fd, &st) == -1 || st.st_size == 0) {
                    close(fd);
                    files_completed.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                size_t size = st.st_size;

                if (size <= CHUNK_SIZE) {


                    void* addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
                    close(fd);
                    if (addr == MAP_FAILED) {
                        // std::cout << "Memroy allocation failed for " << path << ", size: " << size << std::endl;
                        files_completed.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    semaphore.acquire();
                    active_chunks.fetch_add(1, std::memory_order_relaxed);


                    tg.run([&shard_manager, &semaphore, i, path, size, addr]() {
                        std::string_view view(static_cast<char*>(addr), size);
                        auto tokens = idf::tokenize_chunk(view, 0);

                        shard_manager.write_tokens(i, path, tokens);
                        munmap(addr, size);
                        chunks_completed.fetch_add(1, std::memory_order_relaxed);
                        active_chunks.fetch_sub(1, std::memory_order_relaxed);
                        semaphore.release();
                    });



                } else {


                    void* addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                    close(fd);
                    if (addr == MAP_FAILED) {
                        files_completed.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }

                    madvise(addr, size, MADV_SEQUENTIAL | MADV_HUGEPAGE);

                    auto mmap_ptr = std::shared_ptr<char>(
                        static_cast<char*>(addr), [size](char* p) { munmap(p, size); });

                    size_t offset = 0;
                    while (offset < size) {
                        semaphore.acquire();
                        active_chunks.fetch_add(1, std::memory_order_relaxed);

                        size_t end = std::min(offset + CHUNK_SIZE, size);
                        

                        size_t current_chunk_size = end - offset;
                        tg.run([&shard_manager, &semaphore, i, path, offset, current_chunk_size, mmap_ptr]() {
                            std::string_view view(mmap_ptr.get() + offset, current_chunk_size);
                            auto tokens = idf::tokenize_chunk(view, offset);
                            shard_manager.write_tokens(i, path, tokens);
                            chunks_completed.fetch_add(1, std::memory_order_relaxed);
                            active_chunks.fetch_sub(1, std::memory_order_relaxed);
                            semaphore.release();
                        });
                        offset = end;
                    }
                }
                files_completed.fetch_add(1, std::memory_order_relaxed);
            });




        tg.wait();
    }
}

#endif
