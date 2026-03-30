#ifndef TOKENIZER_HPP
#define TOKENIZER_HPP

#include <vector>
#include "token_types.hpp"
#include "simd_tok.h"

namespace idf {
    inline std::vector<TokenInfo> tokenize_chunk(std::string_view content, uint64_t base_offset) {
        std::vector<TokenInfo> tokens;
        tokens.reserve(content.size() / 8);

#ifdef __AVX512BW__



        static const bool have_avx512bw = __builtin_cpu_supports("avx512bw");
        if (have_avx512bw) {
            tokenize_simd_avx512(content, base_offset, tokens);
        } else {
            tokenize_simd_avx2(content, base_offset, tokens);
        }
#else
        tokenize_simd_avx2(content, base_offset, tokens);
#endif

        return tokens;
    }
}

#endif
