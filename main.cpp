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
#include "src/tokenizer.hpp"
#include "src/sharding.hpp"

constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024;
constexpr size_t MAX_ACTIVE_CHUNKS = 4000;

int main(int argc, char *argv[]) {
    size_t num_hpx_threads = hpx::get_os_thread_count();
    std::cout << "HPX Worker Threads: " << num_hpx_threads << std::endl;

    auto fl = dirtree::file_list("/", 16000).to_vector();
    std::cout << "Found " << fl.size() << " files." << std::endl;

    std::atomic<size_t> files_completed{0};
    std::atomic<size_t> chunks_completed{0};
    std::atomic<size_t> active_chunks{0};
    std::atomic<bool> done{false};

    hpx::counting_semaphore<MAX_ACTIVE_CHUNKS> semaphore(MAX_ACTIVE_CHUNKS);
    auto start_time = std::chrono::high_resolution_clock::now();
    idf::ShardManager shard_manager(256, "tokens");

    std::thread reporter([&]() {
        size_t last_chunks = 0;
        auto last_time = start_time;
        while (!done) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = std::chrono::high_resolution_clock::now();
            size_t current_chunks = chunks_completed.load();
            size_t current_files = files_completed.load();

            std::chrono::duration<double> total_elapsed = now - start_time;
            std::chrono::duration<double> delta_elapsed = now - last_time;

            double mib_total = (current_chunks * CHUNK_SIZE) / (1024.0 * 1024.0);
            double mib_s = ((current_chunks - last_chunks) * CHUNK_SIZE) / (1024.0 * 1024.0 * delta_elapsed.count());

            std::cout << "\rFiles: " << current_files
                    << " | Chunks: " << current_chunks
                    << " | In-Flight: " << active_chunks.load()
                    << " | " << std::fixed << std::setprecision(1) << mib_s << " MiB/s"
                    << " | Total: " << std::fixed << std::setprecision(1) << mib_total << " MiB" << std::flush;

            last_chunks = current_chunks;
            last_time = now;
        }
        std::cout << std::endl;
    });


    hpx::for_each(hpx::execution::par.with(hpx::execution::dynamic_chunk_size()),
                  hpx::util::counting_iterator<uint32_t>(0),
                  hpx::util::counting_iterator<uint32_t>(fl.size()),
                  [&](uint32_t i) {
                      const std::string &path = fl[i];
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
                          semaphore.acquire();
                          active_chunks.fetch_add(1, std::memory_order_relaxed);

                          hpx::post([&, i, path, size, fd]() {
                              std::string buffer;
                              buffer.resize(size);
                              if (read(fd, buffer.data(), size) == (ssize_t) size) {
                                  auto tokens = idf::tokenize_chunk(buffer, 0);
                                  shard_manager.write_tokens(i, path, tokens);
                              }
                              close(fd);
                              chunks_completed.fetch_add(1, std::memory_order_relaxed);
                              active_chunks.fetch_sub(1, std::memory_order_relaxed);
                              semaphore.release();
                          });
                      } else {
                          void *addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                          close(fd);
                          if (addr == MAP_FAILED) {
                              files_completed.fetch_add(1, std::memory_order_relaxed);
                              return;
                          }

                          madvise(addr, size, MADV_SEQUENTIAL | MADV_HUGEPAGE);

                          auto mmap_ptr = std::shared_ptr<char>(static_cast<char *>(addr), [size](char *p) {
                              munmap(p, size);
                          });

                          size_t offset = 0;
                          while (offset < size) {
                              semaphore.acquire();
                              active_chunks.fetch_add(1, std::memory_order_relaxed);

                              size_t end = std::min(offset + CHUNK_SIZE, size);
                              if (end < size) {
                                  while (end < size && !std::isspace(mmap_ptr.get()[end])) {
                                      end++;
                                  }
                              }

                              size_t current_chunk_size = end - offset;
                              hpx::post([&, i, path, offset, current_chunk_size, mmap_ptr]() {
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


    while (active_chunks.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    done = true;
    if (reporter.joinable()) reporter.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    std::cout << "Finished " << files_completed.load() << " files in " << elapsed.count() << "s" << std::endl;

    return 0;
}
