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
#include <absl/hash/hash.h>
#include "tokens.pb.h"
#include "tokenizer.hpp"
#include <parallel_hashmap/phmap.h>

namespace idf {

    typedef absl::flat_hash_map<std::string, std::vector<std::pair<size_t, size_t>>> im_shard_map; /// stands for in mem comncat map

    namespace google_io = google::protobuf::io;

    class ShardManager {
        struct Shard {
            hpx::mutex mtx;
            int fd;
            std::unique_ptr<google_io::FileOutputStream> file_stream;

            std::unique_ptr<google_io::CodedOutputStream> coded_output;

            Shard() : fd(-1) {
            }

            ~Shard() {
                if (coded_output) coded_output.reset();
                if (file_stream) file_stream->Close();
                if (fd != -1) close(fd);
            }
        };


        std::vector<std::unique_ptr<Shard> > shards;

    public:
        std::string prefix;
        size_t num_shards;
        ShardManager(size_t n, const std::string &prefix) : num_shards(n) {
            std::filesystem::create_directories("shards");
            this->prefix = prefix;
            for (size_t i = 0; i < n; ++i) {
                auto shard = std::make_unique<Shard>();
                std::string filename = "shards/" + prefix + "_" + std::to_string(i) + ".bin";
                shard->fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (shard->fd != -1) {
                    shard->file_stream = std::make_unique<google_io::FileOutputStream>(shard->fd, 64 * 1024);
                    shard->coded_output = std::make_unique<google_io::CodedOutputStream>(shard->file_stream.get());
                }
                shards.push_back(std::move(shard));
            }
        }

        void write_tokens(uint32_t file_idx, const std::string &path, const std::vector<TokenInfo> &tokens) {
            if (tokens.empty()) return;


            size_t shard_idx = absl::HashOf(path) % num_shards;
            auto &shard = shards[shard_idx];


            static thread_local google::protobuf::Arena arena;
            arena.Reset();
            auto *file_tokens = google::protobuf::Arena::Create<idf::FileTokens>(&arena);

            file_tokens->set_path(path);
            file_tokens->set_file_idx(file_idx);

            for (const auto &t_info: tokens) {
                auto *t = file_tokens->add_tokens();
                t->set_pos(t_info.pos);
                t->set_word(t_info.word.data(), t_info.word.size());
            }

            size_t msg_size = file_tokens->ByteSizeLong();
            size_t varint_size = google_io::CodedOutputStream::VarintSize32((uint32_t) msg_size);

            static thread_local std::vector<uint8_t> serialization_buffer;
            serialization_buffer.resize(varint_size + msg_size);

            uint8_t *target = serialization_buffer.data();
            target = google_io::CodedOutputStream::WriteVarint32ToArray((uint32_t) msg_size, target);
            file_tokens->SerializeWithCachedSizesToArray(target);


            std::lock_guard<hpx::mutex> lock(shard->mtx);
            if (shard->coded_output) {
                shard->coded_output->WriteRaw(serialization_buffer.data(), (int) serialization_buffer.size());
            }
        }

        im_shard_map
        read_tokens(uint32_t shard_idx) {
            auto& shard = shards[shard_idx];
            std::string filename = "shards/" + prefix + "_" + std::to_string(shard_idx) + ".bin";
            int read_fd = open(filename.c_str(), O_RDONLY);
            if (read_fd == -1) return {};
            google::protobuf::io::FileInputStream file_input(read_fd);
            google::protobuf::io::CodedInputStream coded_input(&file_input);
            idf::FileTokens msg;
            uint32_t msg_size;

            im_shard_map unique;

            while (coded_input.ReadVarint32(&msg_size)) {
                auto lim = coded_input.PushLimit(msg_size);

                if (msg.ParseFromCodedStream(&coded_input)) {
                    auto hash = absl::HashOf(msg.path());
                    for (const Token & token : msg.tokens()) {
                        const auto& word = token.word();
                        auto pos = token.pos();
                        unique[word].emplace_back(hash, pos);
                    }
                }
                coded_input.PopLimit(lim);
            }

            return unique;
        }
    };
};

#endif
