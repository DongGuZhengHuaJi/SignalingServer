#pragma once

#include <hiredis/hiredis.h>
#include <sw/redis++/redis++.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

class RedisManager {
public:
    static RedisManager& getInstance() {
        static RedisManager _instance;
        return _instance;
    }

    sw::redis::Redis& getClient() {
        return _redis;
    }

    bool set(const std::string &key, const std::string &value, int expire_seconds = -1) {
        try {
            _redis.set(key, value);
            if (expire_seconds > 0) {
                _redis.expire(key, expire_seconds);
            }
            return true;
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis set error: " << err.what() << std::endl;
            return false;
        }

    }

    bool del(const std::string &key) {
        try {
            _redis.del(key);
            return true;
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis del error: " << err.what() << std::endl;
            return false;
        }
    }

    bool get(const std::string &key, std::string &value) {
        try {
            auto val = _redis.get(key);
            if (val) {
                value = *val;
                return true;
            }
            return false;
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis get error: " << err.what() << std::endl;
            return false;
        }
    }

    bool exists(const std::string &key) {
        try {
            return _redis.exists(key);
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis exists error: " << err.what() << std::endl;
            return false;
        }
    }

private:
    RedisManager() : _redis(create_options(), create_pool_options()) {}
    ~RedisManager() = default;
    RedisManager(const RedisManager&) = delete;
    RedisManager& operator=(const RedisManager&) = delete;

    private:
    sw::redis::Redis _redis;

    static std::string env_or_default(const char* key, const std::string& default_value) {
        const char* value = std::getenv(key);
        if (value == nullptr || value[0] == '\0') {
            return default_value;
        }
        return value;
    }

    static int env_int_or_default(const char* key, int default_value) {
        const char* value = std::getenv(key);
        if (value == nullptr || value[0] == '\0') {
            return default_value;
        }
        try {
            return std::stoi(value);
        } catch (...) {
            return default_value;
        }
    }

    // 辅助函数：配置基础连接
    static sw::redis::ConnectionOptions create_options() {
        sw::redis::ConnectionOptions opts;
        opts.host = env_or_default("REDIS_HOST", "127.0.0.1");
        opts.port = env_int_or_default("REDIS_PORT", 6379);

        const std::string redis_password = env_or_default("REDIS_PASSWORD", "");
        if (!redis_password.empty()) {
            opts.password = redis_password;
        }

        opts.db = env_int_or_default("REDIS_DB", 0);
        return opts;
    }

    // 辅助函数：配置连接池
    static sw::redis::ConnectionPoolOptions create_pool_options() {
        sw::redis::ConnectionPoolOptions pool_opts;
        // 使用硬件并发数作为连接池大小
        pool_opts.size = std::thread::hardware_concurrency(); 
        return pool_opts;
    }
};