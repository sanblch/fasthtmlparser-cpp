// FastHtmlParser — C++ port of z3l.FastHtmlParser
// (https://github.com/romanti3172/z3l.FastHtmlParser).
//
// A parsed HTML/XML document is a flat list of tags over the source text: no
// DOM tree, no per-node objects. Parsing happens once; the document is then
// read-only and can be queried from several threads.
//
// The public surface follows the original C# library:
//   Document          ~ HtmlParser
//   Tag               ~ ParserTag
//   TagQuery          ~ TagQuery
//   CssQuery          ~ CssQuery
//   XPathQuery        ~ XPathQuery
//   find/find_all     ~ Find / FindAll (extension methods in C#)
//   text()/body()/... ~ Text / Body / Source / FullSource
//
// The source is treated as UTF-8 bytes (the original works on UTF-16 code
// units), so positions are byte offsets. Character references are decoded to
// UTF-8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "fh/generated_names.hpp"

namespace fh {

  class Document;
  class Tag;
  class CssQuery;
  class XPathQuery;
  class TagView;
  struct TagQuery;
  namespace detail {
    struct CssSelectorList;
    struct CssTree;
    struct XPathExpr;
    struct ParseResult;
  }  // namespace detail

  // ---------------------------------------------------------------------------
  // Tag data
  // ---------------------------------------------------------------------------

  // One tag of a parsed document; positions are byte offsets into
  // Document::source(). Start: the opening '<'. End: the last '>' (of the
  // closing tag when there is one, otherwise of the opening tag).
  // InnerStart/InnerEnd: positions right after the opening tag and at the last
  // char before the closing tag; 0 when the tag is not closed.
  struct TagData {
    std::int32_t start = 0;
    std::int32_t end = 0;
    std::int32_t inner_start = 0;
    std::int32_t inner_end = 0;
    std::int32_t parent =
        -1;  // index of the enclosing tag, -1 at the top level
    std::int16_t depth = 0;
    std::int16_t key = -1;  // (int)Tag, or -1 for a name that is not in Tag
    std::int32_t custom =
        -1;  // index into Document::custom_names(), for a name not in Tag
  };

  // A handle to one tag: a document pointer and an index in its tag list.
  class Tag {
   public:
    Tag() = default;
    Tag(const Document *document, int index)
        : document_(document), index_(index) {}

    bool valid() const {
      return document_ != nullptr && index_ >= 0;
    }
    explicit operator bool() const {
      return valid();
    }

    const Document &document() const {
      return *document_;
    }
    int index() const {
      return index_;
    }

    std::int32_t start() const;
    std::int32_t end() const;
    std::int32_t inner_start() const;
    std::int32_t inner_end() const;
    std::int32_t parent_index() const;
    std::int32_t depth() const;

    // (int)HtmlTag, or -1 for names that are not in the enum.
    int key() const;
    // Lower-case tag name ("div", "!--", "custom-tag", ...); empty for an
    // invalid tag.
    std::string_view name() const;
    // Whether another tag starts inside this one.
    bool has_children() const;

    // Visible text with character references decoded: tags, comments and the
    // content of script/style/noscript/template/noframes/noembed removed,
    // whitespace collapsed.
    std::string text() const;
    // Text with line breaks kept: <br> and block elements -> new line, p/h1-h6
    // -> blank line.
    std::string text_line_breaks() const;
    // Inner HTML (trimmed); empty for unclosed tags.
    std::string body() const;
    // The opening tag without '<' and '>' ("a href=\"/\"").
    std::string source() const;
    // Outer HTML from the opening '<' to the closing '>' (trimmed).
    std::string full_source() const;

    // Decoded attribute value (HTML5 attribute rules); empty when missing.
    std::string attribute(std::string_view name) const;
    std::string attribute(Attr name) const;
    // The value exactly as written in the source, without decoding.
    std::string attribute_body(std::string_view name) const;
    std::string attribute_body(Attr name) const;

    // The href/src as a browser follows it: decoded and resolved against the
    // page URL.
    std::string attribute_href(std::string_view current_url = {}) const;
    std::string attribute_src(std::string_view current_url = {}) const;

    bool is_name(HtmlTag name) const;
    bool is_name(std::string_view name) const;
    bool is_class(std::string_view class_name) const;
    bool is_attribute(std::string_view name, std::string_view value) const;
    bool is_attribute(Attr name, std::string_view value) const;
    bool is(const TagQuery &query) const;

    // Search in the tag's descendants, in document order.
    std::optional<Tag> find(const TagQuery &query) const;
    std::vector<Tag> find_all(const TagQuery &query) const;
    // The same matches as find_all, yielded lazily and without the vector.
    TagView find_all_view(const TagQuery &query) const;

