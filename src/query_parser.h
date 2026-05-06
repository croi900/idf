#ifndef IDF_QUERY_PARSER_H
#define IDF_QUERY_PARSER_H

#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <unordered_map>

#include "src/config.hpp"
#include "src/db_manager.hpp"
#include "src/dirtree.hpp"
#include "src/parser.h"

namespace idf::qp {

using path_filter   = std::function<bool(const std::string& path)>;
using record_filter = std::function<bool(const idf::file_record& rec)>;
using result_filter = std::function<bool(const std::string& path, const std::string& snippet)>;

struct compiled_query {
    std::vector<std::string> content_words;
    std::vector<std::string> type_prefixes;
    std::vector<std::string> highlight_words;

    std::vector<path_filter>   path_filters;
    std::vector<record_filter> record_filters;
    std::vector<result_filter> result_filters;

    bool        is_phrase = true;
    std::string raw;

    bool passes_path(const std::string& path) const {
        for (const auto& f : path_filters) if (!f(path)) return false;
        return true;
    }
    bool passes_record(const idf::file_record& rec) const {
        for (const auto& f : record_filters) if (!f(rec)) return false;
        return true;
    }
    bool passes_result(const std::string& path, const std::string& snip) const {
        for (const auto& f : result_filters) if (!f(path, snip)) return false;
        return true;
    }

