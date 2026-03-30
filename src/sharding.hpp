#ifndef SHARDING_HPP
#define SHARDING_HPP

#include <string>
#include <vector>
#include <hpx/mutex.hpp>
#include <memory>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/arena.h>
#include "tokens.pb.h"
#include "tokenizer.hpp"

namespace idf {
    namespace google_io = google::protobuf::io;

    class ShardManager {
        struct Shard {
            hpx::mutex mtx;
            int fd;
            std::unique_ptr<google_io::FileOutputStream> file_stream;

            Shard() : fd(-1) {}
            ~Shard() {
                if (file_stream) file_stream->Close();
                if (fd != -1) close(fd);
            }
        };

        std::vector<std::unique_ptr<Shard>> shards;
        size_t num_shards;

    public:
        ShardManager(size_t n, const std::string& prefix) : num_shards(n) {
            std::filesystem::create_directories("shards");
            for (size_t i = 0; i < n; ++i) {
                auto shard = std::make_unique<Shard>();
                std::string filename = "shards/" + prefix + "_" + std::to_string(i) + ".bin";
                shard->fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (shard->fd != -1) {
                    shard->file_stream = std::make_unique<google_io::FileOutputStream>(shard->fd, 64 * 1024);
                }
                shards.push_back(std::move(shard));
            }
        }

        void write_tokens(uint32_t file_idx, const std::string& path, const std::vector<TokenInfo>& tokens) {
            if (tokens.empty()) return;

            size_t h = std::hash<std::string>{}(path);
            size_t shard_idx = h % num_shards;
            auto& shard = shards[shard_idx];

            google::protobuf::Arena arena;
            auto* file_tokens = google::protobuf::Arena::Create<idf::FileTokens>(&arena);
            
            file_tokens->set_path(path);
            file_tokens->set_file_idx(file_idx);
            
            for (const auto& t_info : tokens) {
                auto* t = file_tokens->add_tokens();
                t->set_pos(t_info.pos);
                t->set_word(t_info.word.data(), t_info.word.size());
            }


            size_t msg_size = file_tokens->ByteSizeLong();
            size_t varint_size = google_io::CodedOutputStream::VarintSize32((uint32_t)msg_size);
            
            static thread_local std::vector<uint8_t> serialization_buffer;
            serialization_buffer.resize(varint_size + msg_size);
            
            uint8_t* target = serialization_buffer.data();
            target = google_io::CodedOutputStream::WriteVarint32ToArray((uint32_t)msg_size, target);
            file_tokens->SerializeWithCachedSizesToArray(target);


            std::lock_guard<hpx::mutex> lock(shard->mtx);
            if (shard->file_stream) {
                google_io::CodedOutputStream coded_output(shard->file_stream.get());
                coded_output.WriteRaw(serialization_buffer.data(), (int)serialization_buffer.size());
            }
        }
    };
}

#endif
