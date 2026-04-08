#pragma once
#ifndef DB_MANAGER_HPP
#define DB_MANAGER_HPP

#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <duckdb.hpp>

#include "tokens_processed.pb.h"
#include <absl/hash/hash.h>
#include <hpx/hpx.hpp>
#include <hpx/algorithm.hpp>
#include "sharding.hpp"
#include "dirtree.hpp"
#include <map>

namespace idf {
    namespace fs = std::filesystem;
    namespace gp_io = google::protobuf::io;


    inline void serialize_processed_tokens(
        const im_shard_map &tokens,
        const std::string &out_path) {
        idf::TokensProcessed processed;

        uint64_t token_id = 0;
        for (const auto &[word, occurrences]: tokens) {
            auto *entry = processed.add_entries();
            entry->set_token(word);
            entry->set_id(token_id);


            for (const auto &[file_hash, position]: occurrences) {
                auto *occ = processed.add_occurrences();
                occ->set_token_id(token_id);
                occ->set_position(position);
                occ->set_file_hash(file_hash);
            }

            ++token_id;
        }

        uint32_t msg_size = static_cast<uint32_t>(processed.ByteSizeLong());

        int fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            std::cerr << "[db_manager] Failed to open " << out_path << " for writing\n";
            return;
        }
        {
            gp_io::FileOutputStream fos(fd, 256 * 1024);
            gp_io::CodedOutputStream cos(&fos);
            cos.WriteVarint32(msg_size);
            processed.SerializeWithCachedSizes(&cos);
        }
        close(fd);

