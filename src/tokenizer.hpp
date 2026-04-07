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
        tokens.reserve(content.size() / 8);
        sz::string_view delimiters = " \t\n\r\f\v";
        size_t start = 0;
        while (start < view.size()) {
            size_t end = view.find_first_of(delimiters, start);

            if (end == std::string_view::npos) {
                tokens.push_back({start, view.substr(start)});
                break;
            }

            if (end > start) {
                tokens.push_back({start, view.substr(start, end - start)});
            }

            start = end + 1;
        }


        return tokens;
    }
}
#endif
