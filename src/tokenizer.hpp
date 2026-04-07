#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <vector>
#include "token_types.hpp"
#include "simd_tok.h"
#include <stringzilla/stringzilla.hpp>

namespace idf {

    inline std::vector<TokenInfo> tokenize_chunk(std::string_view content, uint64_t base_offset) {
        namespace sz = ashvardanian::stringzilla;
        sz::string_view view = content;
        std::vector<TokenInfo> tokens;

        tokens.reserve(content.size() / 6);

        for (auto word : view.split(sz::byteset({' ','\n','\r','\f','\v'}))) {
            if (word.empty()) continue;

            size_t offset = word.data() - content.data();
            tokens.push_back({base_offset + offset, word});
        }

        return tokens;
    }
}
#endif
