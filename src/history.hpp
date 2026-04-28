#ifndef IDF_HISTORY_HPP
#define IDF_HISTORY_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <algorithm>

#include "src/lmdb_wrapper.hpp"

namespace idf::history {

class history_store {
    lmdb_env env_;
    lmdb_dbi db_;
    bool    open_ = false;

public:
    void open(const std::string& path = "idf_history") {
        env_.open(path, 0, 0664);
        lmdb_txn txn(env_);
        db_.open(txn, "queries", MDB_CREATE | MDB_INTEGERKEY);
        txn.commit();
        open_ = true;
    }

    void record(const std::string& query) {
        if (!open_ || query.empty()) return;
        auto us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        try {
            lmdb_txn txn(env_);
            MDB_val k{sizeof(us), &us};
            MDB_val v{query.size(), const_cast<char*>(query.data())};
            check_lmdb(mdb_put(txn.get(), db_.get(), &k, &v, 0), "history record");
            txn.commit();
        } catch (...) {}
    }

    std::vector<std::string> recent(size_t n) const {
        std::vector<std::string> out;
        if (!open_) return out;
        try {
            lmdb_txn txn(const_cast<lmdb_env&>(env_), MDB_RDONLY);
            lmdb_cursor cursor(txn, const_cast<lmdb_dbi&>(db_));
            MDB_val k, v;
            if (cursor.get(k, v, MDB_LAST)) {
                do {
                    out.emplace_back(static_cast<const char*>(v.mv_data), v.mv_size);
                    if (out.size() >= n) break;
                } while (cursor.get(k, v, MDB_PREV));
            }
        } catch (...) {}
        return out;
    }

    std::unordered_map<std::string, uint32_t> frequency_map() const {
        std::unordered_map<std::string, uint32_t> freq;
        if (!open_) return freq;
        try {
            lmdb_txn txn(const_cast<lmdb_env&>(env_), MDB_RDONLY);
            lmdb_cursor cursor(txn, const_cast<lmdb_dbi&>(db_));
            MDB_val k, v;
            if (cursor.get(k, v, MDB_FIRST)) {
                do {
                    std::string q(static_cast<const char*>(v.mv_data), v.mv_size);
                    freq[q]++;
                } while (cursor.get(k, v, MDB_NEXT));
            }
        } catch (...) {}
        return freq;
    }

    bool is_open() const { return open_; }
};

class search_observer {
    history_store& store_;
public:
    explicit search_observer(history_store& s) : store_(s) {}
    void on_query_executed(const std::string& query) {
        store_.record(query);
    }
};

}

#endif
