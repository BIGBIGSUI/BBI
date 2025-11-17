#pragma once

#include <string>
#include <string_view>

namespace sphaira::i18n {

inline bool init(long) {
    return true;
}

inline void exit() {
}

inline std::string get(std::string_view str) {
    return std::string(str);
}

} // namespace sphaira::i18n

inline namespace literals {

inline std::string operator""_i18n(const char* str, size_t len) {
    return std::string(str, len);
}

} // namespace literals