    std::vector<Tag> css(std::string_view selector) const;
    std::vector<Tag> css(const CssQuery &query) const;
    std::optional<Tag> css_first(std::string_view selector) const;
    std::optional<Tag> css_first(const CssQuery &query) const;
    bool is_css(std::string_view selector) const;
    bool is_css(const CssQuery &query) const;

    std::vector<Tag> xpath(std::string_view expression) const;
    std::vector<Tag> xpath(const XPathQuery &query) const;
    std::vector<std::string> xpath_values(std::string_view expression) const;
    std::vector<std::string> xpath_values(const XPathQuery &query) const;
    std::optional<std::string> xpath_value(std::string_view expression) const;
    std::optional<std::string> xpath_value(const XPathQuery &query) const;

    // Sibling navigation over elements (comments do not count).
    std::optional<Tag> previous_sibling() const;
    std::optional<Tag> next_sibling() const;
    std::optional<Tag> parent() const;

    friend bool operator==(const Tag &a, const Tag &b) {
      return a.document_ == b.document_ && a.index_ == b.index_;
    }
    friend bool operator!=(const Tag &a, const Tag &b) {
      return !(a == b);
    }

   private:
    const Document *document_ = nullptr;
    int index_ = -1;
  };

  // ---------------------------------------------------------------------------
  // Document
  // ---------------------------------------------------------------------------

  // A parsed page: the source text and the flat list of tags in document order.
  class Document {
   public:
    // Parses a copy of the text. pageUrl is used to resolve relative links.
    // A Document owns its tags: Tag handles point at the Document, so it
    // neither moves nor copies. Construct it in place (Document doc(html);), do
    // not put it into a container by value.
    explicit Document(std::string_view source, std::string_view page_url = {});
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;
    Document(Document &&) = delete;
    Document &operator=(Document &&) = delete;
    ~Document();

    static Document parse(std::string_view source,
                          std::string_view page_url = {}) {
      return Document(source, page_url);
    }

    // Parses without taking a copy: `source` must outlive the document and must
    // not be modified while the document is used (like a Rust &str or a C#
    // string). Fastest, but the caller owns the lifetime.
    static Document parse_borrowed(std::string_view source,
                                   std::string_view page_url = {});

    // Parses a document that takes ownership of `source` without copying it.
    static Document parse_owned(std::string source,
                                std::string_view page_url = {});

    // The source exactly as given: the owned copy or the borrowed text.
    std::string_view source() const {
      return source_view_;
    }
    // URL of the page, for resolving relative links.
    const std::string &page_url() const {
      return page_url_;
    }

    const std::vector<TagData> &tag_data() const {
      return data_;
    }
    const std::vector<std::string> &custom_names() const {
      return custom_names_;
    }
    std::size_t tag_count() const {
      return data_.size();
    }
    Tag tag(int index) const {
      return Tag(this, index);
    }

    // All tags in document order. Built on first use: parsing alone does not
    // fill it.
    const std::vector<Tag> &tags() const;

    // Sibling links and positions of the elements, built on first use (CSS
    // combinators).
    const detail::CssTree &css_tree() const;

    // First tag that matches every criterion, or nullopt.
    std::optional<Tag> find(const TagQuery &query) const;
    // All tags that match every criterion, in document order.
    std::vector<Tag> find_all(const TagQuery &query) const;
    // The same matches, yielded lazily and without the vector. The view copies
    // the query and reads the document on demand, so it works with
    // std::views::take/filter and stops as soon as iteration stops.
    TagView find_all_view(const TagQuery &query) const;

    std::vector<Tag> css(std::string_view selector) const;
    std::vector<Tag> css(const CssQuery &query) const;
    std::optional<Tag> css_first(std::string_view selector) const;
    std::optional<Tag> css_first(const CssQuery &query) const;

    std::vector<Tag> xpath(std::string_view expression) const;
    std::vector<Tag> xpath(const XPathQuery &query) const;
    std::vector<std::string> xpath_values(std::string_view expression) const;
    std::vector<std::string> xpath_values(const XPathQuery &query) const;
    std::optional<std::string> xpath_value(std::string_view expression) const;
    std::optional<std::string> xpath_value(const XPathQuery &query) const;

    // Internal: the parsed tag list and the interned custom names, filled by
    // the parser.
    void set_parsed(std::vector<TagData> data,
                    std::vector<std::string> custom_names);

   private:
    struct OwnedTag {};
    struct BorrowedTag {};
    Document(OwnedTag, std::string &&source, std::string_view page_url);
    Document(BorrowedTag, std::string_view source, std::string_view page_url);

