// Document, Tag, TagQuery and the attribute/text helpers (SpanHandlingCore,
// TagQuery, extensions).
#include "internal.hpp"

#include <algorithm>
#include <limits>

namespace fh::detail {

  // ---------------------------------------------------------------------------
  // SpanHandlingCore
  // ---------------------------------------------------------------------------

  namespace {

    int next_word(std::string_view open, std::string_view attribute, int from) {
      while (std::size_t(from) < open.size()) {
        const std::size_t found = ifind(open, attribute, std::size_t(from));
        if (found == std::string_view::npos) break;

        from = int(found);
        const std::size_t after = found + attribute.size();

        const bool starts_name = from > 0
                                 && (is_space(open[std::size_t(from) - 1])
                                     || open[std::size_t(from) - 1] == '"'
                                     || open[std::size_t(from) - 1] == '\''
                                     || open[std::size_t(from) - 1] == '/'
                                     || open[std::size_t(from) - 1] == '=');
        const bool ends_name = after >= open.size() || is_space(open[after])
                               || open[after] == '=' || open[after] == '/'
                               || open[after] == '>';
        if (starts_name && ends_name) return from;

        from++;
      }
      return -1;
    }

    // Whether the quote at `quote` (or -1: none) stands right after an '='.
    bool opens_value(std::string_view before, int quote) {
      if (quote < 0) return false;

      int k = quote - 1;
      while (k > 0 && is_space(before[std::size_t(k)])) k--;
      return before[std::size_t(k)] == '=';
    }

    // Whether the word at `position` of an opening tag is an attribute name,
    // judged by the chars before it.
    bool is_name_for_sure(std::string_view open, int position) {
      int last = position - 1;
      if (last < 0 || !is_space(open[std::size_t(last)])) return false;
      while (last > 0 && is_space(open[std::size_t(last)])) last--;
      if (open[std::size_t(last)] == '=') return false;

      const std::string_view before = open.substr(0, std::size_t(last) + 1);
      int double_quote = -1;
      int single_quote = -1;

      if (before.size() <= 32) {
        for (int k = int(before.size()) - 1;
             k > 0 && (double_quote < 0 || single_quote < 0);
             k--) {
          if (before[std::size_t(k)] == '"') {
            if (double_quote < 0) double_quote = k;
          } else if (before[std::size_t(k)] == '\'' && single_quote < 0) {
            single_quote = k;
          }
        }
      } else {
        const std::size_t d = before.rfind('"');
        const std::size_t s = before.rfind('\'');
        double_quote = d == std::string_view::npos ? -1 : int(d);
        single_quote = s == std::string_view::npos ? -1 : int(s);
      }

      return !opens_value(before, double_quote)
             && !opens_value(before, single_quote);
    }

  }  // namespace

  int AttributeScanner::open_tag_end(const Tag &tag, int source_length) {
    const int open_end = tag.inner_start() > 0 && tag.inner_end() > 0
                             ? tag.inner_start() - 1
                             : tag.end();
    return std::min(open_end, source_length);
  }

  int AttributeScanner::read_value(std::string_view s,
                                   int position,
                                   int end,
                                   int &value_start,
                                   int &value_length) {
    int p = position;
    while (p < end && is_space(s[std::size_t(p)])) p++;

    if (p >= end || s[std::size_t(p)] != '=') {
      value_start = -1;
      value_length = 0;
      return position;
    }

    p++;
    while (p < end && is_space(s[std::size_t(p)])) p++;

    if (p < end && (s[std::size_t(p)] == '"' || s[std::size_t(p)] == '\'')) {
      const char quote = s[std::size_t(p)];
      value_start = p + 1;
      const std::size_t close = s.find(quote, std::size_t(value_start));
      if (close == std::string_view::npos || close >= std::size_t(end)) {
        value_length = end - value_start;
        return end;
      }
      value_length = int(close) - value_start;
      return int(close) + 1;
    }

    value_start = p;
    while (p < end && !is_space(s[std::size_t(p)])) p++;
    value_length = p - value_start;
    return p;
  }