        std::cout << "[db_manager] Wrote " << processed.entries_size() << " token entries"
                << " and " << processed.occurrences_size() << " occurrences"
                << " → " << out_path << "\n";
    }


    inline void delete_shards(const shard_manager &mgr) {
        std::error_code ec;
        size_t removed = 0;
        for (uint32_t i = 0; i < mgr.num_shards; ++i) {
            fs::path p = fs::path("shards") / (mgr.prefix + "_" + std::to_string(i) + ".bin");
            if (fs::remove(p, ec)) { ++removed; }
        }

        fs::remove("shards", ec);
        std::cout << "[db_manager] Deleted " << removed << " shard files\n";
    }


    inline std::unordered_map<uint64_t, uint64_t> get_existing_mtimes(duckdb::DuckDB& db, const std::string& prefix) {
        duckdb::Connection con(db);
        std::unordered_map<uint64_t, uint64_t> mtimes;
        
        auto res = con.Query("SELECT hash, mtime, path FROM files;");
        if (res->HasError()) {
            con.Query("ALTER TABLE files ADD COLUMN mtime UBIGINT DEFAULT 0;");
            con.Query("ALTER TABLE files ADD COLUMN size UBIGINT DEFAULT 0;");
            con.Query("ALTER TABLE files ADD COLUMN inode UBIGINT DEFAULT 0;");
            con.Query("ALTER TABLE files ADD COLUMN dev UBIGINT DEFAULT 0;");
            res = con.Query("SELECT hash, mtime, path FROM files;");
            if (res->HasError()) return mtimes;
        }

        auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
        for (duckdb::idx_t i = 0; i < m_res.RowCount(); ++i) {
            std::string path = m_res.GetValue(2, i).GetValue<std::string>();
            if (path.rfind(prefix, 0) == 0 || prefix.rfind(path, 0) == 0 || path.front() == '.') {
                mtimes[m_res.GetValue(0, i).GetValue<uint64_t>()] = m_res.GetValue(1, i).GetValue<uint64_t>();
            }
        }
        return mtimes;
    }

    inline void delete_file_records(duckdb::DuckDB &db, const std::vector<uint64_t>& file_hashes) {
        if (file_hashes.empty()) return;
        duckdb::Connection con(db);
        
        std::string hashes_list = "";
        for (size_t i = 0; i < file_hashes.size(); ++i) {
            if (i > 0) hashes_list += ", ";
            hashes_list += std::to_string(file_hashes[i]);
        }
        
        auto res = con.Query("DELETE FROM token_occurrences WHERE file_hash IN (" + hashes_list + ");");
        if (res->HasError()) std::cerr << "[db_manager] Error deleting token occurrences: " << res->GetError() << "\n";

        res = con.Query("DELETE FROM files WHERE hash IN (" + hashes_list + ");");
        if (res->HasError()) std::cerr << "[db_manager] Error deleting files: " << res->GetError() << "\n";
    }

    inline uint64_t get_next_token_id(duckdb::DuckDB &db) {
        duckdb::Connection con(db);
        auto res = con.Query("SELECT MAX(id) FROM tokens;");
        if (!res->HasError()) {
            auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
            if (m_res.RowCount() > 0 && !m_res.GetValue(0, 0).IsNull()) {
                return m_res.GetValue(0, 0).GetValue<uint64_t>() + 1;
            }
        }
        return 0;
    }

    inline void load_into_duckdb(
        duckdb::DuckDB &db,
        const im_shard_map &tokens,
        const std::vector<dirtree::FileEntry> &file_list,
        uint64_t start_token_id = 0) {
        duckdb::Connection con(db);

        auto run = [&](const std::string &sql) {
            auto res = con.Query(sql);
            if (res->HasError()) {
                std::cerr << "[db_manager] DuckDB error: " << res->GetError() << "\n";
            }
        };


        run(R"(
            CREATE TABLE IF NOT EXISTS tokens (
                id    UBIGINT PRIMARY KEY,
                token BLOB NOT NULL
            );
        )");

        run(R"(
            CREATE TABLE IF NOT EXISTS token_occurrences (
                token_id  UBIGINT NOT NULL,
                position  UBIGINT NOT NULL,
                file_hash UBIGINT NOT NULL
            );
        )");

        run(R"(
            CREATE TABLE IF NOT EXISTS files (
                hash UBIGINT PRIMARY KEY,
                path VARCHAR NOT NULL,
                size UBIGINT NOT NULL,
                mtime UBIGINT NOT NULL,
                inode UBIGINT NOT NULL,
                dev UBIGINT NOT NULL
            );
        )");



        std::unordered_map<std::string, uint64_t> existing_tokens;
        auto t_res = con.Query("SELECT id, token FROM tokens;");
        if (!t_res->HasError()) {
            auto& mr = static_cast<duckdb::MaterializedQueryResult&>(*t_res);
            for (duckdb::idx_t i = 0; i < mr.RowCount(); ++i) {
                auto id = mr.GetValue(0, i).GetValue<uint64_t>();
                auto word_blob = mr.GetValue(1, i).GetValue<std::string>();
                existing_tokens[word_blob] = id;
            }
        }


        std::vector<const std::pair<const std::string, std::vector<std::pair<size_t, size_t>>>*> token_ptrs;
        token_ptrs.reserve(tokens.size());
        for (const auto& kv : tokens) {
            token_ptrs.push_back(&kv);
        }

        std::atomic<uint64_t> current_token_id{start_token_id};

        size_t chunk_size = 10000;
        size_t num_chunks = (token_ptrs.size() + chunk_size - 1) / chunk_size;

        if (num_chunks > 0) {
            hpx::for_each(hpx::execution::par,
                hpx::util::counting_iterator<size_t>(0),
                hpx::util::counting_iterator<size_t>(num_chunks),
                [&](size_t chunk_idx) {

                    duckdb::Connection local_con(db);
                    duckdb::Appender local_app_tokens(local_con, "tokens");
                    duckdb::Appender local_app_occ(local_con, "token_occurrences");

                    size_t start = chunk_idx * chunk_size;
                    size_t end = std::min(start + chunk_size, token_ptrs.size());

                    for (size_t i = start; i < end; ++i) {
                        const auto& [word, occurrences] = *(token_ptrs[i]);
                        uint64_t token_id;
                        

                        auto it = existing_tokens.find(word);
                        if (it != existing_tokens.end()) {
                            token_id = it->second;
                        } else {
                            token_id = current_token_id.fetch_add(1, std::memory_order_relaxed);
                            local_app_tokens.BeginRow();
                            local_app_tokens.Append((uint64_t) token_id);
                            local_app_tokens.Append(duckdb::Value::BLOB((const uint8_t *) word.data(), word.size()));
                            local_app_tokens.EndRow();
                        }

                        for (const auto &[file_hash, position]: occurrences) {
                            local_app_occ.BeginRow();
                            local_app_occ.Append((uint64_t) token_id);
                            local_app_occ.Append((uint64_t) position);
                            local_app_occ.Append((uint64_t) file_hash);
                            local_app_occ.EndRow();
                        }
                    }

                    local_app_tokens.Flush();
                    local_app_tokens.Close();
                    local_app_occ.Flush();
                    local_app_occ.Close();
                });
        }

        size_t file_chunk_size = 5000;
        size_t num_file_chunks = (file_list.size() + file_chunk_size - 1) / file_chunk_size;
        
        if (num_file_chunks > 0) {
            hpx::for_each(hpx::execution::par,
                hpx::util::counting_iterator<size_t>(0),
                hpx::util::counting_iterator<size_t>(num_file_chunks),
                [&](size_t chunk_idx) {
                    duckdb::Connection local_con(db);
                    duckdb::Appender local_app_files(local_con, "files");

                    size_t start = chunk_idx * file_chunk_size;
                    size_t end = std::min(start + file_chunk_size, file_list.size());

                    for (size_t i = start; i < end; ++i) {
                        const auto &entry = file_list[i];
                        local_app_files.BeginRow();
                        local_app_files.Append((uint64_t) std::hash<std::string>{}(entry.path));
                        local_app_files.Append(duckdb::Value(entry.path));
                        local_app_files.Append((uint64_t) entry.size);
                        local_app_files.Append((uint64_t) entry.mtime);
                        local_app_files.Append((uint64_t) entry.inode);
                        local_app_files.Append((uint64_t) entry.dev);
                        local_app_files.EndRow();
                    }

                    local_app_files.Flush();
                    local_app_files.Close();
                });
        }

        run("CREATE INDEX IF NOT EXISTS idx_tokens_token ON tokens(token);");
        run("CREATE INDEX IF NOT EXISTS idx_occ_token_id ON token_occurrences(token_id);");
        run("CREATE INDEX IF NOT EXISTS idx_occ_file_hash ON token_occurrences(file_hash);");
        run("CREATE INDEX IF NOT EXISTS idx_occ_file_tok_pos ON token_occurrences(file_hash, token_id, position);");


        auto res = con.Query("SELECT count(*) FROM tokens;");
        if (!res->HasError()) {
            auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
            std::cout << "[db_manager] tokens rows:            " << m_res.GetValue(0, 0) << "\n";
        }
        res = con.Query("SELECT count(*) FROM token_occurrences;");
        if (!res->HasError()) {
            auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
            std::cout << "[db_manager] token_occurrences rows: " << m_res.GetValue(0, 0) << "\n";
        }
        res = con.Query("SELECT count(*) FROM files;");
        if (!res->HasError()) {
            auto& m_res = static_cast<duckdb::MaterializedQueryResult&>(*res);
            std::cout << "[db_manager] files rows:             " << m_res.GetValue(0, 0) << "\n";
        }
    }
}

#endif
