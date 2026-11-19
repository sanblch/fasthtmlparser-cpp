// Internal helpers shared by the implementation files.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fh/fasthtmlparser.hpp"
#include "fh/generated_entities.hpp"

namespace fh::detail {

  // ---------------------------------------------------------------------------
  // ASCII helpers (the C# code uses char.IsWhiteSpace / char.IsLetter and works
  // on UTF-16; here the source is UTF-8 bytes and the HTML syntax itself is
  // ASCII).
  // ---------------------------------------------------------------------------

  inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
           || c == '\v';
  }
  inline bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
  }
  inline bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }
  inline bool is_ascii_alnum(char c) {
    return is_ascii_alpha(c) || is_ascii_digit(c);
  }
  inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
  }
  inline char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? char(c - 32) : c;
  }

  bool iequals(std::string_view a, std::string_view b);
  bool iequals_ascii_prefix(std::string_view value, std::string_view prefix);
  std::string to_lower(std::string_view value);
  std::string_view trim(std::string_view value);
  std::string_view trim_end(std::string_view value);
  std::string_view trim_start(std::string_view value);

  // Case-insensitive find; returns npos when absent.
  std::size_t ifind(std::string_view haystack,
                    std::string_view needle,
                    std::size_t from = 0);
  std::size_t ifind_char(std::string_view haystack,
                         char needle,
                         std::size_t from = 0);

  // ---------------------------------------------------------------------------
  // Name tables (the dictionary lookups of ParsingCore)
  // ---------------------------------------------------------------------------

  // Case-insensitive lookup of a tag name; -1 when it is not one of the known
  // names.
  int lookup_tag_name(std::string_view name);
  // Case-insensitive lookup of an attribute name; -1 when unknown.
  int lookup_attr_name(std::string_view name);

  // ---------------------------------------------------------------------------
  // Character references (HtmlDecodeCore)
  // ---------------------------------------------------------------------------

  bool needs_decoding(std::string_view value);
  std::string normalize_newlines(std::string_view value);
  std::string decode_html(std::string_view input, bool in_attribute = false);
  // Decodes one reference: `after_amp` is the text right after '&'. Writes at
  // most 4 bytes and returns the number of bytes consumed after '&' (0: not a
  // reference).
  int decode_reference(std::string_view after_amp, char out[4], int &written);

  // ---------------------------------------------------------------------------
  // Text (HtmlToTextCore and the text part of ParsingCore)
  // ---------------------------------------------------------------------------

  bool is_markup_start(std::string_view value, std::size_t i);
  // Index of the last char of what is not text at `i` (a comment, a tag or a
  // tag with invisible content), or -1 for an unterminated tag.
  int skip_markup(std::string_view value, std::size_t i);
  // The visible text of `value` written to `destination`; returns the length.
  std::size_t strip_tags(std::string_view value, char *destination);
  std::string clear_tags(std::string_view value);
  std::string clear_tags_line_breaks(std::string_view value);

  int find_comment_end(std::string_view source, int content_start);
  int find_raw_text_end(std::string_view source,
                        int from,
                        std::string_view end_tag);
  std::string_view invisible_content_end(std::string_view open_tag);
  bool is_raw_text(int key);

  // ---------------------------------------------------------------------------
  // Tag helpers
  // ---------------------------------------------------------------------------

  inline const TagData &data_of(const Tag &tag) {
    return tag.document().tag_data()[std::size_t(tag.index())];
  }

  std::string_view span_body(const Tag &tag);
  std::string_view span_source(const Tag &tag);
  std::string_view span_full_source(const Tag &tag);

  // The value of an href/src: character references decoded and made absolute
  // against the current URL (or the page URL); percent-escapes are kept as
  // written.
  std::string resolve_link(std::string_view raw,
                           std::string_view page_url,
                           std::string_view current_url);

  // ---------------------------------------------------------------------------
  // Attributes (AttributeScanner and SpanHandlingCore)
  // ---------------------------------------------------------------------------

  // Reads the attributes of an opening tag as name / value positions in the
  // page source.
  class AttributeScanner {
   public:
    explicit AttributeScanner(const Tag &tag);

    // Position of the '>' that ends the opening tag of `tag`.
    static int open_tag_end(const Tag &tag, int source_length);

    // Reads the value that follows an attribute name; `position` is right after
    // the name and `end` is the closing '>' of the opening tag. Returns the
    // position after the value; value_start is -1 when the name has no '='.
    static int read_value(std::string_view s,
                          int position,
                          int end,
                          int &value_start,
                          int &value_length);

    bool next(int &name_start,
              int &name_length,
              int &value_start,
              int &value_length);

   private:
    std::string_view source_;
    int end_ = 0;  // exclusive: the closing '>' of the opening tag
    int pos_ = 0;
  };

  // Raw value of the first attribute with the given name (case-insensitive);
  // empty when missing.
  std::string_view span_attribute_by_name(const Tag &tag,
                                          std::string_view attribute);
  std::string_view span_attribute_by_name(const Tag &tag, Attr attribute);
  std::string decode_attribute(std::string_view raw);
  bool attribute_equals(std::string_view raw, std::string_view value);
  bool attribute_starts_with(std::string_view raw, std::string_view value);
  bool has_attribute(const Tag &tag, std::string_view name);

  // Comparisons without allocation unless the raw value contains '&'.
  bool text_equals(const Tag &tag, std::string_view expected);

  // ---------------------------------------------------------------------------
  // Parsing
  // ---------------------------------------------------------------------------

  struct ParseResult {
    std::vector<TagData> tags;
    // One string per distinct custom tag name, lower-case.
    std::vector<std::string> custom_names;
  };

  ParseResult parse(std::string_view source);

  // ---------------------------------------------------------------------------
  // CSS tree: sibling links and positions of the elements of a document
  // ---------------------------------------------------------------------------

  struct CssTree {
    const Document *document = nullptr;
    std::vector<int>
        previous;  // index of the previous element sibling, -1 for none
    std::vector<int> next;      // index of the next element sibling
    std::vector<int> position;  // 1-based position among the element siblings
    std::vector<int>
        from_end;  // 1-based position from the last element sibling

    mutable std::vector<int> type_position;
    mutable std::vector<int> type_from_end;
    mutable bool types_ready = false;

    explicit CssTree(const Document &doc);

    std::optional<Tag> previous_tag(const Tag &tag) const;
    std::optional<Tag> next_tag(const Tag &tag) const;
    int position_of(const Tag &tag) const;
    int position_from_end_of(const Tag &tag) const;
    int type_position_of(const Tag &tag) const;
    int type_position_from_end_of(const Tag &tag) const;

   private:
    void build_types() const;
  };

  // ---------------------------------------------------------------------------
  // Search scope
  // ---------------------------------------------------------------------------

  struct Scope {
    const Document *document = nullptr;
    int first = 0;
    int end = INT32_MAX;
  };

  inline Scope scope_of(const Document &doc) {
    return {&doc, 0, INT32_MAX};
  }
  inline Scope scope_of(const Tag &tag) {
    return {&tag.document(), tag.index() + 1, tag.end()};
  }

  std::optional<Tag> find_in(const Scope &scope, const TagQuery &query);
  std::vector<Tag> find_all_in(const Scope &scope, const TagQuery &query);

  // The matches of `query` from the scope, appended to `output` (used by CSS
  // and XPath too).
  void collect_matches(const Scope &scope,
                       const TagQuery &query,
                       std::vector<Tag> &output);

}  // namespace fh::detail