  AttributeScanner::AttributeScanner(const Tag &tag) {
    source_ = tag.document().source();
    end_ = open_tag_end(tag, int(source_.size()));

    // skip '<' and the name
    int i = tag.start() + 1;
    while (i < end_) {
      const char c = source_[std::size_t(i)];
      if (is_space(c) || c == '/' || c == '>') break;
      i++;
    }
    pos_ = i;
  }

  bool AttributeScanner::next(int &name_start,
                              int &name_length,
                              int &value_start,
                              int &value_length) {
    while (true) {
      while (pos_ < end_
             && (is_space(source_[std::size_t(pos_)])
                 || source_[std::size_t(pos_)] == '/'))
        pos_++;
      if (pos_ >= end_) break;

      name_start = pos_;
      while (pos_ < end_) {
        const char c = source_[std::size_t(pos_)];
        if (is_space(c) || c == '=' || c == '/' || c == '>') break;
        pos_++;
      }
      name_length = pos_ - name_start;

      if (name_length == 0) {
        pos_++;  // a stray '='
        continue;
      }

      pos_ = read_value(source_, pos_, end_, value_start, value_length);
      return true;
    }

    name_start = name_length = value_start = value_length = 0;
    return false;
  }

  std::string_view span_attribute_by_name(const Tag &tag,
                                          std::string_view attribute) {
    // a comment has no attributes, whatever its text looks like
    if (!tag.valid() || attribute.empty() || tag.key() == int(HtmlTag::Comment))
      return {};

    const std::string_view source = tag.document().source();
    const int end = AttributeScanner::open_tag_end(tag, int(source.size()));
    const std::string_view open =
        source.substr(std::size_t(tag.start()), std::size_t(end - tag.start()));

    int position = next_word(open, attribute, 1);
    if (position < 0) return {};

    const bool first_word =
        tag.key() >= 0
        && position == int(kTagNames[std::size_t(tag.key())].size()) + 2;
    if (first_word || is_name_for_sure(open, position)) {
      int start = 0;
      int length = 0;
      AttributeScanner::read_value(
          source,
          tag.start() + position + int(attribute.size()),
          end,
          start,
          length);
      return start < 0 ? std::string_view{}
                       : source.substr(std::size_t(start), std::size_t(length));
    }

    // Otherwise the attributes are read one by one from the start of the tag.
    AttributeScanner scanner(tag);
    int name_start = 0, name_length = 0, value_start = 0, value_length = 0;
    while (scanner.next(name_start, name_length, value_start, value_length)) {
      const int name = name_start - tag.start();

      while (position < name) {
        position = next_word(open, attribute, position + 1);
        if (position < 0) return {};
      }

      if (position == name && name_length == int(attribute.size())) {
        return value_start < 0 ? std::string_view{}
                               : source.substr(std::size_t(value_start),
                                               std::size_t(value_length));
      }
    }

    return {};
  }

  std::string_view span_attribute_by_name(const Tag &tag, Attr attribute) {
    return span_attribute_by_name(tag, kAttrNames[std::size_t(attribute)]);
  }

  std::string decode_attribute(std::string_view raw) {
    return decode_html(raw, true);
  }

  bool attribute_equals(std::string_view raw, std::string_view value) {
    if (needs_decoding(raw)) return decode_attribute(raw) == value;
    return raw == value;
  }

  bool attribute_starts_with(std::string_view raw, std::string_view value) {
    if (needs_decoding(raw)) return decode_attribute(raw).starts_with(value);
    return raw.starts_with(value);
  }

  bool has_attribute(const Tag &tag, std::string_view name) {
    if (!span_attribute_by_name(tag, name).empty()) return true;

    AttributeScanner scanner(tag);
    int name_start = 0, name_length = 0, value_start = 0, value_length = 0;
    while (scanner.next(name_start, name_length, value_start, value_length)) {
      if (iequals(
              std::string_view(tag.document().source())
                  .substr(std::size_t(name_start), std::size_t(name_length)),
              name))
        return true;
    }
    return false;
  }

