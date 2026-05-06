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
    MDB_env* env;
public:
    lmdb_env() : env(nullptr) {
        check_lmdb(mdb_env_create(&env), "Failed to create LMDB env");
        check_lmdb(mdb_env_set_maxdbs(env, 10), "Failed to set max DBs");
        check_lmdb(mdb_env_set_mapsize(env, 1048576ULL * 1024ULL * 50ULL), "Failed to set map size (50GB)");
    }

    ~lmdb_env() {
        if (env) mdb_env_close(env);
    }

    void open(const std::string& path, unsigned int flags = 0, mdb_mode_t mode = 0664) {
        check_lmdb(mdb_env_open(env, path.c_str(), flags, mode), "Failed to open LMDB env at " + path);
    }

    MDB_env* get() const { return env; }
};

class lmdb_txn {
    MDB_txn* txn;
    bool committed;
public:
    lmdb_txn(lmdb_env& env, unsigned int flags = 0, MDB_txn* parent = nullptr) : txn(nullptr), committed(false) {
        check_lmdb(mdb_txn_begin(env.get(), parent, flags, &txn), "Failed to begin transaction");
    }

    ~lmdb_txn() {
        if (!committed && txn) {
            mdb_txn_abort(txn);
        }
    }

    void commit() {
        if (!committed) {
            check_lmdb(mdb_txn_commit(txn), "Failed to commit transaction");
            committed = true;
        }
    }

    void abort() {
        if (!committed && txn) {
            mdb_txn_abort(txn);
            committed = true;
        }
    }

    MDB_txn* get() const { return txn; }
};

class lmdb_dbi {
    MDB_dbi dbi;
public:
    lmdb_dbi() : dbi(0) {}
    
    void open(lmdb_txn& txn, const std::string& name, unsigned int flags = 0) {
        check_lmdb(mdb_dbi_open(txn.get(), name.c_str(), flags, &dbi), "Failed to open DBI: " + name);
    }

    MDB_dbi get() const { return dbi; }

    void put(lmdb_txn& txn, const std::string_view& key, const std::string_view& value, unsigned int flags = 0) {
        MDB_val k{key.size(), const_cast<char*>(key.data())};
        MDB_val v{value.size(), const_cast<char*>(value.data())};
        int rc = mdb_put(txn.get(), dbi, &k, &v, flags);
        if (rc != MDB_SUCCESS && rc != MDB_KEYEXIST) check_lmdb(rc, "Failed to put key");
    }

    void put(lmdb_txn& txn, const MDB_val& key, const MDB_val& value, unsigned int flags = 0) {
        int rc = mdb_put(txn.get(), dbi, const_cast<MDB_val*>(&key), const_cast<MDB_val*>(&value), flags);
        if (rc != MDB_SUCCESS && rc != MDB_KEYEXIST) check_lmdb(rc, "Failed to put key-value");
    }

    bool get(lmdb_txn& txn, const MDB_val& key, MDB_val& value) {
        int rc = mdb_get(txn.get(), dbi, const_cast<MDB_val*>(&key), &value);
        if (rc == MDB_NOTFOUND) return false;
        check_lmdb(rc, "Failed to get key");
        return true;
    }

    void del(lmdb_txn& txn, const MDB_val& key, MDB_val* value = nullptr) {
        int rc = mdb_del(txn.get(), dbi, const_cast<MDB_val*>(&key), value);
        if (rc != MDB_NOTFOUND) check_lmdb(rc, "Failed to delete key");
    }
};

class lmdb_cursor {
    MDB_cursor* cursor;
public:
    lmdb_cursor(lmdb_txn& txn, lmdb_dbi& dbi) : cursor(nullptr) {
        check_lmdb(mdb_cursor_open(txn.get(), dbi.get(), &cursor), "Failed to open cursor");
    }

    ~lmdb_cursor() {
        if (cursor) mdb_cursor_close(cursor);
    }

    bool get(MDB_val& key, MDB_val& data, MDB_cursor_op op) {
        int rc = mdb_cursor_get(cursor, &key, &data, op);
        if (rc == MDB_NOTFOUND) return false;
        check_lmdb(rc, "Cursor get failed");
        return true;
    }

    void put(MDB_val& key, MDB_val& data, unsigned int flags = 0) {
        int rc = mdb_cursor_put(cursor, &key, &data, flags);
        if (rc != MDB_SUCCESS && rc != MDB_KEYEXIST) check_lmdb(rc, "Cursor put failed");
    }

    void del(unsigned int flags = 0) {
        check_lmdb(mdb_cursor_del(cursor, flags), "Cursor del failed");
    }

    MDB_cursor* get_raw() const { return cursor; }
};

} // namespace idf

#endif
