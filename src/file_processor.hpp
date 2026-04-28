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
#include <tree_sitter/api.h>

#include "src/dirtree.hpp"
#include "src/tokenizer.hpp"
#include "src/reporter.hpp"
#include "src/config.hpp"
#include "src/db_manager.hpp"
#include "src/parser.h"
#include "src/query_parser.h"


namespace fp {

    inline const TSLanguage* get_language_for_extension(std::string_view ext) {
        if (ext == ".bash" || ext == ".sh") return tree_sitter_bash();
        if (ext == ".c" || ext == ".h") return tree_sitter_c();
        if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" || ext == ".hh" || ext == ".hxx") return tree_sitter_cpp();
        if (ext == ".js" || ext == ".jsx") return tree_sitter_javascript();
        if (ext == ".lua") return tree_sitter_lua();
        if (ext == ".md" || ext == ".markdown") return tree_sitter_markdown();
        if (ext == ".py" || ext == ".pyi") return tree_sitter_python();
        if (ext == ".scm") return tree_sitter_query();
        if (ext == ".rs") return tree_sitter_rust();
        if (ext == ".vim") return tree_sitter_vim();
        if (ext == ".txt") return tree_sitter_vimdoc();
        return nullptr;
    }

    inline std::string lang_string_for_extension(std::string_view ext) {
        if (ext == ".bash" || ext == ".sh")  return "bash";
        if (ext == ".c")                       return "c";
        if (ext == ".h")                       return "c";
        if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".c++" || ext == ".hh"  || ext == ".hxx") return "cpp";
        if (ext == ".js" || ext == ".jsx")  return "js";
        if (ext == ".lua")                    return "lua";
        if (ext == ".md" || ext == ".markdown") return "markdown";
        if (ext == ".py" || ext == ".pyi")  return "python";
        if (ext == ".scm")                    return "query";
        if (ext == ".rs")                     return "rust";
        if (ext == ".vim")                    return "vim";
        if (ext == ".txt")                    return "text";
        return "";
    }

    inline void process_file_list(const std::vector<dirtree::file_entry> &fl, idf::lmdb_writer &writer) {
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
                
                std::string ext = std::filesystem::path(path).extension().string();
                const TSLanguage* lang = get_language_for_extension(ext);
                uint64_t fhash = std::hash<std::string>{}(path);

                if (lang) {
                    semaphore.acquire();
                    active_chunks.fetch_add(1, std::memory_order_relaxed);
                    
                    tg.run([lang, data_ptr, total_size, fhash, i, path, shared_loader, fl, &writer, &semaphore]() {
                        std::string_view source_view(data_ptr, total_size);
                        
                        idf::parse_batch batch;
                        batch.file  = fl[i];
                        batch.lang  = lang_string_for_extension(std::filesystem::path(fl[i].path).extension().string());
                        batch.score = idf::qp::score_path(fl[i], batch.lang);

                        auto tokens = idf::tokenize_chunk(source_view, 0);
                        for (const auto& t : tokens) {
                            batch.text_tokens.emplace_back(std::string(t.word), idf::text_occurrence{fhash, static_cast<uint32_t>(t.pos)});
                        }

                        auto& ts_parser = idf::parser::parser_pool::get_instance(lang);
                        auto tree = ts_parser.parse(source_view);

                        if (tree) {
                            thread_local std::vector<idf::parser::symbol_info> flat_list;
                            ts_parser.flatten_tree_dfs(flat_list);
                            
                            for (const auto& sym : flat_list) {
                                std::string_view node_type = ts_parser.symbol_name(sym.type);
                                batch.lang_tokens.emplace_back(std::string(node_type), idf::lang_occurrence{fhash, sym.start_byte, sym.end_byte});
                            }
                        }
                        
                        writer.push(std::move(batch));
                        semaphore.release();
                        active_chunks.fetch_sub(1, std::memory_order_relaxed);
                        bytes_completed.fetch_add(total_size, std::memory_order_relaxed);
                    });
                } else {
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

                        tg.run([&semaphore, i, fl, offset, fhash, current_chunk_size, shared_loader, &writer, ext]() {
                            std::string_view view(reinterpret_cast<const char *>(shared_loader->address_begin) + offset, current_chunk_size);
                            auto tokens = idf::tokenize_chunk(view, offset);
                            
                            idf::parse_batch batch;
                            batch.file  = fl[i];
                            batch.lang  = lang_string_for_extension(ext);
                            batch.score = idf::qp::score_path(fl[i], batch.lang);
                            for (const auto& t : tokens) {
                                batch.text_tokens.emplace_back(std::string(t.word), idf::text_occurrence{fhash, static_cast<uint32_t>(t.pos)});
                            }
                            writer.push(std::move(batch));
                            
                            semaphore.release();
                            active_chunks.fetch_sub(1, std::memory_order_relaxed);
                            bytes_completed.fetch_add(current_chunk_size, std::memory_order_relaxed);
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
