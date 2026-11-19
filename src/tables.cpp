#include "internal.hpp"

namespace fh::detail {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool iequals_ascii_prefix(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    return iequals(value.substr(0, prefix.size()), prefix);
}

std::string to_lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) c = ascii_lower(c);
    return result;
}

std::string_view trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && is_space(value[begin])) begin++;
    while (end > begin && is_space(value[end - 1])) end--;
    return value.substr(begin, end - begin);
}

std::string_view trim_end(std::string_view value) {
    std::size_t end = value.size();
    while (end > 0 && is_space(value[end - 1])) end--;
    return value.substr(0, end);
}

std::string_view trim_start(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) begin++;
    return value.substr(begin);
}

std::size_t ifind(std::string_view haystack, std::string_view needle, std::size_t from) {
    if (needle.empty()) return from <= haystack.size() ? from : std::string_view::npos;
    if (from > haystack.size() || needle.size() > haystack.size() - from) return std::string_view::npos;

    const char first = ascii_lower(needle[0]);
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t i = from; i <= last; i++) {
        if (ascii_lower(haystack[i]) != first) continue;
        std::size_t k = 1;
        while (k < needle.size() && ascii_lower(haystack[i + k]) == ascii_lower(needle[k])) k++;
        if (k == needle.size()) return i;
    }
    return std::string_view::npos;
}

std::size_t ifind_char(std::string_view haystack, char needle, std::size_t from) {
    const char lower = ascii_lower(needle);
    for (std::size_t i = from; i < haystack.size(); i++) {
        if (ascii_lower(haystack[i]) == lower) return i;
    }
    return std::string_view::npos;
}

namespace {

struct CiHash {
    std::size_t operator()(std::string_view value) const noexcept {
        std::size_t hash = 1469598103934665603ull;
        for (char c : value) {
            hash ^= std::size_t(static_cast<unsigned char>(ascii_lower(c)));
            hash *= 1099511628211ull;
        }
        return hash;
    }
};

struct CiEqual {
    bool operator()(std::string_view a, std::string_view b) const noexcept { return iequals(a, b); }
};

using NameMap = std::unordered_map<std::string_view, int, CiHash, CiEqual>;

const NameMap& tag_map() {
    static const NameMap map = [] {
        NameMap result;
        result.reserve(kTagCount * 2);
        for (int i = 0; i < kTagCount; i++) result.emplace(kTagNames[i], i);
        return result;
    }();
    return map;
}

const NameMap& attr_map() {
    static const NameMap map = [] {
        NameMap result;
        result.reserve(kAttrCount * 2);
        for (int i = 0; i < kAttrCount; i++) result.emplace(kAttrNames[i], i);
        return result;
    }();
    return map;
}

}  // namespace

int lookup_tag_name(std::string_view name) {
    const auto& map = tag_map();
    auto it = map.find(name);
    return it == map.end() ? -1 : it->second;
}

int lookup_attr_name(std::string_view name) {
    const auto& map = attr_map();
    auto it = map.find(name);
    return it == map.end() ? -1 : it->second;
}

}  // namespace fh::detail
