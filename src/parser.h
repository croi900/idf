#ifndef IDF_PARSER_H
#define IDF_PARSER_H

#include <memory>
#include <string_view>
#include <vector>
#include <map>
#include <tree_sitter/api.h>

#include "src/config.hpp"

extern "C" const TSLanguage* tree_sitter_bash();
extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();
extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_lua();
extern "C" const TSLanguage* tree_sitter_markdown();
extern "C" const TSLanguage* tree_sitter_python();
extern "C" const TSLanguage* tree_sitter_query();
extern "C" const TSLanguage* tree_sitter_rust();
extern "C" const TSLanguage* tree_sitter_vim();
extern "C" const TSLanguage* tree_sitter_vimdoc();

namespace idf::parser {

    inline std::vector<std::string> get_all_ts_node_names() {
        std::vector<std::string> names;
        const TSLanguage* langs[] = {
            tree_sitter_bash(), tree_sitter_c(), tree_sitter_cpp(),
            tree_sitter_javascript(), tree_sitter_lua(), tree_sitter_markdown(),
            tree_sitter_python(), tree_sitter_query(), tree_sitter_rust(),
            tree_sitter_vim(), tree_sitter_vimdoc()
        };
        for (auto l : langs) {
            uint32_t count = ts_language_symbol_count(l);
            for (uint32_t i = 0; i < count; ++i) {
                const char* name = ts_language_symbol_name(l, (TSSymbol)i);
                if (name && name[0] != '_') names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    struct symbol_info {
        uint32_t type;
        uint32_t start_byte;
        uint32_t end_byte;
    };

    struct ts_parser_deleter {
        void operator()(TSParser *p) const { ts_parser_delete(p); }
    };

    struct ts_tree_deleter {
        void operator()(TSTree *t) const { ts_tree_delete(t); }
    };

    using parser_ptr = std::unique_ptr<TSParser, ts_parser_deleter>;
    using tree_ptr   = std::unique_ptr<TSTree,   ts_tree_deleter>;

    inline bool is_indexable_node(std::string_view name) {
        if (name.empty() || name[0] == '_') return false;
        
        // Explicitly blacklist junk node types
        static const std::vector<std::string_view> blacklist = {
            "argument_list", "parameter_list", "field_list", "block", 
            "compound_statement", "expression_statement", "parenthesized_expression",
            "condition", "consequence", "alternative", "update", "body",
            "pair", "array", "object", "attribute", "element",
            "comma", "semicolon", "dot", "colon", "open_paren", "close_paren",
            "open_brace", "close_brace", "open_bracket", "close_bracket"
        };

        for (const auto& item : blacklist) {
            if (name == item) return false;
        }

        // Catch common punctuation/operators that might be named
        if (name.size() <= 2) {
            char c = name[0];
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' ||
                c == ',' || c == ';' || c == '.' || c == ':' || c == '?' || c == '!' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' ||
                c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '~') {
                return false;
            }
        }

        return true;
    }

    class parser {
    public:
        explicit parser(const TSLanguage *language)
            : parser_obj(ts_parser_new()) {
            ts_parser_set_language(parser_obj.get(), language);
            initialize_global_symbol_map(language);
        }

        tree_ptr parse(std::string_view source) {
            TSTree *tree = ts_parser_parse_string(
                parser_obj.get(),
                nullptr,
                source.data(),
                static_cast<uint32_t>(source.size())
            );
            root   = ts_tree_root_node(tree);
            cursor = ts_tree_cursor_new(root);
            return tree_ptr(tree);
        }

        void flatten_tree_dfs(std::vector<symbol_info>& flat_list, size_t max_nodes = 0) {
            if (max_nodes == 0) max_nodes = idf::config::max_ts_nodes_per_file;
            if (ts_node_is_null(root)) return;
            flat_list.clear();
            ts_tree_cursor_reset(&cursor, root);

            while (flat_list.size() < max_nodes) {
                TSNode node = ts_tree_cursor_current_node(&cursor);
                uint32_t symbol_id = ts_node_symbol(node);

                std::string_view type_name;
                if (symbol_id < global_symbol_map.size())
                    type_name = global_symbol_map[symbol_id];

                if (is_indexable_node(type_name)) {
                    flat_list.push_back({symbol_id,
                                         ts_node_start_byte(node),
                                         ts_node_end_byte(node)});
                }

                if (ts_tree_cursor_goto_first_child(&cursor))   continue;
                if (ts_tree_cursor_goto_next_sibling(&cursor))  continue;

                bool found = false;
                while (ts_tree_cursor_goto_parent(&cursor)) {
                    if (ts_tree_cursor_goto_next_sibling(&cursor)) { found = true; break; }
                }
                if (!found) break;
            }
        }

        std::string_view symbol_name(uint32_t id) {
            if (id < global_symbol_map.size()) return global_symbol_map[id];
            return "ERROR";
        }

    private:
        std::vector<std::string> global_symbol_map;
        TSTreeCursor cursor;
        TSNode root;

        void initialize_global_symbol_map(const TSLanguage *lang) {
            uint32_t count = ts_language_symbol_count(lang);
            global_symbol_map.resize(count);
            for (uint32_t i = 0; i < count; ++i) {
                const char *name = ts_language_symbol_name(lang, (TSSymbol)i);
                if (name) global_symbol_map[i] = name;
            }
        }

        parser_ptr parser_obj;
    };

    class parser_pool {
    public:
        static parser& get_instance(const TSLanguage* lang) {
            static thread_local std::map<const TSLanguage*, idf::parser::parser> local_parsers;
            auto it = local_parsers.find(lang);
            if (it == local_parsers.end())
                it = local_parsers.emplace(lang, idf::parser::parser(lang)).first;
            return it->second;
        }
    };
}

#endif //IDF_PARSER_H
