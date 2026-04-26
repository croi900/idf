//
// Created by croi on 11.04.2026.
//

#ifndef IDF_PARSER_QUERIES_H
#define IDF_PARSER_QUERIES_H
#include <string>

namespace idf::queries::c {
    constexpr std::string_view func = "(function_definition declarator: [(function_declarator declarator: (identifier) @func)(field_identifier) @func])";
    constexpr std::string_view var = "(declaration declarator: (type_identifier) @var))";
    constexpr std::string_view structs = "(struct_specifier name: (type_identifier) @type)";
    constexpr std::string_view unions = "(union_specifier name: (type_identifier) @type)";
    constexpr std::string_view str_literal = "(string_literal) @str";
    constexpr std::string_view cmt = "(comment) @cmt";
}
#endif //IDF_PARSER_QUERIES_H
