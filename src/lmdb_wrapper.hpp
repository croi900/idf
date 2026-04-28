#pragma once
#ifndef LMDB_WRAPPER_HPP
#define LMDB_WRAPPER_HPP

#include <lmdb.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <iostream>

namespace idf {

class LMDBException : public std::runtime_error {
public:
    LMDBException(const std::string& msg, int err_code) 
        : std::runtime_error(msg + ": " + mdb_strerror(err_code)), code(err_code) {}
    int code;
};

inline void check_lmdb(int rc, const std::string& msg) {
    if (rc != MDB_SUCCESS) {
        throw LMDBException(msg, rc);
    }
}

class lmdb_env {
    MDB_env* env_;
public:
    lmdb_env() : env_(nullptr) {
        check_lmdb(mdb_env_create(&env_), "Failed to create LMDB env");
        check_lmdb(mdb_env_set_maxdbs(env_, 10), "Failed to set max DBs");
        check_lmdb(mdb_env_set_mapsize(env_, 1048576ULL * 1024ULL * 50ULL), "Failed to set map size (50GB)");
    }

    ~lmdb_env() {
        if (env_) mdb_env_close(env_);
    }

    void open(const std::string& path, unsigned int flags = 0, mdb_mode_t mode = 0664) {
        check_lmdb(mdb_env_open(env_, path.c_str(), flags, mode), "Failed to open LMDB env at " + path);
    }

    MDB_env* get() const { return env_; }
};

class lmdb_txn {
    MDB_txn* txn_;
    bool committed_;
public:
    lmdb_txn(lmdb_env& env, unsigned int flags = 0, MDB_txn* parent = nullptr) : txn_(nullptr), committed_(false) {
        check_lmdb(mdb_txn_begin(env.get(), parent, flags, &txn_), "Failed to begin transaction");
    }

    ~lmdb_txn() {
        if (!committed_ && txn_) {
            mdb_txn_abort(txn_);
        }
    }

    void commit() {
        if (!committed_) {
            check_lmdb(mdb_txn_commit(txn_), "Failed to commit transaction");
            committed_ = true;
        }
    }

    void abort() {
        if (!committed_ && txn_) {
            mdb_txn_abort(txn_);
            committed_ = true;
        }
    }

    MDB_txn* get() const { return txn_; }
};

class lmdb_dbi {
    MDB_dbi dbi_;
public:
    lmdb_dbi() : dbi_(0) {}
    
    void open(lmdb_txn& txn, const std::string& name, unsigned int flags = 0) {
        check_lmdb(mdb_dbi_open(txn.get(), name.c_str(), flags, &dbi_), "Failed to open DBI: " + name);
    }

    MDB_dbi get() const { return dbi_; }

    void put(lmdb_txn& txn, const std::string_view& key, const std::string_view& value, unsigned int flags = 0) {
        MDB_val k{key.size(), const_cast<char*>(key.data())};
        MDB_val v{value.size(), const_cast<char*>(value.data())};
        check_lmdb(mdb_put(txn.get(), dbi_, &k, &v, flags), "Failed to put key");
    }

    void put(lmdb_txn& txn, const MDB_val& key, const MDB_val& value, unsigned int flags = 0) {
        check_lmdb(mdb_put(txn.get(), dbi_, const_cast<MDB_val*>(&key), const_cast<MDB_val*>(&value), flags), "Failed to put key-value");
    }

    bool get(lmdb_txn& txn, const MDB_val& key, MDB_val& value) {
        int rc = mdb_get(txn.get(), dbi_, const_cast<MDB_val*>(&key), &value);
        if (rc == MDB_NOTFOUND) return false;
        check_lmdb(rc, "Failed to get key");
        return true;
    }

    void del(lmdb_txn& txn, const MDB_val& key, MDB_val* value = nullptr) {
        int rc = mdb_del(txn.get(), dbi_, const_cast<MDB_val*>(&key), value);
        if (rc != MDB_NOTFOUND) check_lmdb(rc, "Failed to delete key");
    }
};

class lmdb_cursor {
    MDB_cursor* cursor_;
public:
    lmdb_cursor(lmdb_txn& txn, lmdb_dbi& dbi) : cursor_(nullptr) {
        check_lmdb(mdb_cursor_open(txn.get(), dbi.get(), &cursor_), "Failed to open cursor");
    }

    ~lmdb_cursor() {
        if (cursor_) mdb_cursor_close(cursor_);
    }

    bool get(MDB_val& key, MDB_val& data, MDB_cursor_op op) {
        int rc = mdb_cursor_get(cursor_, &key, &data, op);
        if (rc == MDB_NOTFOUND) return false;
        check_lmdb(rc, "Cursor get failed");
        return true;
    }

    void put(MDB_val& key, MDB_val& data, unsigned int flags = 0) {
        check_lmdb(mdb_cursor_put(cursor_, &key, &data, flags), "Cursor put failed");
    }

    void del(unsigned int flags = 0) {
        check_lmdb(mdb_cursor_del(cursor_, flags), "Cursor del failed");
    }

    MDB_cursor* get_raw() const { return cursor_; }
};

} // namespace idf

#endif