  std::string_view span_body(const Tag &tag) {
    if (!tag.valid()) return {};
    const int inner_start = tag.inner_start();
    const int inner_end = tag.inner_end();
    if (inner_start < 0 || inner_end <= 0) return {};

    return trim(std::string_view(tag.document().source())
                    .substr(std::size_t(inner_start),
                            std::size_t(inner_end - inner_start + 1)));
  }

  std::string_view span_source(const Tag &tag) {
    if (!tag.valid()) return {};
    const std::string_view source = tag.document().source();
    const int start = tag.start();
    const int end = tag.end();
    const int inner_start = tag.inner_start();
    if (end <= start || std::size_t(end) > source.size()) return {};

    int length;
    if (inner_start > 0 && tag.inner_end() > 0) {
      length = inner_start - start - 2;
    } else if (source[std::size_t(end) - 1] == '/') {
      length = end - start - 2;
    } else {
      length = end - start - 1;
    }

    return trim(source.substr(std::size_t(start) + 1,
                              std::size_t(std::max(length, 0))));
  }

  std::string_view span_full_source(const Tag &tag) {
    if (!tag.valid() || tag.inner_start() <= 0 || tag.inner_end() <= 0)
      return {};
    return trim(std::string_view(tag.document().source())
                    .substr(std::size_t(tag.start()),
                            std::size_t(tag.end() - tag.start() + 1)));
  }

  // The inner HTML matches `expected`: tags, comments and invisible content
  // removed, whitespace collapsed, entities decoded, compared on the fly.
  bool text_equals(const Tag &tag, std::string_view expected) {
    const std::string_view value = span_body(tag);

    if (value.empty() || expected.empty()) return false;

    std::size_t matched = 0;
    bool prev_whitespace = true;
    bool pending_space = false;

    for (std::size_t i = 0; i < value.size(); i++) {
      const char c = value[i];

      if (c == '<' && is_markup_start(value, i)) {
        const int last = skip_markup(value, i);
        if (last < 0) break;
        i = std::size_t(last);
      } else if (is_space(c)) {
        if (!prev_whitespace) {
          pending_space = true;
          prev_whitespace = true;
        }
      } else {
        if (c == '&') return clear_tags(value) == expected;

        if (pending_space) {
          if (matched >= expected.size() || expected[matched] != ' ')
            return false;
          matched++;
          pending_space = false;
        }

        if (matched >= expected.size() || expected[matched] != c) return false;
        matched++;
        prev_whitespace = false;
      }
    }

    return matched == expected.size();
  }

  // ---------------------------------------------------------------------------
  // Search
  // ---------------------------------------------------------------------------

  void collect_matches(const Scope &scope,
                       const TagQuery &query,
                       std::vector<Tag> &output) {
    const Document &document = *scope.document;
    const auto &data = document.tag_data();

    for (int i = scope.first; i < int(data.size()); i++) {
      const TagData &tag = data[std::size_t(i)];
      if (tag.end > scope.end) break;

      const Tag handle(&document, i);
      if (query.matches(handle)) output.push_back(handle);
    }
  }

  std::optional<Tag> find_in(const Scope &scope, const TagQuery &query) {
    const Document &document = *scope.document;
    const auto &data = document.tag_data();

    for (int i = scope.first; i < int(data.size()); i++) {
      const TagData &tag = data[std::size_t(i)];
      if (tag.end > scope.end) break;

      const Tag handle(&document, i);
      if (query.matches(handle)) return handle;
    }
    return std::nullopt;
  }

  std::vector<Tag> find_all_in(const Scope &scope, const TagQuery &query) {
    std::vector<Tag> result;
    collect_matches(scope, query, result);
    return result;
  }

}  // namespace fh::detail

namespace fh {

  // ---------------------------------------------------------------------------
  // Tag
  // ---------------------------------------------------------------------------