    std::string owned_source_;  // filled only when the document owns the text
    std::string page_url_;
    std::string_view source_view_;  // owned_source_ or the borrowed text
    std::vector<TagData> data_;
    std::vector<std::string> custom_names_;
    mutable std::vector<Tag> tags_;
    mutable std::once_flag tags_once_;
    mutable std::shared_ptr<const detail::CssTree> css_tree_;
    mutable std::once_flag css_tree_once_;
  };

  // ---------------------------------------------------------------------------
  // TagQuery
  // ---------------------------------------------------------------------------

  // What a tag must look like, for find/find_all/is. Every criterion that is
  // set must match.
  struct TagQuery {
    // Name criteria.
    int key = -1;           // (int)HtmlTag, -1 = not set
    bool has_name = false;  // a string name is set
    std::string name;       // case-insensitive string name

    // Criteria of the tag itself.
    std::optional<std::string> class_value;
    std::optional<std::string> id;
    std::optional<std::string> text;
    std::optional<Attr> attribute;
    std::optional<std::string> attribute_name;
    std::optional<std::string> attribute_value;
    std::optional<std::string> id_starts_with;
    std::optional<std::string> text_starts_with;
    std::optional<std::string> body;
    std::optional<std::string> body_starts_with;
    std::optional<std::string> source;

    TagQuery() = default;
    explicit TagQuery(HtmlTag name);
    explicit TagQuery(std::string_view name);

    // Chained criteria, for the C# initializer syntax.
    TagQuery &with_class(std::string value) {
      class_value = std::move(value);
      return *this;
    }
    TagQuery &with_id(std::string value) {
      id = std::move(value);
      return *this;
    }
    TagQuery &with_text(std::string value) {
      text = std::move(value);
      return *this;
    }
    TagQuery &with_attribute(Attr attribute_id, std::string value) {
      attribute = attribute_id;
      attribute_value = std::move(value);
      return *this;
    }
    TagQuery &with_attribute(std::string attribute_id, std::string value) {
      attribute_name = std::move(attribute_id);
      attribute_value = std::move(value);
      return *this;
    }

    bool matches(const Tag &tag) const;
  };

  // ---------------------------------------------------------------------------
  // TagView
  // ---------------------------------------------------------------------------

  // What find_all_view returns: the tags a query matches, in document order,
  // read lazily from the flat tag list. It allocates nothing and copies the
  // query, so a temporary query is fine; the document must outlive the view
  // and its iterators. A view is an input range, so it composes with the
  // standard views and algorithms:
  //
  //   for (const Tag &tag :
  //        doc.find_all_view(TagQuery(HtmlTag::A)) | std::views::take(5)) ...
  //
  // Iterators point into the view, so do not copy or move it while iterating
  // (a range-for over a returned temporary is fine: its lifetime is extended).
  class TagView : public std::ranges::view_interface<TagView> {
   public:
    class iterator {
     public:
      using value_type = Tag;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::input_iterator_tag;
      using iterator_category = std::input_iterator_tag;

      iterator() = default;
      iterator(const Document *document,
               const TagQuery *query,
               int index,
               int stop)
          : document_(document), query_(query), index_(index), stop_(stop) {
        skip();
      }

      Tag operator*() const {
        return document_->tag(index_);
      }
      iterator &operator++() {
        index_++;
        skip();
        return *this;
      }
      void operator++(int) {
        ++*this;
      }
      bool operator==(const iterator &) const = default;

     private:
      void skip() {
        if (!document_) return;
        while (index_ < stop_ && !document_->tag(index_).is(*query_)) index_++;
      }

      const Document *document_ = nullptr;
      const TagQuery *query_ = nullptr;
      int index_ = 0;
      int stop_ = 0;
    };

    TagView() = default;
    TagView(const Document &document, const TagQuery &query)
        : document_(&document),
          query_(query),
          stop_(int(document.tag_count())) {}
    // [first, stop) are indices in the document's tag list; stop is where the
    // scope's last descendant ends.
    TagView(const Document &document,
            const TagQuery &query,
            int first,
            int stop)
        : document_(&document), query_(query), first_(first), stop_(stop) {}

    iterator begin() const {
      if (!document_) return {};
      return iterator(document_, &query_, first_, stop_);
    }
    iterator end() const {
      if (!document_) return {};
      return iterator(document_, &query_, stop_, stop_);
    }

   private:
    const Document *document_ = nullptr;
    TagQuery query_;
    int first_ = 0;
    int stop_ = 0;
  };

  // ---------------------------------------------------------------------------
  // CssQuery / XPathQuery
  // ---------------------------------------------------------------------------

