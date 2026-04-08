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
#include <fast_io.h>

#include "src/dirtree.hpp"
#include "src/tokenizer.hpp"
#include "src/sharding.hpp"
#include "src/reporter.hpp"
#include "src/config.hpp"

namespace fp {


    inline void process_file_list(const std::vector<dirtree::FileEntry> &fl, idf::ShardManager &shard_manager) {

        namespace sz = ashvardanian::stringzilla;


        hpx::counting_semaphore<> semaphore(idf::config::max_active_chunks);



        hpx::experimental::task_group tg;
        hpx::for_each(
            hpx::execution::par.with(hpx::execution::experimental::adaptive_static_chunk_size()),
            hpx::util::counting_iterator<uint32_t>(0),
            hpx::util::counting_iterator<uint32_t>(fl.size()),
            [&](uint32_t i) {
                const std::string &path = fl[i].path;

                fast_io::native_file_loader loader(path);
                if (loader.size() == 0) {
                    files_completed.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                auto shared_loader = std::make_shared<fast_io::native_file_loader>(std::move(loader));

                const char *data_ptr = shared_loader->address_begin;
                size_t total_size = shared_loader->size();
                size_t offset = 0;

                while (offset < total_size) {
                    semaphore.acquire();
                    active_chunks.fetch_add(1, std::memory_order_relaxed);

                    size_t end = std::min(offset + idf::config::chunk_size, total_size);

                    if (end < total_size) {
                        sz::string_view tail(data_ptr + end, total_size - end);

                        auto pos = tail.find_first_of(" \n\r\t");

                        if (pos != sz::string_view::npos) {
                            end += pos;
                        } else {
                            end = total_size;
                        }
                    }

                    size_t current_chunk_size = end - offset;

                    tg.run([ &semaphore, i, path, offset, current_chunk_size, shared_loader, &shard_manager]() {
                        std::string_view view(reinterpret_cast<const char *>(shared_loader->address_begin) + offset,
                                              current_chunk_size);

                        auto tokens = idf::tokenize_chunk(view, offset);
                        shard_manager.write_tokens(i,path,tokens);
                        semaphore.release();
                        active_chunks.fetch_sub(1, std::memory_order_relaxed);
                        bytes_completed.fetch_add(current_chunk_size, std::memory_order_relaxed);
                    });

                    offset = end;
                }
                files_completed.fetch_add(1, std::memory_order_relaxed);
            });
        
        tg.wait();

    }
}

#endif