  std::int32_t Tag::start() const {
    return valid() ? detail::data_of(*this).start : 0;
  }
  std::int32_t Tag::end() const {
    return valid() ? detail::data_of(*this).end : 0;
  }
  std::int32_t Tag::inner_start() const {
    return valid() ? detail::data_of(*this).inner_start : 0;
  }
  std::int32_t Tag::inner_end() const {
    return valid() ? detail::data_of(*this).inner_end : 0;
  }
  std::int32_t Tag::parent_index() const {
    return valid() ? detail::data_of(*this).parent : -1;
  }
  std::int32_t Tag::depth() const {
    return valid() ? detail::data_of(*this).depth : 0;
  }
  int Tag::key() const {
    return valid() ? detail::data_of(*this).key : -1;
  }

  std::string_view Tag::name() const {
    if (!valid()) return {};
    const TagData &data = detail::data_of(*this);
    if (data.key >= 0) return kTagNames[std::size_t(data.key)];
    if (data.custom >= 0)
      return document_->custom_names()[std::size_t(data.custom)];
    return {};
  }

  bool Tag::has_children() const {
    if (!valid()) return false;
    const Document &doc = *document_;
    if (std::size_t(index_ + 1) >= doc.tag_data().size()) return false;
    return doc.tag_data()[std::size_t(index_ + 1)].start < end();
  }

  std::string Tag::text() const {
    if (!valid()) return {};
    const std::string_view body = detail::span_body(*this);

    if (!has_children()
        && (body.find('<') == std::string_view::npos
            || detail::is_raw_text(key()))) {
      return detail::needs_decoding(body) ? detail::decode_html(body)
                                          : std::string(body);
    }

    return detail::clear_tags(body);
  }

  std::string Tag::text_line_breaks() const {
    return detail::clear_tags_line_breaks(detail::span_body(*this));
  }
  std::string Tag::body() const {
    return std::string(detail::span_body(*this));
  }
  std::string Tag::source() const {
    return std::string(detail::span_source(*this));
  }
  std::string Tag::full_source() const {
    return std::string(detail::span_full_source(*this));
  }

  std::string Tag::attribute(std::string_view name) const {
    return detail::decode_attribute(
        detail::span_attribute_by_name(*this, name));
  }

  std::string Tag::attribute(Attr name) const {
    return attribute(kAttrNames[std::size_t(name)]);
  }

  std::string Tag::attribute_body(std::string_view name) const {
    return std::string(detail::span_attribute_by_name(*this, name));
  }

  std::string Tag::attribute_body(Attr name) const {
    return attribute_body(kAttrNames[std::size_t(name)]);
  }

  std::string Tag::attribute_href(std::string_view current_url) const {
    return detail::resolve_link(
        detail::span_attribute_by_name(*this, Attr::Href),
        document_->page_url(),
        current_url);
  }

  std::string Tag::attribute_src(std::string_view current_url) const {
    return detail::resolve_link(
        detail::span_attribute_by_name(*this, Attr::Src),
        document_->page_url(),
        current_url);
  }

  bool Tag::is_name(HtmlTag name) const {
    return key() == int(name);
  }

  bool Tag::is_name(std::string_view name) const {
    return detail::iequals(name, this->name());
  }

  bool Tag::is_class(std::string_view class_name) const {
    if (!valid() || class_name.empty()) return false;

    const std::string_view raw =
        detail::span_attribute_by_name(*this, Attr::Class);
    const std::string decoded = detail::needs_decoding(raw)
                                    ? detail::decode_attribute(raw)
                                    : std::string(raw);
    if (decoded.empty()) return false;

    // one of the space-separated classes, case-insensitive
    const std::size_t attr_len = decoded.size();
    const std::size_t class_len = class_name.size();
    if (attr_len == class_len) return detail::iequals(decoded, class_name);
    if (class_len > attr_len) return false;

    std::size_t from = 0;
    while (from < attr_len) {
      const std::size_t index = detail::ifind(decoded, class_name, from);
      if (index == std::string_view::npos) break;

      const bool start_ok = index == 0 || detail::is_space(decoded[index - 1]);
      if (start_ok) {
        const std::size_t end_index = index + class_len;
        const bool end_ok =
            end_index == attr_len || detail::is_space(decoded[end_index]);
        if (end_ok) return true;
      }

      from = index + class_len;
    }

    return false;
  }

