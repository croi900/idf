//
// Created by croi on 10.04.2026.
//

#ifndef IDF_PARSER_H
#define IDF_PARSER_H

#include <memory>
#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_c();
extern "C" const TSLanguage* tree_sitter_cpp();



namespace idf::parser {

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
    using tree_ptr = std::unique_ptr<TSTree, ts_tree_deleter>;

    class parser {
    public:
        explicit parser(const TSLanguage *language)
            : parser_obj(ts_parser_new()) {
            ts_parser_set_language(parser_obj.get(), language);
            initialize_global_symbol_map(language);
        }

        [[nodiscard]]
        tree_ptr parse(std::string_view source) {
            TSTree *tree = ts_parser_parse_string(
                parser_obj.get(),
                nullptr,
                source.data(),
                static_cast<uint32_t>(source.size())
            );
            root = ts_tree_root_node(tree);
            cursor = ts_tree_cursor_new(root);
            return tree_ptr(tree);
        }


        [[nodiscard]]
        void flatten_tree_dfs( std::vector<symbol_info> & flat_list) {
            if (ts_node_is_null(root)) return;
            flat_list.clear();
            ts_tree_cursor_reset(&cursor, root);

            while (true) {
                TSNode node = ts_tree_cursor_current_node(&cursor);

                uint32_t symbol_id = ts_node_symbol(node);

                flat_list.push_back({
                    symbol_id,
                    ts_node_start_byte(node),
                    ts_node_end_byte(node)
                });
                if (ts_tree_cursor_goto_first_child(&cursor)) {
                    continue;
                }

                if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                    continue;
                }

                bool found_sibling = false;
                while (ts_tree_cursor_goto_parent(&cursor)) {
                    if (ts_tree_cursor_goto_next_sibling(&cursor)) {
                        found_sibling = true;
                        break;
                    }
                }

                if (!found_sibling) break;
            }

        }

        std::string_view symbol_name(uint32_t id) {
            return global_symbol_map[id];
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
                if (name) {
                    global_symbol_map[i] = name;
                }
            }
        }

        parser_ptr parser_obj;
    };
}

#endif //IDF_PARSER_H