    bool empty() const { return content_words.empty() && type_prefixes.empty(); }
};

inline compiled_query compile(const std::string& raw, bool is_phrase = true) {
    compiled_query cq;
    cq.raw       = raw;
    cq.is_phrase = is_phrase;

    static std::vector<std::string> def_node_types;
    static std::vector<std::string> ref_node_types;
    static std::vector<std::string> call_node_types;
    static std::vector<std::string> import_node_types;

    static bool initialized = false;
    if (!initialized) {
        auto all_nodes = idf::parser::get_all_ts_node_names();
        for (const auto& node : all_nodes) {
            if (node.find("def") != std::string::npos || 
                node.find("decl") != std::string::npos ||
                node.find("class") != std::string::npos ||
                node.find("struct") != std::string::npos ||
                node.find("enum") != std::string::npos) {
                def_node_types.push_back(node);
            }
            if (node.find("ident") != std::string::npos || 
                node.find("type_ident") != std::string::npos) {
                ref_node_types.push_back(node);
            }
            if (node.find("call") != std::string::npos) {
                call_node_types.push_back(node);
            }
            if (node.find("import") != std::string::npos || 
                node.find("include") != std::string::npos ||
                node.find("use") != std::string::npos) {
                import_node_types.push_back(node);
            }
        }
        initialized = true;
    }

    static const std::unordered_map<std::string, std::vector<std::string>> lang_extensions = {
        {"cpp",    {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx", ".h"}},
        {"c",      {".c", ".h"}},
        {"python", {".py", ".pyi"}},
        {"rust",   {".rs"}},
        {"js",     {".js", ".jsx"}},
        {"lua",    {".lua"}},
        {"bash",   {".sh", ".bash"}},
        {"markdown", {".md", ".markdown"}},
        {"vim",    {".vim"}},
    };

    std::istringstream ss(raw);
    std::string token;
    while (ss >> token) {
        auto colon = token.find(':');
        if (colon == std::string::npos) {
            cq.content_words.push_back(token);
            cq.highlight_words.push_back(token);
            continue;
        }

        std::string qualifier = token.substr(0, colon);
        std::string value     = token.substr(colon + 1);
        if (value.empty()) { 
            cq.content_words.push_back(token); 
            cq.highlight_words.push_back(token);
            continue; 
        }

        if (qualifier == "content") {
            cq.content_words.push_back(value);
            cq.highlight_words.push_back(value);

        } else if (qualifier == "path") {
            cq.path_filters.push_back([value](const std::string& p) {
                return p.find(value) != std::string::npos;
            });

        } else if (qualifier == "lang") {
            auto it = lang_extensions.find(value);
            if (it != lang_extensions.end()) {
                std::vector<std::string> exts = it->second;
                cq.path_filters.push_back([exts](const std::string& p) {
                    auto ext = std::filesystem::path(p).extension().string();
                    return std::find(exts.begin(), exts.end(), ext) != exts.end();
                });
            }
            std::string v = value;
            cq.record_filters.push_back([v](const idf::file_record& rec) {
                return std::strncmp(rec.lang, v.c_str(), sizeof(rec.lang)) == 0;
            });

        } else if (qualifier == "type") {
            cq.type_prefixes.push_back(value);
            cq.highlight_words.push_back(value);

        } else if (qualifier == "def") {
            for (const auto& nt : def_node_types) cq.type_prefixes.push_back(nt);
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else if (qualifier == "ref") {
            for (const auto& nt : ref_node_types) cq.type_prefixes.push_back(nt);
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else if (qualifier == "call") {
            for (const auto& nt : call_node_types) cq.type_prefixes.push_back(nt);
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else if (qualifier == "import") {
            for (const auto& nt : import_node_types) cq.type_prefixes.push_back(nt);
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else if (qualifier == "comment") {
            cq.type_prefixes.push_back("comment");
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else if (qualifier == "symbol") {
            for (const auto& nt : def_node_types) cq.type_prefixes.push_back(nt);
            if (!value.empty()) {
                std::string v = value;
                cq.highlight_words.push_back(v);
                cq.result_filters.push_back([v](const std::string&, const std::string& snip) {
                    return snip.find(v) != std::string::npos;
                });
            }

        } else {
            cq.content_words.push_back(token);
            cq.highlight_words.push_back(token);
        }
    }

    return cq;
}

inline float score_path(const dirtree::file_entry& f, const std::string& lang) {
    auto& p = f.path;

    size_t depth = std::count(p.begin(), p.end(), '/');
    float depth_score = 1.0f / static_cast<float>(depth + 1);

    float ext_score = 0.3f;
    auto ext = std::filesystem::path(p).extension().string();
    if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".c" ||
        ext == ".py"  || ext == ".rs" || ext == ".js"  || ext == ".ts")
        ext_score = 1.0f;
    else if (ext == ".md" || ext == ".txt" || ext == ".yaml" || ext == ".json")
        ext_score = 0.6f;

    float test_penalty = 1.0f;
    if (p.find("/test") != std::string::npos || p.find("/tests") != std::string::npos ||
        p.find("_test.") != std::string::npos || p.find("test_") != std::string::npos)
        test_penalty = 0.5f;

    constexpr float LOG_MAX_SIZE = 23.0f;
    float size_score = 1.0f;
    if (f.size > 0) {
        float log_sz = std::log(static_cast<float>(f.size + 1));
        float log_ideal = std::log(4096.0f);
        size_score = 1.0f - std::abs(log_sz - log_ideal) / LOG_MAX_SIZE;
        size_score = std::max(0.0f, size_score);
    }

    float path_len_score = std::max(0.0f, 1.0f - static_cast<float>(p.length()) / 200.0f);

    return (path_len_score * 0.5f + depth_score * 0.2f + ext_score * 0.2f + size_score * 0.1f) * test_penalty;
}

struct scored_result {
    std::string path;
    std::string snippet;
    float       score;
    uint64_t    atime;
};

inline auto make_comparator(idf::rank_strategy s)
    -> std::function<bool(const scored_result&, const scored_result&)>
{
    switch (s) {
        case idf::rank_strategy::Alphabetical:
            return [](const scored_result& a, const scored_result& b) {
                return a.path < b.path;
            };
        case idf::rank_strategy::DateModified:
            return [](const scored_result& a, const scored_result& b) {
                return a.atime > b.atime;
            };
        case idf::rank_strategy::Score:
        default:
            return [](const scored_result& a, const scored_result& b) {
                return a.score > b.score;
            };
    }
}

}

#endif
