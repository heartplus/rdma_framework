#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace rdma {

// Error codes
enum class ErrorCode : uint8_t {
    OK = 0,
    INVALID_ARGUMENT = 1,
    NOT_FOUND = 2,
    ALREADY_EXISTS = 3,
    PERMISSION_DENIED = 4,
    RESOURCE_EXHAUSTED = 5,
    FAILED_PRECONDITION = 6,
    ABORTED = 7,
    OUT_OF_RANGE = 8,
    UNIMPLEMENTED = 9,
    INTERNAL = 10,
    UNAVAILABLE = 11,
    DATA_LOSS = 12,
    IO_ERROR = 13,
    TIMEOUT = 14,
    CANCELLED = 15,
    CONNECTION_REFUSED = 16,
    CONNECTION_RESET = 17,
};

// Status class for error handling
class Status {
public:
    // Constructors
    Status() : code_(ErrorCode::OK), errno_value_(0) {}

    explicit Status(ErrorCode code, const char* message = nullptr, int errno_val = 0)
            : code_(code), errno_value_(errno_val) {
        if (message) {
            message_ = std::make_unique<std::string>(message);
        }
    }

    Status(const Status& other)
            : code_(other.code_), errno_value_(other.errno_value_) {
        if (other.message_) {
            message_ = std::make_unique<std::string>(*other.message_);
        }
    }

    Status& operator=(const Status& other) {
        if (this != &other) {
            code_ = other.code_;
            errno_value_ = other.errno_value_;
            if (other.message_) {
                message_ = std::make_unique<std::string>(*other.message_);
            } else {
                message_.reset();
            }
        }
        return *this;
    }

    Status(Status&&) noexcept = default;
    Status& operator=(Status&&) noexcept = default;

    // Factory methods
    static Status OK() { return Status(); }

    static Status InvalidArgument(const char* msg = nullptr) {
        return Status(ErrorCode::INVALID_ARGUMENT, msg);
    }

    static Status NotFound(const char* msg = nullptr) {
        return Status(ErrorCode::NOT_FOUND, msg);
    }

    static Status ResourceExhausted(const char* msg = nullptr) {
        return Status(ErrorCode::RESOURCE_EXHAUSTED, msg);
    }

    static Status IOError(const char* msg = nullptr, int errno_val = 0) {
        return Status(ErrorCode::IO_ERROR, msg, errno_val);
    }

    static Status Timeout(const char* msg = nullptr) {
        return Status(ErrorCode::TIMEOUT, msg);
    }

    static Status Internal(const char* msg = nullptr) {
        return Status(ErrorCode::INTERNAL, msg);
    }

    static Status Unavailable(const char* msg = nullptr) {
        return Status(ErrorCode::UNAVAILABLE, msg);
    }

    static Status ConnectionRefused(const char* msg = nullptr) {
        return Status(ErrorCode::CONNECTION_REFUSED, msg);
    }

    static Status ConnectionReset(const char* msg = nullptr) {
        return Status(ErrorCode::CONNECTION_RESET, msg);
    }

    static Status Cancelled(const char* msg = nullptr) {
        return Status(ErrorCode::CANCELLED, msg);
    }

    // Accessors
    bool ok() const { return code_ == ErrorCode::OK; }
    ErrorCode code() const { return code_; }
    int errno_value() const { return errno_value_; }

    const char* message() const {
        return message_ ? message_->c_str() : "";
    }

    std::string ToString() const {
        if (ok()) return "OK";
        std::string result = CodeToString(code_);
        if (message_) {
            result += ": " + *message_;
        }
        if (errno_value_ != 0) {
            result += " (errno=" + std::to_string(errno_value_) + ": ";
            result += strerror(errno_value_);
            result += ")";
        }
        return result;
    }

private:
    static const char* CodeToString(ErrorCode code) {
        switch (code) {
            case ErrorCode::OK:
                return "OK";
            case ErrorCode::INVALID_ARGUMENT:
                return "Invalid argument";
            case ErrorCode::NOT_FOUND:
                return "Not found";
            case ErrorCode::ALREADY_EXISTS:
                return "Already exists";
            case ErrorCode::PERMISSION_DENIED:
                return "Permission denied";
            case ErrorCode::RESOURCE_EXHAUSTED:
                return "Resource exhausted";
            case ErrorCode::FAILED_PRECONDITION:
                return "Failed precondition";
            case ErrorCode::ABORTED:
                return "Aborted";
            case ErrorCode::OUT_OF_RANGE:
                return "Out of range";
            case ErrorCode::UNIMPLEMENTED:
                return "Unimplemented";
            case ErrorCode::INTERNAL:
                return "Internal error";
            case ErrorCode::UNAVAILABLE:
                return "Unavailable";
            case ErrorCode::DATA_LOSS:
                return "Data loss";
            case ErrorCode::IO_ERROR:
                return "I/O error";
            case ErrorCode::TIMEOUT:
                return "Timeout";
            case ErrorCode::CANCELLED:
                return "Cancelled";
            case ErrorCode::CONNECTION_REFUSED:
                return "Connection refused";
            case ErrorCode::CONNECTION_RESET:
                return "Connection reset";
            default:
                return "Unknown error";
        }
    }

    ErrorCode code_;
    int errno_value_;
    std::unique_ptr<std::string> message_;
};

// Result<T> for returning values with error handling
template <typename T>
class Result {
public:
    // Success constructors
    Result(T value) : value_(std::move(value)), status_(Status::OK()) {}

    // Error constructors
    Result(Status status) : status_(std::move(status)) {}

    // Move constructors
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    // Delete copy (to avoid unnecessary copies)
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    // Accessors
    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }

    T& value() & { return value_; }
    const T& value() const& { return value_; }
    T&& value() && { return std::move(value_); }

    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }

    T& operator*() & { return value_; }
    const T& operator*() const& { return value_; }
    T&& operator*() && { return std::move(value_); }

private:
    T value_;
    Status status_;
};

// Specialization for unique_ptr
template <typename T>
class Result<std::unique_ptr<T>> {
public:
    Result(std::unique_ptr<T> value) : value_(std::move(value)), status_(Status::OK()) {}

    Result(Status status) : status_(std::move(status)) {}

    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    bool ok() const { return status_.ok(); }
    const Status& status() const { return status_; }

    std::unique_ptr<T>& value() & { return value_; }
    const std::unique_ptr<T>& value() const& { return value_; }
    std::unique_ptr<T>&& value() && { return std::move(value_); }

    T* operator->() { return value_.get(); }
    const T* operator->() const { return value_.get(); }

    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }

private:
    std::unique_ptr<T> value_;
    Status status_;
};

}  // namespace rdma
