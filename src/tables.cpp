#include "internal.hpp"

#include <array>
#include <cstdint>

namespace fh::detail {

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); i++) {
    if (ascii_lower(a[i]) != ascii_lower(b[i]))
      return false;
  }
  return true;
}

bool iequals_ascii_prefix(std::string_view value, std::string_view prefix) {
  if (value.size() < prefix.size())
    return false;
  return iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && is_space(value[begin]))
    begin++;
  while (end > begin && is_space(value[end - 1]))
    end--;
  return value.substr(begin, end - begin);
}

std::string_view trim_end(std::string_view value) {
  std::size_t end = value.size();
  while (end > 0 && is_space(value[end - 1]))
    end--;
  return value.substr(0, end);
}

std::string_view trim_start(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && is_space(value[begin]))
    begin++;
  return value.substr(begin);
}

std::string to_lower(std::string_view value) {
  std::string result(value);
  for (char &c : result)
    c = ascii_lower(c);
  return result;
}

std::size_t ifind(std::string_view haystack, std::string_view needle,
                  std::size_t from) {
  if (needle.empty())
    return from <= haystack.size() ? from : std::string_view::npos;
  if (from > haystack.size() || needle.size() > haystack.size() - from)
    return std::string_view::npos;

  const char first = ascii_lower(needle[0]);
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t i = from; i <= last; i++) {
    if (ascii_lower(haystack[i]) != first)
      continue;
    std::size_t k = 1;
    while (k < needle.size() &&
           ascii_lower(haystack[i + k]) == ascii_lower(needle[k]))
      k++;
    if (k == needle.size())
      return i;
  }
  return std::string_view::npos;
}

std::size_t ifind_char(std::string_view haystack, char needle,
                       std::size_t from) {
  const char lower = ascii_lower(needle);
  for (std::size_t i = from; i < haystack.size(); i++) {
    if (ascii_lower(haystack[i]) == lower)
      return i;
  }
  return std::string_view::npos;
}

namespace {

// Names are looked up for every tag and attribute, so the map is an open
// addressing table: hashing the lower-cased bytes is cheaper than an
// unordered_map lookup, and there are no nodes to chase. The tables are
// built at compile time and live in read-only data.
constexpr std::size_t kLookupSize = 1024; // 10 bits, a power of two
constexpr std::size_t kMaxNameLength = 32;

constexpr std::size_t name_hash(std::string_view name) noexcept {
  std::uint32_t hash = 0;
  for (char c : name) {
    hash = hash * 31 + static_cast<unsigned char>(ascii_lower(c));
  }
  return ((hash + static_cast<std::uint32_t>(name.size())) * 2654435769u) >> 22;
}

struct NameTable {
  std::array<std::int16_t, kLookupSize> slots{};
  std::array<std::string_view, kLookupSize> names{};
  constexpr NameTable() { slots.fill(-1); }
};

constexpr NameTable make_name_table(const std::string_view *names, int count) {
  NameTable table;
  for (int i = 0; i < count; i++) {
    const std::string_view name = names[i];
    std::size_t slot = name_hash(name);
    while (table.slots[slot] >= 0)
      slot = (slot + 1) & (kLookupSize - 1);
    table.slots[slot] = static_cast<std::int16_t>(i);
    table.names[slot] = name;
  }
  return table;
}

int find_name(const NameTable &table, std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxNameLength)
    return -1;
  std::size_t slot = name_hash(name);
  for (;;) {
    const int index = table.slots[slot];
    if (index < 0)
      return -1;
    if (iequals(table.names[slot], name))
      return index;
    slot = (slot + 1) & (kLookupSize - 1);
  }
}

inline constexpr NameTable kTagTable = make_name_table(kTagNames, kTagCount);
inline constexpr NameTable kAttrTable = make_name_table(kAttrNames, kAttrCount);

} // namespace

int lookup_tag_name(std::string_view name) {
  return find_name(kTagTable, name);
}

int lookup_attr_name(std::string_view name) {
  return find_name(kAttrTable, name);
}

} // namespace fh::detail