  bool Tag::is_attribute(std::string_view name, std::string_view value) const {
    return detail::attribute_equals(detail::span_attribute_by_name(*this, name),
                                    value);
  }

  bool Tag::is_attribute(Attr name, std::string_view value) const {
    return is_attribute(kAttrNames[std::size_t(name)], value);
  }

  bool Tag::is(const TagQuery &query) const {
    return query.matches(*this);
  }

  std::optional<Tag> Tag::find(const TagQuery &query) const {
    return valid() ? detail::find_in(detail::scope_of(*this), query)
                   : std::nullopt;
  }

  std::vector<Tag> Tag::find_all(const TagQuery &query) const {
    return valid() ? detail::find_all_in(detail::scope_of(*this), query)
                   : std::vector<Tag>{};
  }

  TagView Tag::find_all_view(const TagQuery &query) const {
    if (!valid()) return {};

    // Descendants are the tags that start before the scope's end; the tag list
    // is ordered by start, so the first one past the scope is found by binary
    // search once instead of checking the boundary on every iteration.
    const Document &doc = document();
    const auto &data = doc.tag_data();
    const int first = index() + 1;
    const auto stop = std::lower_bound(
        data.begin() + first,
        data.end(),
        end(),
        [](const TagData &tag, int value) { return tag.start < value; });
    return TagView(doc, query, first, int(stop - data.begin()));
  }

  std::vector<Tag> Tag::css(std::string_view selector) const {
    return CssQuery::cached(selector).select_all(*this);
  }

  std::vector<Tag> Tag::css(const CssQuery &query) const {
    return query.select_all(*this);
  }

  std::optional<Tag> Tag::css_first(std::string_view selector) const {
    return CssQuery::cached(selector).select_first(*this);
  }

  std::optional<Tag> Tag::css_first(const CssQuery &query) const {
    return query.select_first(*this);
  }

  bool Tag::is_css(std::string_view selector) const {
    return CssQuery::cached(selector).matches(*this);
  }
  bool Tag::is_css(const CssQuery &query) const {
    return query.matches(*this);
  }

  std::vector<Tag> Tag::xpath(std::string_view expression) const {
    return XPathQuery::cached(expression).select_tags(*this);
  }

  std::vector<Tag> Tag::xpath(const XPathQuery &query) const {
    return query.select_tags(*this);
  }

  std::vector<std::string> Tag::xpath_values(
      std::string_view expression) const {
    return XPathQuery::cached(expression).select_values(*this);
  }

  std::vector<std::string> Tag::xpath_values(const XPathQuery &query) const {
    return query.select_values(*this);
  }

  std::optional<std::string> Tag::xpath_value(
      std::string_view expression) const {
    return XPathQuery::cached(expression).select_value(*this);
  }

  std::optional<std::string> Tag::xpath_value(const XPathQuery &query) const {
    return query.select_value(*this);
  }

  std::optional<Tag> Tag::previous_sibling() const {
    return valid() ? document_->css_tree().previous_tag(*this) : std::nullopt;
  }
  std::optional<Tag> Tag::next_sibling() const {
    return valid() ? document_->css_tree().next_tag(*this) : std::nullopt;
  }

  std::optional<Tag> Tag::parent() const {
    if (!valid() || parent_index() < 0) return std::nullopt;
    return Tag(document_, parent_index());
  }

  // ---------------------------------------------------------------------------
  // TagQuery
  // ---------------------------------------------------------------------------

  TagQuery::TagQuery(HtmlTag name) : key(int(name)) {}

  TagQuery::TagQuery(std::string_view name) : name(name) {
    has_name = !name.empty();
    if (has_name) {
      const int known = detail::lookup_tag_name(name);
      if (known >= 0) {
        key = known;
        this->name.clear();
        has_name = false;
      }
    }
  }

