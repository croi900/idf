#ifndef DIRTREE_HPP
#define DIRTREE_HPP

#include <hpx/hpx.hpp>
#include <hpx/algorithm.hpp>
#include <vector>
#include <string>
#include <filesystem>
#include <deque>
#include <atomic>
#include <memory>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <set>
#include "extensions.hpp"
#include "config.hpp"

namespace dirtree {
    struct linux_dirent64 {
        ino64_t d_ino;
        off64_t d_off;
        unsigned short d_reclen;
        unsigned char d_type;
        char d_name[];
    };



    struct file_entry {
        std::string name;
        std::string path;
        size_t size{0};
        uint64_t mtime{0};
        uint64_t inode{0};
        uint64_t dev{0};
    };

    struct compressed_dir {
        std::string path;
        std::vector<file_entry> files;
    };

    class file_collection {
        std::deque<compressed_dir> dirs;
        size_t total_files = 0;

    public:
        void add_dir(const std::string& path, std::vector<file_entry>&& files_in_dir) {
            if (!files_in_dir.empty()) {
                total_files += files_in_dir.size();
                dirs.push_back({path, std::move(files_in_dir)});
            }
        }

        size_t size() const { return total_files; }

        void merge(file_collection&& other) {
            for (auto& d : other.dirs) dirs.push_back(std::move(d));
            total_files += other.total_files;
        }

        template<typename F>
        void for_each_path(F&& func) const {
            for (const auto& d : dirs)
                for (const auto& f : d.files)
                    func(d.path + "/" + f.name);
        }

        std::vector<file_entry> to_vector() const {
            std::vector<std::pair<size_t, file_entry>> sized_files;
            sized_files.reserve(total_files);
            for (const auto& d : dirs)
                for (auto f : d.files) {
                    f.path = d.path + "/" + f.name;
                    sized_files.push_back({f.size, f});
                }


            hpx::sort(hpx::execution::par, sized_files.begin(), sized_files.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            std::vector<file_entry> result;
            result.reserve(total_files);
            for (auto& p : sized_files) result.push_back(std::move(p.second));
            return result;
        }
    };


