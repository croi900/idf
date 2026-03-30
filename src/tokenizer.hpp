#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <vector>
#include "token_types.hpp"
#include "simd_tok.h"

namespace idf {
    inline std::vector<TokenInfo> tokenize_chunk(std::string_view content, uint64_t base_offset) {
        std::vector<TokenInfo> tokens;
        tokens.reserve(content.size() / 8); 
        tokenize_simd_avx2(content, base_offset, tokens);
        return tokens;
    }
}

#endif
