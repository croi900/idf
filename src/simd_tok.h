#ifndef SIMD_TOK_H
#define SIMD_TOK_H

#include <immintrin.h>
#include <cstdint>
#include <string_view>
#include <vector>
#include "token_types.hpp"

namespace idf {

    inline void tokenize_simd_avx2(std::string_view content, uint64_t base_offset, std::vector<TokenInfo>& tokens) {
        const char* data = content.data();
        size_t size = content.size();
        size_t i = 0;

        __m256i space = _mm256_set1_epi8(' ');
        __m256i tab = _mm256_set1_epi8('\t');
        __m256i newline = _mm256_set1_epi8('\n');
        __m256i carriage = _mm256_set1_epi8('\r');

        size_t last_word_start = 0;
        bool in_word = false;

        for (; i + 31 < size; i += 32) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            __m256i mask_v = _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, space), _mm256_cmpeq_epi8(chunk, tab)),
                _mm256_or_si256(_mm256_cmpeq_epi8(chunk, newline), _mm256_cmpeq_epi8(chunk, carriage))
            );
            
            uint32_t mask = _mm256_movemask_epi8(mask_v);
            uint32_t current_bit = 0;

            while (current_bit < 32) {
                if (!in_word) {

                    uint32_t remaining = (~mask) >> current_bit;
                    if (remaining == 0) break;
                    uint32_t skip = __builtin_ctz(remaining);
                    current_bit += skip;
                    last_word_start = i + current_bit;
                    in_word = true;
                } else {

                    uint32_t remaining = mask >> current_bit;
                    if (remaining == 0) break;
                    uint32_t skip = __builtin_ctz(remaining);
                    current_bit += skip;
                    tokens.push_back({base_offset + last_word_start, content.substr(last_word_start, (i + current_bit) - last_word_start)});
                    in_word = false;
                }
            }
        }


        for (; i < size; ++i) {
            bool is_ws = (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r');
            if (!in_word && !is_ws) {
                last_word_start = i;
                in_word = true;
            } else if (in_word && is_ws) {
                tokens.push_back({base_offset + last_word_start, content.substr(last_word_start, i - last_word_start)});
                in_word = false;
            }
        }

        if (in_word) {
            tokens.push_back({base_offset + last_word_start, content.substr(last_word_start, size - last_word_start)});
        }
    }
}

#endif
