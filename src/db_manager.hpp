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
#include "sharding.hpp"

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


    inline void delete_shards(const ShardManager &mgr) {
        std::error_code ec;
        size_t removed = 0;
        for (uint32_t i = 0; i < mgr.num_shards; ++i) {
            fs::path p = fs::path("shards") / (mgr.prefix + "_" + std::to_string(i) + ".bin");
            if (fs::remove(p, ec)) { ++removed; }
        }

        fs::remove("shards", ec);
        std::cout << "[db_manager] Deleted " << removed << " shard files\n";
    }


    inline void load_into_duckdb(
        duckdb::DuckDB &db,
        const im_shard_map &tokens,
        const std::vector<std::string> &file_list) {
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
                path VARCHAR NOT NULL
            );
        )");

        run("CREATE INDEX IF NOT EXISTS idx_tokens_token ON tokens(token);");
        run("CREATE INDEX IF NOT EXISTS idx_occ_token_id ON token_occurrences(token_id);");
        run("CREATE INDEX IF NOT EXISTS idx_occ_file_hash ON token_occurrences(file_hash);");


        duckdb::Appender app_tokens(con, "tokens");
        duckdb::Appender app_occ(con, "token_occurrences");

        uint64_t token_id = 0;
        for (const auto &[word, occurrences]: tokens) {
            app_tokens.BeginRow();
            app_tokens.Append((uint64_t) token_id);
            app_tokens.Append(duckdb::Value::BLOB((const uint8_t *) word.data(), word.size()));
            app_tokens.EndRow();


            for (const auto &[file_hash, position]: occurrences) {
                app_occ.BeginRow();
                app_occ.Append((uint64_t) token_id);
                app_occ.Append((uint64_t) position);
                app_occ.Append((uint64_t) file_hash);
                app_occ.EndRow();
            }
            ++token_id;
        }

        app_tokens.Flush();
        app_tokens.Close();
        app_occ.Flush();
        app_occ.Close();

        duckdb::Appender app_files(con, "files");
        for (const auto &path: file_list) {
            app_files.BeginRow();
            app_files.Append((uint64_t) absl::HashOf(path));
            app_files.Append(duckdb::Value(path));
            app_files.EndRow();
        }
        app_files.Flush();
        app_files.Close();


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