  bool TagQuery::matches(const Tag &tag) const {
    if (key >= 0) {
      if (tag.key() != key
          && (tag.key() >= 0 || !has_name || !tag.is_name(name)))
        return false;
    } else if (has_name && !tag.is_name(name)) {
      return false;
    }

    // attributes
    if (id && ((*id).empty() || !tag.is_attribute(Attr::Id, *id))) return false;
    if (attribute_value) {
      if (attribute) {
        if (!tag.is_attribute(*attribute, *attribute_value)) return false;
      } else if (attribute_name) {
        if (!tag.is_attribute(*attribute_name, *attribute_value)) return false;
      } else {
        return false;
      }
    }
    if (class_value && !tag.is_class(*class_value)) return false;

    if (id_starts_with
        && !detail::attribute_starts_with(
            detail::span_attribute_by_name(tag, Attr::Id), *id_starts_with))
      return false;
    if (source && std::string_view(detail::span_source(tag)) != *source)
      return false;
    if (body && std::string(detail::span_body(tag)) != *body) return false;
    if (body_starts_with
        && !detail::span_body(tag).starts_with(*body_starts_with))
      return false;

    // the text last: it is the most expensive check
    if (text && !detail::text_equals(tag, *text)) return false;
    if (text_starts_with && !tag.text().starts_with(*text_starts_with))
      return false;

    return true;
  }

  // ---------------------------------------------------------------------------
  // Document
  // ---------------------------------------------------------------------------

  Document::Document(std::string_view source, std::string_view page_url)
      : owned_source_(source),
        page_url_(page_url),
        source_view_(owned_source_) {
    detail::ParseResult parsed = detail::parse(source_view_);
    set_parsed(std::move(parsed.tags), std::move(parsed.custom_names));
  }

  Document::Document(BorrowedTag,
                     std::string_view source,
                     std::string_view page_url)
      : page_url_(page_url), source_view_(source) {
    detail::ParseResult parsed = detail::parse(source_view_);
    set_parsed(std::move(parsed.tags), std::move(parsed.custom_names));
  }

  Document::Document(OwnedTag, std::string &&source, std::string_view page_url)
      : owned_source_(std::move(source)),
        page_url_(page_url),
        source_view_(owned_source_) {
    detail::ParseResult parsed = detail::parse(source_view_);
    set_parsed(std::move(parsed.tags), std::move(parsed.custom_names));
  }

  Document Document::parse_borrowed(std::string_view source,
                                    std::string_view page_url) {
    return Document(BorrowedTag{}, source, page_url);
  }

  Document Document::parse_owned(std::string source,
                                 std::string_view page_url) {
    return Document(OwnedTag{}, std::move(source), page_url);
  }

  Document::~Document() = default;

  void Document::set_parsed(std::vector<TagData> data,
                            std::vector<std::string> custom_names) {
    data_ = std::move(data);
    custom_names_ = std::move(custom_names);
  }

  const std::vector<Tag> &Document::tags() const {
    std::call_once(tags_once_, [this] {
      tags_.reserve(data_.size());
      for (int i = 0; i < int(data_.size()); i++) tags_.emplace_back(this, i);
    });
    return tags_;
  }

  const detail::CssTree &Document::css_tree() const {
    std::call_once(css_tree_once_, [this] {
      css_tree_ = std::make_shared<detail::CssTree>(*this);
    });
    return *css_tree_;
  }

  std::optional<Tag> Document::find(const TagQuery &query) const {
    return detail::find_in(detail::scope_of(*this), query);
  }

  std::vector<Tag> Document::find_all(const TagQuery &query) const {
    return detail::find_all_in(detail::scope_of(*this), query);
  }

  TagView Document::find_all_view(const TagQuery &query) const {
    return TagView(*this, query);
  }

  std::vector<Tag> Document::css(std::string_view selector) const {
    return CssQuery::cached(selector).select_all(*this);
  }

  std::vector<Tag> Document::css(const CssQuery &query) const {
    return query.select_all(*this);
  }

  std::optional<Tag> Document::css_first(std::string_view selector) const {
    return CssQuery::cached(selector).select_first(*this);
  }

  std::optional<Tag> Document::css_first(const CssQuery &query) const {
    return query.select_first(*this);
  }

