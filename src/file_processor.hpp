#ifndef IDF_FILE_PROCESSOR_HPP
#define IDF_FILE_PROCESSOR_HPP

#include <string>
#include <vector>
#include <span>
#include <hpx/hpx_main.hpp>
#include <hpx/include/parallel_for_loop.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/async.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/include/threads.hpp>
#include <iostream>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <memory>
#include <mutex>
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
        if (ext == ".bash" || ext == ".sh")  return tree_sitter_bash();
        if (ext == ".c"   || ext == ".h")    return tree_sitter_c();
        if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".c++" || ext == ".hh"  || ext == ".hxx") return tree_sitter_cpp();
        if (ext == ".js"  || ext == ".jsx")  return tree_sitter_javascript();
        if (ext == ".lua")                   return tree_sitter_lua();
        if (ext == ".md"  || ext == ".markdown") return tree_sitter_markdown();
        if (ext == ".py"  || ext == ".pyi")  return tree_sitter_python();
        if (ext == ".scm")                   return tree_sitter_query();
        if (ext == ".rs")                    return tree_sitter_rust();
        if (ext == ".vim")                   return tree_sitter_vim();
        if (ext == ".txt")                   return tree_sitter_vimdoc();
        return nullptr;
    }

    inline std::string lang_string_for_extension(std::string_view ext) {
        if (ext == ".bash" || ext == ".sh")  return "bash";
        if (ext == ".c")                     return "c";
        if (ext == ".h")                     return "c";
        if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".c++" || ext == ".hh"  || ext == ".hxx") return "cpp";
        if (ext == ".js"  || ext == ".jsx")  return "js";
        if (ext == ".lua")                   return "lua";
        if (ext == ".md"  || ext == ".markdown") return "markdown";
        if (ext == ".py"  || ext == ".pyi")  return "python";
        if (ext == ".scm")                   return "query";
        if (ext == ".rs")                    return "rust";
        if (ext == ".vim")                   return "vim";
        if (ext == ".txt")                   return "text";
        return "";
    }

    struct loaded_file {
        dirtree::file_entry  entry;
        std::vector<char>    data;
        std::string          lang;
        uint64_t             hash;
        const TSLanguage*    ts_lang;
    };

    inline std::vector<loaded_file> load_batch(std::span<const dirtree::file_entry> files) {
        std::vector<loaded_file> out;
        out.reserve(files.size());

        for (const auto& fe : files) {
            const std::string& path = fe.path;
            std::string ext = std::filesystem::path(path).extension().string();

            int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd == -1) {
                files_completed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            struct stat st;
            if (::fstat(fd, &st) != 0 || st.st_size == 0) {
                ::close(fd);
                files_completed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            size_t size = static_cast<size_t>(st.st_size);
            std::vector<char> buf(size);

            ssize_t n = ::read(fd, buf.data(), size);
            ::close(fd);

            if (n <= 0) {
                files_completed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            buf.resize(static_cast<size_t>(n));

            loaded_file lf;
            lf.entry   = fe;
            lf.data    = std::move(buf);
            lf.lang    = lang_string_for_extension(ext);
            lf.hash    = std::hash<std::string>{}(path);
            lf.ts_lang = (lf.data.size() <= idf::config::max_ts_file_bytes)
                             ? get_language_for_extension(ext) : nullptr;
            out.push_back(std::move(lf));
        }
        return out;
    }

    inline void parse_loaded_batch(std::vector<loaded_file>& batch,
                                   idf::lmdb_writer& writer) {
        std::mutex                       collect_mtx;
        std::vector<idf::parse_batch>    results;
        results.reserve(batch.size());

        hpx::for_each(
            hpx::execution::par,
            batch.begin(), batch.end(),
            [&](loaded_file& lf) {
                std::string_view sv(lf.data.data(), lf.data.size());

                idf::parse_batch pb;
                pb.file  = lf.entry;
                pb.lang  = lf.lang;
                pb.score = idf::qp::score_path(lf.entry, lf.lang);

                auto tokens = idf::tokenize_chunk(sv, 0);
                pb.text_tokens.reserve(tokens.size());
                for (const auto& t : tokens)
                    pb.text_tokens.emplace_back(std::string(t.word),
                        idf::text_occurrence{lf.hash, static_cast<uint32_t>(t.pos)});

                if (lf.ts_lang) {
                    auto& ts_p = idf::parser::parser_pool::get_instance(lf.ts_lang);
                    auto  tree = ts_p.parse(sv);
                    if (tree) {
                        thread_local std::vector<idf::parser::symbol_info> flat;
                        ts_p.flatten_tree_dfs(flat);
                        pb.lang_tokens.reserve(flat.size());
                        for (const auto& sym : flat) {
                            std::string_view node_type = ts_p.symbol_name(sym.type);
                            pb.lang_tokens.emplace_back(std::string(node_type),
                                idf::lang_occurrence{lf.hash, sym.start_byte, sym.end_byte});
                        }
                    }
                }

                bytes_completed.fetch_add(lf.data.size(), std::memory_order_relaxed);
                files_completed.fetch_add(1, std::memory_order_relaxed);

                std::lock_guard<std::mutex> lock(collect_mtx);
                results.push_back(std::move(pb));
            }
        );

        for (auto& pb : results)
            writer.push(std::move(pb));
    }

    inline void process_file_list(const std::vector<dirtree::file_entry>& fl,
                                  idf::lmdb_writer& writer) {
        const size_t batch_sz = idf::config::io_batch_size;
        size_t total = fl.size();

        for (size_t start = 0; start < total; start += batch_sz) {
            size_t end = std::min(start + batch_sz, total);
            std::span<const dirtree::file_entry> slice(fl.data() + start, end - start);

            auto loaded = load_batch(slice);
            parse_loaded_batch(loaded, writer);
        }
    }

}

#endif
