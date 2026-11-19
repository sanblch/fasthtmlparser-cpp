// HTML5 character reference decoder (WHATWG "character reference state"), a port of HtmlDecodeCore.
#include "internal.hpp"

#include <cstring>

namespace fh::detail {

namespace {

// The replacements for 0x80..0x9F, as UTF-32 code points; 0 = keep.
constexpr std::uint32_t kWindows1252CodePoints[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

int encode_utf8(std::uint32_t cp, char* out) {
    if (cp <= 0x7F) {
        out[0] = char(cp);
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = char(0xC0 | (cp >> 6));
        out[1] = char(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = char(0xE0 | (cp >> 12));
        out[1] = char(0x80 | ((cp >> 6) & 0x3F));
        out[2] = char(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = char(0xF0 | (cp >> 18));
    out[1] = char(0x80 | ((cp >> 12) & 0x3F));
    out[2] = char(0x80 | ((cp >> 6) & 0x3F));
    out[3] = char(0x80 | (cp & 0x3F));
    return 4;
}

// `s` starts with '#'. Returns the number of chars consumed (0 = not a reference).
int match_numeric(std::string_view s, std::uint32_t& code_point) {
    int i = 1;
    const bool hex = i < int(s.size()) && (ascii_lower(s[std::size_t(i)]) == 'x');
    if (hex) i++;

    const int digits_start = i;
    std::uint32_t value = 0;

    for (; i < int(s.size()); i++) {
        const char c = s[std::size_t(i)];
        std::uint32_t digit;

        if (c >= '0' && c <= '9') {
            digit = std::uint32_t(c - '0');
        } else if (hex && ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f') {
            digit = std::uint32_t(ascii_lower(c) - 'a' + 10);
        } else {
            break;
        }

        value = value * (hex ? 16u : 10u) + digit;
        if (value > 0x10FFFF) value = 0x110000;
    }

    if (i == digits_start) {
        code_point = 0;
        return 0;
    }

    if (i < int(s.size()) && s[std::size_t(i)] == ';') i++;

    if (value == 0 || value > 0x10FFFF || (value - 0xD800 <= 0x7FF)) {
        value = 0xFFFD;
    } else if (value - 0x80 <= 0x1F) {
        const std::uint32_t mapped = kWindows1252CodePoints[value - 0x80];
        if (mapped != 0) value = mapped;
    }

    code_point = value;
    return i;
}

// `s` is the text right after '&'. Returns the number of chars consumed (0 = not a reference).
int match_named(std::string_view s, bool in_attribute, std::string_view& value) {
    const std::size_t limit = std::min(s.size(), kMaxEntityNameLength + 1);
    std::size_t len = 0;
    while (len < limit && is_ascii_alnum(s[len])) len++;

    if (len >= 2) {
        // Full match: "&name;".
        if (len < s.size() && s[len] == ';') {
            if (const EntityEntry* entry = find_entity(s.substr(0, len))) {
                value = entry->value;
                return int(len + 1);
            }
        }

        // Longest legacy prefix without ';': "&copy2024" -> "(c)2024", "&notit;" -> "not it;".
        const std::size_t max_legacy = std::min(len, kMaxLegacyEntityNameLength);
        for (std::size_t k = max_legacy; k >= 2; k--) {
            if (const EntityEntry* entry = find_entity(s.substr(0, k)); entry && entry->legacy) {
                if (in_attribute && k < s.size() && (s[k] == '=' || is_ascii_alnum(s[k]))) break;
                value = entry->value;
                return int(k);
            }
        }
    }

    value = {};
    return 0;
}

}  // namespace

const EntityEntry* find_entity(std::string_view name) {
    // kEntities is sorted by name: binary search.
    std::size_t lo = 0;
    std::size_t hi = kEntityCount;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const int cmp = kEntities[mid].name.compare(name);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            return &kEntities[mid];
        }
    }
    return nullptr;
}

bool needs_decoding(std::string_view value) {
    return value.find_first_of(std::string_view("&\r", 2)) != std::string_view::npos;
}

std::string normalize_newlines(std::string_view value) {
    if (value.find('\r') == std::string_view::npos) return std::string(value);

    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); i++) {
        const char c = value[i];
        if (c != '\r') {
            result.push_back(c);
            continue;
        }
        result.push_back('\n');
        if (i + 1 < value.size() && value[i + 1] == '\n') i++;
    }
    return result;
}

int decode_reference(std::string_view after_amp, char out[4], int& written) {
    written = 0;

    if (!after_amp.empty() && after_amp[0] == '#') {
        std::uint32_t code_point = 0;
        const int consumed = match_numeric(after_amp, code_point);
        if (consumed > 0) written = encode_utf8(code_point, out);
        return consumed;
    }

    std::string_view value;
    const int consumed = match_named(after_amp, false, value);
    if (consumed > 0) {
        written = int(value.size());
        std::memcpy(out, value.data(), value.size());
    }
    return consumed;
}

std::string decode_html(std::string_view input, bool in_attribute) {
    // Line breaks first, as a browser reads the page.
    std::string normalized;
    if (input.find('\r') != std::string_view::npos) {
        normalized = normalize_newlines(input);
        input = normalized;
    }

    std::size_t amp = input.find('&');
    if (amp == std::string_view::npos) return std::string(input);

    std::string output;
    output.reserve(input.size());

    std::size_t pos = 0;
    char encoded[4];

    while (amp != std::string_view::npos) {
        output.append(input.substr(pos, amp - pos));

        const std::string_view rest = input.substr(amp + 1);
        int consumed = 0;

        if (!rest.empty() && rest[0] == '#') {
            std::uint32_t code_point = 0;
            consumed = match_numeric(rest, code_point);
            if (consumed > 0) {
                const int written = encode_utf8(code_point, encoded);
                output.append(encoded, std::size_t(written));
            }
        } else {
            std::string_view value;
            consumed = match_named(rest, in_attribute, value);
            if (consumed > 0) output.append(value.data(), value.size());
        }

        if (consumed == 0) output.push_back('&');

        pos = amp + 1 + std::size_t(consumed);
        amp = input.find('&', pos);
    }

    output.append(input.substr(pos));
    return output;
}

}  // namespace fh::detail

namespace fh {

std::string html_decode(std::string_view text, bool in_attribute) {
    return detail::decode_html(text, in_attribute);
}

}  // namespace fh