  std::vector<Tag> Document::xpath(std::string_view expression) const {
    return XPathQuery::cached(expression).select_tags(*this);
  }

  std::vector<Tag> Document::xpath(const XPathQuery &query) const {
    return query.select_tags(*this);
  }

  std::vector<std::string> Document::xpath_values(
      std::string_view expression) const {
    return XPathQuery::cached(expression).select_values(*this);
  }

  std::vector<std::string> Document::xpath_values(
      const XPathQuery &query) const {
    return query.select_values(*this);
  }

  std::optional<std::string> Document::xpath_value(
      std::string_view expression) const {
    return XPathQuery::cached(expression).select_value(*this);
  }

  std::optional<std::string> Document::xpath_value(
      const XPathQuery &query) const {
    return query.select_value(*this);
  }

  // ---------------------------------------------------------------------------
  // Free functions
  // ---------------------------------------------------------------------------

  std::string_view tag_name(HtmlTag tag) {
    return kTagNames[std::size_t(tag)];
  }
  std::string_view attr_name(Attr attribute) {
    return kAttrNames[std::size_t(attribute)];
  }

  bool is_comment(const Tag &tag) {
    return tag.valid() && tag.key() == int(HtmlTag::Comment);
  }

  bool is_element(const Tag &tag) {
    if (!tag.valid()) return false;
    const int key = tag.key();
    if (key >= 0) {
      return key != int(HtmlTag::Comment) && key != int(HtmlTag::Doctype)
             && key != int(HtmlTag::XmlDeclaration);
    }
    const std::string_view name = tag.name();
    return !name.empty() && name[0] != '!' && name[0] != '?';
  }

  std::string clear_tags(std::string_view html) {
    return detail::clear_tags(html);
  }

  std::optional<HtmlTag> try_get_tag(std::string_view name) {
    const int key = detail::lookup_tag_name(name);
    if (key < 0) return std::nullopt;
    return HtmlTag(key);
  }

  std::optional<Tag> find_by_name(const Document &scope, HtmlTag name) {
    return scope.find(TagQuery(name));
  }
  std::optional<Tag> find_by_name(const Document &scope,
                                  std::string_view name) {
    return name.empty() ? std::nullopt : scope.find(TagQuery(name));
  }
  std::optional<Tag> find_by_name(const Tag &scope, HtmlTag name) {
    return scope.find(TagQuery(name));
  }
  std::optional<Tag> find_by_name(const Tag &scope, std::string_view name) {
    return name.empty() ? std::nullopt : scope.find(TagQuery(name));
  }
  std::optional<Tag> find_by_id(const Document &scope, std::string_view id) {
    return id.empty() ? std::nullopt
                      : scope.find(TagQuery().with_id(std::string(id)));
  }
  std::optional<Tag> find_by_id(const Tag &scope, std::string_view id) {
    return id.empty() ? std::nullopt
                      : scope.find(TagQuery().with_id(std::string(id)));
  }
  std::optional<Tag> find_by_name_class(const Document &scope,
                                        HtmlTag name,
                                        std::string_view class_value) {
    return scope.find(TagQuery(name).with_class(std::string(class_value)));
  }
  std::optional<Tag> find_by_name_class(const Tag &scope,
                                        HtmlTag name,
                                        std::string_view class_value) {
    return scope.find(TagQuery(name).with_class(std::string(class_value)));
  }
  std::vector<Tag> find_all_by_name(const Document &scope, HtmlTag name) {
    return scope.find_all(TagQuery(name));
  }
  std::vector<Tag> find_all_by_name(const Tag &scope, HtmlTag name) {
    return scope.find_all(TagQuery(name));
  }
  std::vector<Tag> find_all_by_name_class(const Document &scope,
                                          HtmlTag name,
                                          std::string_view class_value) {
    return scope.find_all(TagQuery(name).with_class(std::string(class_value)));
  }
  std::vector<Tag> find_all_by_name_class(const Tag &scope,
                                          HtmlTag name,
                                          std::string_view class_value) {
    return scope.find_all(TagQuery(name).with_class(std::string(class_value)));
  }

}  // namespace fh
