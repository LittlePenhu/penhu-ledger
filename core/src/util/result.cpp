#include "penhu/util/result.hpp"

namespace penhu {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:               return "Ok";
        case ErrorCode::InvalidArgument:  return "InvalidArgument";
        case ErrorCode::NotFound:         return "NotFound";
        case ErrorCode::AlreadyExists:    return "AlreadyExists";
        case ErrorCode::AuthFailed:       return "AuthFailed";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::StorageFailure:   return "StorageFailure";
        case ErrorCode::CryptoFailure:    return "CryptoFailure";
        case ErrorCode::NetworkFailure:   return "NetworkFailure";
        case ErrorCode::LlmFailure:       return "LlmFailure";
        case ErrorCode::ConfigError:      return "ConfigError";
        case ErrorCode::Internal:         return "Internal";
    }
    return "Unknown";
}

std::string Error::to_string() const {
    std::string out;
    out.reserve(message.size() + context.size() + 24);
    out += '[';
    out += penhu::to_string(code);
    out += "] ";
    out += message;
    if (!context.empty()) {
        out += "  <";
        out += context;
        out += '>';
    }
    return out;
}

}  // namespace penhu