  // A compiled CSS selector. Compile once and reuse it on any number of
  // documents. Supported: type, *, #id, .class, attribute selectors ([a], =,
  // ~=, |=, ^=, $=, *=, i/s), combinators (descendant, >, +, ~), selector lists
  // and the pseudo-classes :first-child, :last-child, :only-child,
  // :first-of-type, :last-of-type, :only-of-type, :nth-child(),
  // :nth-last-child(), :nth-of-type(), :nth-last-of-type(), :not(), :is(),
  // :where(), :has(), :empty, :root, :scope, :link, :any-link, :checked,
  // :disabled, :enabled, :required, :optional and :contains("text").
  class CssQuery {
   public:
    static CssQuery compile(std::string_view selector);
    // A compiled selector from the process-wide cache (compiled on first use);
    // the string overloads of css/css_first use this.
    static CssQuery cached(std::string_view selector);

    const std::string &selector() const {
      return selector_;
    }

    std::vector<Tag> select_all(const Document &doc) const;
    std::vector<Tag> select_all(const Tag &scope) const;
    std::optional<Tag> select_first(const Document &doc) const;
    std::optional<Tag> select_first(const Tag &scope) const;
    bool matches(const Tag &tag) const;

   private:
    CssQuery(std::string selector,
             std::shared_ptr<const detail::CssSelectorList> compiled);
    std::string selector_;
    std::shared_ptr<const detail::CssSelectorList> compiled_;
  };

  // A compiled XPath 1.0 expression: all axes except namespace, predicates, |,
  // operators and the core functions, plus ends-with(), lower-case() and
  // upper-case().
  class XPathQuery {
   public:
    static XPathQuery compile(std::string_view expression);
    // A compiled expression from the process-wide cache (compiled on first
    // use); the string overloads of xpath/xpath_value use this.
    static XPathQuery cached(std::string_view expression);

    const std::string &expression() const {
      return expression_;
    }

    std::vector<Tag> select_tags(const Document &doc) const;
    std::vector<Tag> select_tags(const Tag &scope) const;
    std::vector<std::string> select_values(const Document &doc) const;
    std::vector<std::string> select_values(const Tag &scope) const;
    std::optional<std::string> select_value(const Document &doc) const;
    std::optional<std::string> select_value(const Tag &scope) const;

   private:
    XPathQuery(std::string expression,
               std::shared_ptr<const detail::XPathExpr> expr);
    std::string expression_;
    std::shared_ptr<const detail::XPathExpr> compiled_;
  };

  // ---------------------------------------------------------------------------
  // Free functions
  // ---------------------------------------------------------------------------

  // Lower-case name of an enum member.
  std::string_view tag_name(HtmlTag tag);
  std::string_view attr_name(Attr attribute);

  // Whether a tag is an element (not a comment, the doctype or the XML
  // declaration).
  bool is_element(const Tag &tag);
  bool is_comment(const Tag &tag);

  // HTML -> plain text: tags removed, whitespace collapsed, character
  // references decoded. The original: HtmlToTextCore.ClearTags.
  std::string clear_tags(std::string_view html);

  // HTML character references decoded (HTML5 rules). The original:
  // HtmlDecodeCore.HtmlDecode.
  std::string html_decode(std::string_view text, bool in_attribute = false);

  // The lower-case name as a known HtmlTag (case-insensitive), if it is one of
  // them.
  std::optional<HtmlTag> try_get_tag(std::string_view name);

  // One-call searches (the Shortcuts namespace of the original).
  std::optional<Tag> find_by_name(const Document &scope, HtmlTag name);
  std::optional<Tag> find_by_name(const Document &scope, std::string_view name);
  std::optional<Tag> find_by_name(const Tag &scope, HtmlTag name);
  std::optional<Tag> find_by_name(const Tag &scope, std::string_view name);
  std::optional<Tag> find_by_id(const Document &scope, std::string_view id);
  std::optional<Tag> find_by_id(const Tag &scope, std::string_view id);
  std::optional<Tag> find_by_name_class(const Document &scope,
                                        HtmlTag name,
                                        std::string_view class_value);
  std::optional<Tag> find_by_name_class(const Tag &scope,
                                        HtmlTag name,
                                        std::string_view class_value);
  std::vector<Tag> find_all_by_name(const Document &scope, HtmlTag name);
  std::vector<Tag> find_all_by_name(const Tag &scope, HtmlTag name);
  std::vector<Tag> find_all_by_name_class(const Document &scope,
                                          HtmlTag name,
                                          std::string_view class_value);
  std::vector<Tag> find_all_by_name_class(const Tag &scope,
                                          HtmlTag name,
                                          std::string_view class_value);

}  // namespace fh