    inline void list_files_serial_internal(const std::string& root_path, file_collection& local_buffer, std::set<std::pair<uint64_t, uint64_t>>& visited) {
        std::vector<std::string> stack;
        stack.push_back(root_path);

        struct stat root_st;
        if (lstat(root_path.c_str(), &root_st) == 0) {
            visited.insert({root_st.st_dev, root_st.st_ino});
        }

        std::vector<char> buf(32768);
        std::vector<file_entry> local_files;

        while (!stack.empty()) {
            std::string path = std::move(stack.back());
            stack.pop_back();

            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
            if (fd == -1) continue;

            local_files.clear();

            while (true) {
                int nread = syscall(SYS_getdents64, fd, buf.data(), buf.size());
                if (nread == -1 || nread == 0) break;

                for (int bpos = 0; bpos < nread;) {
                    auto* d = reinterpret_cast<linux_dirent64*>(buf.data() + bpos);
                    std::string_view name(d->d_name);

                    if (name != "." && name != "..") {
                        std::string full_path = path + "/" + std::string(name);
                        struct stat st;
                        if (fstatat(fd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                            if (S_ISLNK(st.st_mode)) {
                                if (stat(full_path.c_str(), &st) != 0) goto next_file;
                            }
                            
                            if (S_ISDIR(st.st_mode)) {
                                if (visited.insert({st.st_dev, st.st_ino}).second) {
                                    stack.push_back(full_path);
                                }
                            } else if (S_ISREG(st.st_mode)) {
                                size_t dot_pos = name.find_last_of('.');
                                if (dot_pos != std::string_view::npos && idf::config::is_allowed_extension(name.substr(dot_pos))) {
                                    local_files.push_back({std::string(name), "", (size_t)st.st_size, (uint64_t)st.st_mtime, (uint64_t)st.st_ino, (uint64_t)st.st_dev});
                                }
                            }
                        }
                    }
                next_file:
                    bpos += d->d_reclen;
                }
            }
            close(fd);

            if (!local_files.empty()) local_buffer.add_dir(path, std::move(local_files));
        }
    }


    inline void list_files_hpx_internal(std::string path,
                                        std::vector<file_collection>& tls_buffers,
                                        std::atomic<int>& active_tasks,
                                        int max_tasks,
                                        hpx::promise<void>& completion_promise) {
        
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
        if (fd == -1) {
            if (active_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
                completion_promise.set_value();
            return;
        }

        std::vector<char> buf(32768);
        std::vector<std::string> subdirs;
        std::vector<file_entry> local_files;

        while (true) {
            int nread = syscall(SYS_getdents64, fd, buf.data(), buf.size());
            if (nread == -1 || nread == 0) break;

            for (int bpos = 0; bpos < nread;) {
                auto* d = reinterpret_cast<linux_dirent64*>(buf.data() + bpos);
                std::string_view name(d->d_name);

                if (name != "." && name != "..") {
                    std::string full_path = path + "/" + std::string(name);
                    struct stat st;
                    if (fstatat(fd, d->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                        if (S_ISDIR(st.st_mode)) { 
                            subdirs.push_back(full_path);
                        } else if (S_ISREG(st.st_mode)) {
                            size_t dot_pos = name.find_last_of('.');
                            if (dot_pos != std::string_view::npos && idf::config::is_allowed_extension(name.substr(dot_pos))) {
                                local_files.push_back({std::string(name), "", (size_t)st.st_size, (uint64_t)st.st_mtime, (uint64_t)st.st_ino, (uint64_t)st.st_dev});
                            }
                        }
                    }
                }
                bpos += d->d_reclen;
            }
        }
        close(fd);

        size_t thread_id = hpx::get_worker_thread_num();
        if (thread_id >= tls_buffers.size()) thread_id = 0;

        if (!local_files.empty()) tls_buffers[thread_id].add_dir(path, std::move(local_files));

        for (auto& subdir : subdirs) {
            if (active_tasks.load(std::memory_order_relaxed) < max_tasks) {
                active_tasks.fetch_add(1, std::memory_order_relaxed);
                hpx::post([subdir = std::move(subdir), &tls_buffers, &active_tasks, max_tasks,
                           &completion_promise]() mutable {
                    list_files_hpx_internal(std::move(subdir), tls_buffers, active_tasks, max_tasks,
                                            completion_promise);
                });
            } else {
                std::set<std::pair<uint64_t, uint64_t>> local_visited;
                list_files_serial_internal(subdir, tls_buffers[thread_id], local_visited);
            }
        }

        if (active_tasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
            completion_promise.set_value();
    }


    inline file_collection file_list(const std::filesystem::path& p, const int max_tasks = 2000) {
        std::string start_path = p.string();
        if (!std::filesystem::exists(p)) return {};

        std::vector<file_collection> tls_buffers(hpx::get_os_thread_count());
        std::atomic<int> active_tasks(1);
        hpx::promise<void> completion_promise;
        auto f = completion_promise.get_future();

        hpx::post([start_path, &tls_buffers, &active_tasks, max_tasks, &completion_promise]() {
            list_files_hpx_internal(start_path, tls_buffers, active_tasks, max_tasks, completion_promise);
        });

        f.get();

        file_collection result;
        for (auto& buf : tls_buffers) result.merge(std::move(buf));
        return result;
    }

    inline file_collection file_list_serial(const std::filesystem::path& p) {
        file_collection result;
        if (!std::filesystem::exists(p)) return result;
        std::set<std::pair<uint64_t, uint64_t>> visited;
        list_files_serial_internal(p.string(), result, visited);
        return result;
    }
}

#endif
