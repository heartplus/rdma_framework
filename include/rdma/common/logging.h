#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace rdma {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

class Logger {
public:
    static void SetLevel(LogLevel level) { level_ = level; }
    static LogLevel GetLevel() { return level_; }

    static void Debug(const char* file, int line, const char* fmt, ...) {
#ifdef RDMA_ENABLE_LOGGING
        if (level_ <= LogLevel::DEBUG) {
            va_list args;
            va_start(args, fmt);
            Log("DEBUG", file, line, fmt, args);
            va_end(args);
        }
#else
        (void)file;
        (void)line;
        (void)fmt;
#endif
    }

    static void Info(const char* file, int line, const char* fmt, ...) {
#ifdef RDMA_ENABLE_LOGGING
        if (level_ <= LogLevel::INFO) {
            va_list args;
            va_start(args, fmt);
            Log("INFO", file, line, fmt, args);
            va_end(args);
        }
#else
        (void)file;
        (void)line;
        (void)fmt;
#endif
    }

    static void Warn(const char* file, int line, const char* fmt, ...) {
#ifdef RDMA_ENABLE_LOGGING
        if (level_ <= LogLevel::WARN) {
            va_list args;
            va_start(args, fmt);
            Log("WARN", file, line, fmt, args);
            va_end(args);
        }
#else
        (void)file;
        (void)line;
        (void)fmt;
#endif
    }

    static void Error(const char* file, int line, const char* fmt, ...) {
        // Error logs are always enabled
        va_list args;
        va_start(args, fmt);
        Log("ERROR", file, line, fmt, args);
        va_end(args);
    }

private:
    static void Log(const char* level, const char* file, int line, const char* fmt, va_list args) {
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);

        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

        fprintf(stderr, "[%s] [%s] %s:%d: ", time_str, level, file, line);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    }

    static inline LogLevel level_ = LogLevel::INFO;
};

}  // namespace rdma

// Logging macros
#ifdef RDMA_ENABLE_LOGGING
#define RDMA_LOG_DEBUG(fmt, ...) \
    rdma::Logger::Debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RDMA_LOG_INFO(fmt, ...) \
    rdma::Logger::Info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define RDMA_LOG_WARN(fmt, ...) \
    rdma::Logger::Warn(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define RDMA_LOG_DEBUG(fmt, ...) ((void)0)
#define RDMA_LOG_INFO(fmt, ...) ((void)0)
#define RDMA_LOG_WARN(fmt, ...) ((void)0)
#endif

// Error logs are always enabled
#define RDMA_LOG_ERROR(fmt, ...) \
    rdma::Logger::Error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
