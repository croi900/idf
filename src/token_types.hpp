#ifndef TOKEN_TYPES_HPP
#define TOKEN_TYPES_HPP

#include <cstdint>
#include <string_view>

namespace idf {
    struct TokenInfo {
        uint64_t pos;
        std::string_view word;
    };
}

#endif
