// CSS selectors: a port of Core/Css/CssSelectors.cs and
// Core/Css/CssCompiler.cs.
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fh::detail {

  // ---------------------------------------------------------------------------
  // CssTree
  // ---------------------------------------------------------------------------

  namespace {

    std::uint64_t type_key(const TagData &tag) {
      return (std::uint64_t(std::uint32_t(tag.parent + 1)) << 32)
             | (std::uint64_t(std::uint16_t(tag.key + 1)) << 16)
             | std::uint64_t(std::uint16_t(tag.custom + 1));
    }

  }  // namespace

  CssTree::CssTree(const Document &doc) : document(&doc) {
    const auto &data = doc.tag_data();
    const std::size_t count = data.size();

    previous.assign(count, -1);
    next.assign(count, -1);
    position.assign(count, 0);
    from_end.assign(count, 0);
    type_position.assign(count, 0);
    type_from_end.assign(count, 0);

    // per parent (slot = parent index + 1, slot 0: the top level): the last
    // element child so far
    std::vector<int> last_child(count + 1, -1);
    std::vector<int> children(count + 1, 0);

    for (std::size_t i = 0; i < count; i++) {
      const TagData &tag = data[i];
      const Tag handle(&doc, int(i));
      if (!is_element(handle)) continue;

      const std::size_t slot = std::size_t(tag.parent + 1);
      const int previous_index = last_child[slot];
      previous[i] = previous_index;
      if (previous_index >= 0) next[std::size_t(previous_index)] = int(i);
      last_child[slot] = int(i);
      position[i] = ++children[slot];
    }

    for (std::size_t i = 0; i < count; i++) {
      if (position[i] > 0) {
        from_end[i] =
            children[std::size_t(data[i].parent + 1)] - position[i] + 1;
      }
    }
  }

  void CssTree::build_types() const {
    if (types_ready) return;
    types_ready = true;

    const auto &data = document->tag_data();
    const std::size_t count = data.size();
    std::fill(type_position.begin(), type_position.end(), 0);
    std::fill(type_from_end.begin(), type_from_end.end(), 0);

    std::unordered_map<std::uint64_t, int> seen;
    seen.reserve(count * 2);

    for (std::size_t i = 0; i < count; i++) {
      if (position[i] == 0) continue;  // not an element
      int &n = seen[type_key(data[i])];
      type_position[i] = ++n;
    }

    for (std::size_t i = 0; i < count; i++) {
      if (type_position[i] == 0) continue;
      type_from_end[i] = seen[type_key(data[i])] - type_position[i] + 1;
    }
  }

  std::optional<Tag> CssTree::previous_tag(const Tag &tag) const {
    const int index = previous[std::size_t(tag.index())];
    return index < 0 ? std::nullopt : std::optional<Tag>(Tag(document, index));
  }

  std::optional<Tag> CssTree::next_tag(const Tag &tag) const {
    const int index = next[std::size_t(tag.index())];
    return index < 0 ? std::nullopt : std::optional<Tag>(Tag(document, index));
  }

  int CssTree::position_of(const Tag &tag) const {
    return position[std::size_t(tag.index())];
  }
  int CssTree::position_from_end_of(const Tag &tag) const {
    return from_end[std::size_t(tag.index())];
  }

  int CssTree::type_position_of(const Tag &tag) const {
    build_types();
    return type_position[std::size_t(tag.index())];
  }

  int CssTree::type_position_from_end_of(const Tag &tag) const {
    build_types();
    return type_from_end[std::size_t(tag.index())];
  }

  // ---------------------------------------------------------------------------
  // Selector model
  // ---------------------------------------------------------------------------

  enum class CssCombinator : std::uint8_t {
    None,
    Descendant,  // "a b"
    Child,       // "a > b"
    Adjacent,    // "a + b"
    Sibling,     // "a ~ b"
  };

  struct CssContext {
    const Document *document = nullptr;
    std::optional<Tag> scope;
    std::optional<Tag> anchor;

    const CssTree &tree() const {
      return document->css_tree();
    }
    CssContext with_anchor(const Tag &value) const {
      return {document, scope, value};
    }
  };

  struct CssComplex;
  struct CssSelectorList;

  bool matches_complex(const CssComplex &selector,
                       int i,
                       const Tag &tag,
                       const CssContext &context);
  bool has_match(const CssSelectorList &relative,
                 const Tag &anchor,
                 const CssContext &context);

  struct CssSimple {
    virtual ~CssSimple() = default;
    virtual bool matches(const Tag &tag, const CssContext &context) const = 0;
  };

  struct CssCompound {
    int key = -1;           // (int)HtmlTag, or -1
    bool has_name = false;  // a lower-case name not in HtmlTag
    std::string name;
    std::vector<std::unique_ptr<CssSimple>> simples;

    bool matches(const Tag &tag, const CssContext &context) const {
      if (key >= 0) {
        if (tag.key() != key) return false;
      } else if (has_name) {
        if (tag.key() >= 0 || tag.name() != name) return false;
      } else if (!is_element(tag)) {
        return false;
      }

      for (const auto &simple : simples) {
        if (!simple->matches(tag, context)) return false;
      }
      return true;
    }
  };

  struct CssComplex {
    CssCombinator leading = CssCombinator::None;
    std::vector<CssCompound> compounds;
    std::vector<CssCombinator> combinators;
  };

  struct CssSelectorList {
    std::vector<CssComplex> items;

    bool matches(const Tag &tag, const CssContext &context) const {
      for (const CssComplex &item : items) {
        if (matches_complex(item, int(item.compounds.size()) - 1, tag, context))
          return true;
      }
      return false;
    }
  };

  namespace {

    std::optional<Tag> parent_of(const Tag &tag) {
      if (tag.parent_index() < 0) return std::nullopt;
      return Tag(&tag.document(), tag.parent_index());
    }

    bool is_whitespace(char c) {
      return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
    }

    // Whether a whitespace-separated list contains `word`.
    bool contains_word(std::string_view list,
                       std::string_view word,
                       bool case_insensitive) {
      if (word.empty() || list.size() < word.size()) return false;

      while (true) {
        std::size_t start = 0;
        while (start < list.size() && is_whitespace(list[start])) start++;
        if (start == list.size()) return false;

        list = list.substr(start);
        std::size_t end = 0;
        while (end < list.size() && !is_whitespace(list[end])) end++;
        const std::string_view item = list.substr(0, end);

        if (case_insensitive ? iequals(item, word) : item == word) return true;
        if (end >= list.size()) return false;

        list = list.substr(end);
      }
    }

    std::string decoded(std::string_view raw) {
      return needs_decoding(raw) ? decode_attribute(raw) : std::string(raw);
    }

    bool is_related(CssCombinator combinator,
                    const std::optional<Tag> &anchor,
                    const Tag &tag,
                    const CssContext &context) {
      switch (combinator) {
        case CssCombinator::Child:
          return !anchor ? tag.parent_index() < 0
                         : tag.parent_index() == anchor->index();
        case CssCombinator::Descendant:
          if (!anchor) return true;
          for (auto parent = parent_of(tag); parent;
               parent = parent_of(*parent)) {
            if (*parent == *anchor) return true;
          }
          return false;
        case CssCombinator::Adjacent:
          return anchor && context.tree().previous_tag(tag) == anchor;
        case CssCombinator::Sibling:
          return anchor && anchor->index() < tag.index()
                 && anchor->parent_index() == tag.parent_index()
                 && is_element(*anchor);
        default:
          return false;
      }
    }

  }  // namespace

  bool matches_complex(const CssComplex &selector,
                       int i,
                       const Tag &tag,
                       const CssContext &context) {
    if (!selector.compounds[std::size_t(i)].matches(tag, context)) return false;

    if (i == 0)
      return selector.leading == CssCombinator::None
             || is_related(selector.leading, context.anchor, tag, context);

    switch (selector.combinators[std::size_t(i - 1)]) {
      case CssCombinator::Child: {
        const auto parent = parent_of(tag);
        return parent && matches_complex(selector, i - 1, *parent, context);
      }
      case CssCombinator::Descendant: {
        for (auto parent = parent_of(tag); parent;
             parent = parent_of(*parent)) {
          if (matches_complex(selector, i - 1, *parent, context)) return true;
        }
        return false;
      }
      case CssCombinator::Adjacent: {
        const auto previous = context.tree().previous_tag(tag);
        return previous && matches_complex(selector, i - 1, *previous, context);
      }
      case CssCombinator::Sibling: {
        for (auto previous = context.tree().previous_tag(tag); previous;
             previous = context.tree().previous_tag(*previous)) {
          if (matches_complex(selector, i - 1, *previous, context)) return true;
        }
        return false;
      }
      default:
        return false;
    }
  }

  // Whether a tag in the scope matches the selector (:has): the scope is
  // searched from its start.
  bool has_match(const CssSelectorList &relative,
                 const Tag &anchor,
                 const CssContext &context) {
    const Document &document = anchor.document();
    const auto &data = document.tag_data();
    const CssContext inner = context.with_anchor(anchor);

    for (const CssComplex &item : relative.items) {
      const int last = int(item.compounds.size()) - 1;
      int i = anchor.index() + 1;

      if (item.leading == CssCombinator::Descendant
          || item.leading == CssCombinator::Child) {
        // the tags inside the anchor
        for (; i < int(data.size()) && data[std::size_t(i)].end <= anchor.end();
             i++) {
          const Tag handle(&document, i);
          if (is_element(handle) && matches_complex(item, last, handle, inner))
            return true;
        }
      } else {
        // the following siblings and the tags inside them, up to the end of the
        // parent
        while (i < int(data.size()) && data[std::size_t(i)].end <= anchor.end())
          i++;

        const auto parent = parent_of(anchor);
        const int end =
            parent ? parent->end() : std::numeric_limits<int>::max();
        for (; i < int(data.size()) && data[std::size_t(i)].end <= end; i++) {
          const Tag handle(&document, i);
          if (is_element(handle) && matches_complex(item, last, handle, inner))
            return true;
        }
      }
    }

    return false;
  }

  // ---------------------------------------------------------------------------
  // Simple selectors
  // ---------------------------------------------------------------------------

  struct CssId : CssSimple {
    std::string id;
    explicit CssId(std::string value) : id(std::move(value)) {}

    bool matches(const Tag &tag, const CssContext &) const override {
      return attribute_equals(span_attribute_by_name(tag, Attr::Id), id);
    }
  };

  struct CssClass : CssSimple {
    std::string name;
    explicit CssClass(std::string value) : name(std::move(value)) {}

    bool matches(const Tag &tag, const CssContext &) const override {
      const std::string_view raw = span_attribute_by_name(tag, Attr::Class);
      return !raw.empty() && contains_word(decoded(raw), name, false);
    }
  };

  enum class CssAttributeOperator : std::uint8_t {
    Exists,     // [a]
    Equals,     // [a=v]
    Includes,   // [a~=v]
    Dash,       // [a|=v]
    Prefix,     // [a^=v]
    Suffix,     // [a$=v]
    Substring,  // [a*=v]
  };

  struct CssAttribute : CssSimple {
    std::string name;
    CssAttributeOperator op;
    std::string value;
    bool case_insensitive = false;

    CssAttribute(std::string name_,
                 CssAttributeOperator op_,
                 std::string value_,
                 char case_flag)
        : name(std::move(name_)), op(op_), value(std::move(value_)) {
      case_insensitive =
          case_flag == 'i'
          || (case_flag == '\0'
              && case_insensitive_values().count(to_lower(name)) != 0);
    }

    // HTML: the values of these attributes are compared ignoring ASCII case
    // unless the selector has the 's' flag ([type=text] matches type="TEXT").
    static const std::unordered_set<std::string> &case_insensitive_values() {
      static const std::unordered_set<std::string> set = {
          "accept",    "accept-charset", "align",     "alink",    "axis",
          "bgcolor",   "charset",        "checked",   "clear",    "codetype",
          "color",     "compact",        "declare",   "defer",    "dir",
          "direction", "disabled",       "enctype",   "face",     "frame",
          "hreflang",  "http-equiv",     "lang",      "language", "link",
          "media",     "method",         "multiple",  "nohref",   "noresize",
          "noshade",   "nowrap",         "readonly",  "rel",      "rev",
          "rules",     "scope",          "scrolling", "selected", "shape",
          "target",    "text",           "type",      "valign",   "valuetype",
          "vlink",
      };
      return set;
    }

    bool same(std::string_view a, std::string_view b) const {
      return case_insensitive ? iequals(a, b) : a == b;
    }

    bool matches(const Tag &tag, const CssContext &) const override {
      const std::string_view raw = span_attribute_by_name(tag, name);
      if (raw.empty()) {
        // missing or empty: only these can match an empty value
        switch (op) {
          case CssAttributeOperator::Exists:
            return has_attribute(tag, name);
          case CssAttributeOperator::Equals:
          case CssAttributeOperator::Dash:
            return value.empty() && has_attribute(tag, name);
          default:
            return false;
        }
      }

      if (op == CssAttributeOperator::Exists) return true;

      const std::string actual = decoded(raw);

      switch (op) {
        case CssAttributeOperator::Equals:
          return same(actual, value);
        case CssAttributeOperator::Includes:
          return value.find_first_of(" \t\n\f\r") == std::string::npos
                 && contains_word(actual, value, case_insensitive);
        case CssAttributeOperator::Dash:
          return same(actual, value)
                 || (actual.size() > value.size() && actual[value.size()] == '-'
                     && same(actual.substr(0, value.size()), value));
        case CssAttributeOperator::Prefix:
          return !value.empty() && actual.size() >= value.size()
                 && same(actual.substr(0, value.size()), value);
        case CssAttributeOperator::Suffix:
          return !value.empty() && actual.size() >= value.size()
                 && same(actual.substr(actual.size() - value.size()), value);
        case CssAttributeOperator::Substring:
          if (value.empty()) return false;
          return case_insensitive
                     ? ifind(actual, value) != std::string_view::npos
                     : actual.find(value) != std::string::npos;
        default:
          return false;
      }
    }
  };

  struct CssNth : CssSimple {
    int a = 0;
    int b = 0;
    bool from_end = false;
    bool of_type = false;
    std::shared_ptr<const CssSelectorList> of;

    CssNth(int a_,
           int b_,
           bool from_end_,
           bool of_type_,
           std::shared_ptr<const CssSelectorList> of_)
        : a(a_),
          b(b_),
          from_end(from_end_),
          of_type(of_type_),
          of(std::move(of_)) {}

    bool matches(const Tag &tag, const CssContext &context) const override {
      const CssTree &tree = context.tree();
      int position;

      if (of) {
        if (!of->matches(tag, context)) return false;

        position = 1;
        for (auto sibling = step(tree, tag); sibling;
             sibling = step(tree, *sibling)) {
          if (of->matches(*sibling, context)) position++;
        }
      } else if (of_type) {
        position = from_end ? tree.type_position_from_end_of(tag)
                            : tree.type_position_of(tag);
      } else {
        position =
            from_end ? tree.position_from_end_of(tag) : tree.position_of(tag);
      }

      if (position <= 0) return false;

      // position = a*n + b for some n >= 0
      if (a == 0) return position == b;

      const int diff = position - b;
      return diff % a == 0 && diff / a >= 0;
    }

    std::optional<Tag> step(const CssTree &tree, const Tag &tag) const {
      return from_end ? tree.next_tag(tag) : tree.previous_tag(tag);
    }
  };

  struct CssNot : CssSimple {
    std::shared_ptr<const CssSelectorList> list;
    explicit CssNot(std::shared_ptr<const CssSelectorList> value)
        : list(std::move(value)) {}
    bool matches(const Tag &tag, const CssContext &context) const override {
      return !list->matches(tag, context);
    }
  };

  struct CssIs : CssSimple {
    std::shared_ptr<const CssSelectorList> list;
    explicit CssIs(std::shared_ptr<const CssSelectorList> value)
        : list(std::move(value)) {}
    bool matches(const Tag &tag, const CssContext &context) const override {
      return list->matches(tag, context);
    }
  };

  struct CssHas : CssSimple {
    std::shared_ptr<const CssSelectorList> list;
    explicit CssHas(std::shared_ptr<const CssSelectorList> value)
        : list(std::move(value)) {}
    bool matches(const Tag &tag, const CssContext &context) const override {
      return has_match(*list, tag, context);
    }
  };

  struct CssContains : CssSimple {
    std::string text;
    explicit CssContains(std::string value) : text(std::move(value)) {}
    bool matches(const Tag &tag, const CssContext &) const override {
      return tag.text().find(text) != std::string::npos;
    }
  };

  enum class CssPseudoKind : std::uint8_t {
    Root,
    Scope,
    Empty,
    Link,
    Checked,
    Disabled,
    Enabled,
    Required,
    Optional
  };

  struct CssPseudo : CssSimple {
    CssPseudoKind kind;
    explicit CssPseudo(CssPseudoKind value) : kind(value) {}

    bool matches(const Tag &tag, const CssContext &context) const override {
      switch (kind) {
        case CssPseudoKind::Root:
          return is_root(tag, context);
        case CssPseudoKind::Scope:
          return context.scope ? tag == *context.scope : is_root(tag, context);
        case CssPseudoKind::Empty:
          return is_empty(tag);
        case CssPseudoKind::Link:
          return (tag.name() == "a" || tag.name() == "area")
                 && has_attribute(tag, "href");
        case CssPseudoKind::Checked:
          return (tag.name() == "input" && has_attribute(tag, "checked"))
                 || (tag.name() == "option" && has_attribute(tag, "selected"));
        case CssPseudoKind::Disabled:
          return is_form_control(tag) && has_attribute(tag, "disabled");
        case CssPseudoKind::Enabled:
          return is_form_control(tag) && !has_attribute(tag, "disabled");
        case CssPseudoKind::Required:
          return is_input(tag) && has_attribute(tag, "required");
        case CssPseudoKind::Optional:
          return is_input(tag) && !has_attribute(tag, "required");
        default:
          return false;
      }
    }

    static bool is_root(const Tag &tag, const CssContext &context) {
      return tag.parent_index() < 0 && !context.tree().previous_tag(tag);
    }
    static bool is_form_control(const Tag &tag) {
      const std::string_view name = tag.name();
      return name == "button" || name == "input" || name == "select"
             || name == "textarea" || name == "optgroup" || name == "option"
             || name == "fieldset";
    }
    static bool is_input(const Tag &tag) {
      const std::string_view name = tag.name();
      return name == "input" || name == "select" || name == "textarea";
    }
    // No child elements and no text; comments are allowed.
    static bool is_empty(const Tag &tag) {
      if (tag.inner_start() <= 0 || tag.inner_end() <= 0) return true;

      int position = tag.inner_start();
      const auto &data = tag.document().tag_data();
      for (int i = tag.index() + 1;
           i < int(data.size()) && data[std::size_t(i)].end <= tag.end();
           i++) {
        const Tag child(&tag.document(), i);
        if (is_element(child) || child.start() > position) return false;
        position = std::max(position, child.end() + 1);
      }

      return position > tag.inner_end();
    }
  };

  // ---------------------------------------------------------------------------
  // Compiler
  // ---------------------------------------------------------------------------

  class CssCompiler {
   public:
    explicit CssCompiler(std::string_view selector) : s_(selector) {}

    std::shared_ptr<const CssSelectorList> compile() {
      auto list = parse_list(false, true);
      if (std::size_t(p_) < s_.size())
        throw error(std::string("unexpected '") + s_[std::size_t(p_)] + "'");
      return list;
    }

   private:
    std::shared_ptr<CssSelectorList> parse_list(bool relative,
                                                bool top_level = false) {
      auto list = std::make_shared<CssSelectorList>();
      while (true) {
        list->items.push_back(parse_complex(relative, top_level));
        skip_whitespace();
        if (peek() != ',') break;
        p_++;
      }
      return list;
    }

    CssComplex parse_complex(bool relative, bool top_level) {
      skip_whitespace();

      CssComplex complex;
      if (relative || top_level) {
        const CssCombinator combinator = read_combinator_char();
        if (combinator != CssCombinator::None) {
          if (top_level && combinator != CssCombinator::Child) {
            throw error(
                "a selector can start only with '>' (the children of the tag "
                "it is called on)");
          }
          complex.leading = combinator;
        } else if (relative) {
          complex.leading = CssCombinator::Descendant;
        }
      }

      complex.compounds.push_back(parse_compound());

      while (true) {
        const bool whitespace = skip_whitespace();
        const char c = peek();
        if (c == '\0' || c == ',' || c == ')') break;

        CssCombinator combinator = read_combinator_char();
        if (combinator == CssCombinator::None) {
          if (!whitespace) throw error(std::string("unexpected '") + c + "'");
          combinator = CssCombinator::Descendant;
        }

        complex.combinators.push_back(combinator);
        complex.compounds.push_back(parse_compound());
      }

      return complex;
    }

    CssCombinator read_combinator_char() {
      CssCombinator combinator = CssCombinator::None;
      switch (peek()) {
        case '>':
          combinator = CssCombinator::Child;
          break;
        case '+':
          combinator = CssCombinator::Adjacent;
          break;
        case '~':
          combinator = CssCombinator::Sibling;
          break;
        default:
          break;
      }
      if (combinator != CssCombinator::None) {
        p_++;
        skip_whitespace();
      }
      return combinator;
    }

    CssCompound parse_compound() {
      CssCompound compound;
      bool has_type = false;

      if (peek() == '*') {
        p_++;
        has_type = true;
      } else if (is_identifier_start(p_)) {
        std::string name = to_lower(read_identifier());
        has_type = true;
        const int known = lookup_tag_name(name);
        if (known >= 0) {
          compound.key = known;
        } else {
          compound.name = std::move(name);
          compound.has_name = true;
        }
      }

      if (peek() == '|')
        throw error(
            "namespaces are not supported; escape the colon of a prefixed "
            "name: dc\\:creator");

      while (true) {
        switch (peek()) {
          case '#':
            p_++;
            if (!is_name_char(p_)) throw error("an id expected after '#'");
            compound.simples.push_back(std::make_unique<CssId>(read_name()));
            continue;
          case '.':
            p_++;
            if (!is_identifier_start(p_))
              throw error("a class name expected after '.'");
            compound.simples.push_back(
                std::make_unique<CssClass>(read_identifier()));
            continue;
          case '[':
            compound.simples.push_back(parse_attribute());
            continue;
          case ':':
            parse_pseudo(compound.simples);
            continue;
          default:
            break;
        }
        break;
      }

      if (!has_type && compound.simples.empty()) {
        throw error(p_ >= int(s_.size()) ? "a selector expected"
                                         : std::string("unexpected '")
                                               + s_[std::size_t(p_)] + "'");
      }

      return compound;
    }

    std::unique_ptr<CssSimple> parse_attribute() {
      p_++;  // '['
      skip_whitespace();
      if (!is_identifier_start(p_)) throw error("an attribute name expected");
      std::string name = read_identifier();
      skip_whitespace();

      if (peek() == ']') {
        p_++;
        return std::make_unique<CssAttribute>(
            std::move(name), CssAttributeOperator::Exists, "", '\0');
      }

      CssAttributeOperator op;
      const char c = peek();
      if (c == '=') {
        op = CssAttributeOperator::Equals;
        p_++;
      } else {
        switch (c) {
          case '~':
            op = CssAttributeOperator::Includes;
            break;
          case '|':
            op = CssAttributeOperator::Dash;
            break;
          case '^':
            op = CssAttributeOperator::Prefix;
            break;
          case '$':
            op = CssAttributeOperator::Suffix;
            break;
          case '*':
            op = CssAttributeOperator::Substring;
            break;
          default:
            throw error("'=', '~=', '|=', '^=', '$=', '*=' or ']' expected");
        }
        if (peek(1) != '=')
          throw error(std::string("'=' expected after '") + c + "'");
        p_ += 2;
      }

      skip_whitespace();
      std::string value;
      if (peek() == '"' || peek() == '\'') {
        value = read_string();
      } else if (is_identifier_start(p_)) {
        value = read_identifier();
      } else {
        throw error(
            "an attribute value expected (quote it if it is not a plain word)");
      }

      skip_whitespace();
      char case_flag = '\0';
      if (is_identifier_start(p_)) {
        const std::string flag = read_identifier();
        if (iequals(flag, "i")) {
          case_flag = 'i';
        } else if (iequals(flag, "s")) {
          case_flag = 's';
        } else {
          throw error("unknown attribute flag '" + flag + "'");
        }
        skip_whitespace();
      }

      expect(']');
      return std::make_unique<CssAttribute>(
          std::move(name), op, std::move(value), case_flag);
    }

    void parse_pseudo(std::vector<std::unique_ptr<CssSimple>> &simples) {
      p_++;  // ':'
      if (peek() == ':')
        throw error("pseudo-elements (::before, ::text) select no tags");
      if (!is_identifier_start(p_))
        throw error("a pseudo-class name expected after ':'");

      const int start = p_;
      const std::string name = to_lower(read_identifier());

      if (peek() == '(') {
        p_++;
        skip_whitespace();
        if (name == "not") {
          simples.push_back(std::make_unique<CssNot>(parse_list(false)));
        } else if (name == "is" || name == "where" || name == "matches"
                   || name == "any") {
          simples.push_back(std::make_unique<CssIs>(parse_list(false)));
        } else if (name == "has") {
          simples.push_back(std::make_unique<CssHas>(parse_list(true)));
        } else if (name == "nth-child" || name == "nth-last-child"
                   || name == "nth-of-type" || name == "nth-last-of-type") {
          const auto [a, b] = parse_nth();
          const bool of_type = name.ends_with("of-type");
          std::shared_ptr<const CssSelectorList> of;
          skip_whitespace();
          if (!of_type && is_word("of")) {
            p_ += 2;
            of = parse_list(false);
          }
          simples.push_back(std::make_unique<CssNth>(
              a, b, name.find("last") != std::string::npos, of_type, of));
        } else if (name == "contains") {
          std::string text;
          if (peek() == '"' || peek() == '\'') {
            text = read_string();
          } else if (is_identifier_start(p_)) {
            text = read_identifier();
          } else {
            throw error("a string expected in :contains()");
          }
          simples.push_back(std::make_unique<CssContains>(std::move(text)));
        } else {
          p_ = start;
          throw error("unknown pseudo-class ':" + name + "()'");
        }

        skip_whitespace();
        expect(')');
        return;
      }

      if (name == "first-child") {
        simples.push_back(
            std::make_unique<CssNth>(0, 1, false, false, nullptr));
      } else if (name == "last-child") {
        simples.push_back(std::make_unique<CssNth>(0, 1, true, false, nullptr));
      } else if (name == "only-child") {
        simples.push_back(
            std::make_unique<CssNth>(0, 1, false, false, nullptr));
        simples.push_back(std::make_unique<CssNth>(0, 1, true, false, nullptr));
      } else if (name == "first-of-type") {
        simples.push_back(std::make_unique<CssNth>(0, 1, false, true, nullptr));
      } else if (name == "last-of-type") {
        simples.push_back(std::make_unique<CssNth>(0, 1, true, true, nullptr));
      } else if (name == "only-of-type") {
        simples.push_back(std::make_unique<CssNth>(0, 1, false, true, nullptr));
        simples.push_back(std::make_unique<CssNth>(0, 1, true, true, nullptr));
      } else if (name == "root") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Root));
      } else if (name == "scope") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Scope));
      } else if (name == "empty") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Empty));
      } else if (name == "link" || name == "any-link") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Link));
      } else if (name == "checked") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Checked));
      } else if (name == "disabled") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Disabled));
      } else if (name == "enabled") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Enabled));
      } else if (name == "required") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Required));
      } else if (name == "optional") {
        simples.push_back(std::make_unique<CssPseudo>(CssPseudoKind::Optional));
      } else {
        p_ = start;
        throw error("unknown pseudo-class ':" + name + "'");
      }
    }

    // An+B: odd, even, 3, n, -n+3, 2n+1, +2n - 1
    std::pair<int, int> parse_nth() {
      if (is_word("odd")) {
        p_ += 3;
        return {2, 1};
      }
      if (is_word("even")) {
        p_ += 4;
        return {2, 0};
      }

      const int start = p_;
      std::string text;
      while (std::size_t(p_) < s_.size()
             && (s_[std::size_t(p_)] == '+' || s_[std::size_t(p_)] == '-'
                 || s_[std::size_t(p_)] == 'n' || s_[std::size_t(p_)] == 'N'
                 || is_ascii_digit(s_[std::size_t(p_)])
                 || is_whitespace(s_[std::size_t(p_)]))) {
        if (!is_whitespace(s_[std::size_t(p_)]))
          text.push_back(ascii_lower(s_[std::size_t(p_)]));
        p_++;
      }

      const std::size_t n = text.find('n');
      if (n == std::string::npos) {
        int only = 0;
        if (try_int(text, only)) return {0, only};
      } else if (text.find('n', n + 1) == std::string::npos) {
        const std::string a_text = text.substr(0, n);
        const std::string b_text = text.substr(n + 1);
        int a = 0;
        if (a_text.empty() || a_text == "+") {
          a = 1;
        } else if (a_text == "-") {
          a = -1;
        } else if (!try_int(a_text, a)) {
          a = std::numeric_limits<int>::min();
        }

        int b = 0;
        const bool b_ok =
            b_text.empty()
            || ((b_text[0] == '+' || b_text[0] == '-') && b_text.size() > 1
                && is_ascii_digit(b_text[1]) && try_int(b_text, b));
        if (a != std::numeric_limits<int>::min() && b_ok) return {a, b};
      }

      p_ = start;
      throw error("An+B expected (odd, even, 3, 2n+1, -n+3)");
    }

    static bool try_int(const std::string &text, int &value) {
      if (text.empty()) return false;
      std::size_t i = 0;
      bool negative = false;
      if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        i = 1;
      }
      if (i >= text.size()) return false;

      long long result = 0;
      for (; i < text.size(); i++) {
        if (!is_ascii_digit(text[i])) return false;
        result = result * 10 + (text[i] - '0');
        if (result > std::numeric_limits<int>::max()) return false;
      }
      value = int(negative ? -result : result);
      return true;
    }

    char peek(int offset = 0) const {
      return std::size_t(p_ + offset) < s_.size() ? s_[std::size_t(p_ + offset)]
                                                  : '\0';
    }

    static bool is_whitespace(char c) {
      return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    bool skip_whitespace() {
      const int start = p_;
      while (std::size_t(p_) < s_.size() && is_whitespace(s_[std::size_t(p_)]))
        p_++;
      return p_ > start;
    }

    void expect(char c) {
      if (peek() != c) {
        throw error(p_ >= int(s_.size())
                        ? std::string("'") + c + "' expected"
                        : std::string("'") + c + "' expected instead of '"
                              + s_[std::size_t(p_)] + "'");
      }
      p_++;
    }

    bool is_word(std::string_view word) const {
      if (std::size_t(p_) + word.size() > s_.size()) return false;
      if (!iequals(s_.substr(std::size_t(p_), word.size()), word)) return false;
      return !is_name_char(p_ + int(word.size()));
    }

    static bool is_name_start_char(char c) {
      return is_ascii_alpha(c) || c == '_'
             || static_cast<unsigned char>(c) >= 0x80;
    }

    bool is_escape(int p) const {
      const std::size_t index = std::size_t(p);
      return index < s_.size() && s_[index] == '\\'
             && (index + 1 >= s_.size()
                 || !(s_[index + 1] == '\n' || s_[index + 1] == '\r'
                      || s_[index + 1] == '\f'));
    }

    bool is_name_char(int p) const {
      const std::size_t index = std::size_t(p);
      return index < s_.size()
             && (is_name_start_char(s_[index]) || is_ascii_digit(s_[index])
                 || s_[index] == '-' || is_escape(p));
    }

    bool is_identifier_start(int p) const {
      if (std::size_t(p) >= s_.size()) return false;

      const char c = s_[std::size_t(p)];
      if (c == '-') {
        p++;
        return std::size_t(p) < s_.size()
               && (is_name_start_char(s_[std::size_t(p)])
                   || s_[std::size_t(p)] == '-' || is_escape(p));
      }
      return is_name_start_char(c) || is_escape(p);
    }

    std::string read_identifier() {
      return read_name();
    }

    std::string read_name() {
      std::string result;
      while (is_name_char(p_)) {
        if (s_[std::size_t(p_)] == '\\') {
          read_escape(result);
        } else {
          result.push_back(s_[std::size_t(p_++)]);
        }
      }
      return result;
    }

    std::string read_string() {
      const char quote = s_[std::size_t(p_++)];
      const int start = p_ - 1;
      std::string result;
      while (true) {
        if (std::size_t(p_) >= s_.size()) {
          p_ = start;
          throw error("unterminated string");
        }

        const char c = s_[std::size_t(p_)];
        if (c == quote) {
          p_++;
          return result;
        }

        if (c == '\\') {
          if (std::size_t(p_) + 1 < s_.size()
              && (s_[std::size_t(p_) + 1] == '\n'
                  || s_[std::size_t(p_) + 1] == '\f')) {
            p_ += 2;  // an escaped line break continues the string
          } else if (std::size_t(p_) + 2 < s_.size()
                     && s_[std::size_t(p_) + 1] == '\r') {
            p_ += s_[std::size_t(p_) + 2] == '\n' ? 3 : 2;
          } else {
            read_escape(result);
          }
          continue;
        }

        result.push_back(c);
        p_++;
      }
    }

    // '\' then up to 6 hex digits and one optional whitespace, or any other
    // char as itself
    void read_escape(std::string &result) {
      p_++;  // '\'
      if (std::size_t(p_) >= s_.size()) {
        result.append("\xEF\xBF\xBD");
        return;
      }

      const char c = s_[std::size_t(p_)];
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        result.push_back(c);
        p_++;
        return;
      }

      std::uint32_t code = 0;
      int digits = 0;
      while (
          digits < 6 && std::size_t(p_) < s_.size()
          && std::isxdigit(static_cast<unsigned char>(s_[std::size_t(p_)]))) {
        const char h = ascii_lower(s_[std::size_t(p_)]);
        code = code * 16 + std::uint32_t(h >= 'a' ? h - 'a' + 10 : h - '0');
        p_++;
        digits++;
      }

      if (std::size_t(p_) < s_.size() && is_whitespace(s_[std::size_t(p_)])) {
        p_ += (s_[std::size_t(p_)] == '\r' && peek(1) == '\n') ? 2 : 1;
      }

      if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        result.append("\xEF\xBF\xBD");
      } else if (code <= 0x7F) {
        result.push_back(char(code));
      } else if (code <= 0x7FF) {
        result.push_back(char(0xC0 | (code >> 6)));
        result.push_back(char(0x80 | (code & 0x3F)));
      } else if (code <= 0xFFFF) {
        result.push_back(char(0xE0 | (code >> 12)));
        result.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        result.push_back(char(0x80 | (code & 0x3F)));
      } else {
        result.push_back(char(0xF0 | (code >> 18)));
        result.push_back(char(0x80 | ((code >> 12) & 0x3F)));
        result.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        result.push_back(char(0x80 | (code & 0x3F)));
      }
    }

    std::invalid_argument error(const std::string &message) const {
      return std::invalid_argument("Invalid CSS selector \"" + std::string(s_)
                                   + "\" at position " + std::to_string(p_)
                                   + ": " + message + ".");
    }

    std::string_view s_;
    int p_ = 0;
  };

}  // namespace fh::detail

namespace fh {

  CssQuery::CssQuery(std::string selector,
                     std::shared_ptr<const detail::CssSelectorList> compiled)
      : selector_(std::move(selector)), compiled_(std::move(compiled)) {}

  CssQuery CssQuery::compile(std::string_view selector) {
    detail::CssCompiler compiler(selector);
    auto compiled = compiler.compile();
    return CssQuery(std::string(selector), std::move(compiled));
  }

  CssQuery CssQuery::cached(std::string_view selector) {
    static std::mutex mutex;
    static std::unordered_map<std::string, CssQuery> cache;

    std::lock_guard<std::mutex> lock(mutex);
    const std::string key(selector);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    CssQuery query = compile(selector);
    if (cache.size() >= 1024)
      cache.clear();  // dynamically built selectors must not grow it forever
    cache.emplace(key, query);
    return query;
  }

  std::vector<Tag> CssQuery::select_all(const Document &doc) const {
    std::vector<Tag> result;
    const detail::CssContext context{&doc, std::nullopt, std::nullopt};

    const auto &data = doc.tag_data();
    for (int i = 0; i < int(data.size()); i++) {
      const Tag tag(&doc, i);
      if (compiled_->matches(tag, context)) result.push_back(tag);
    }
    return result;
  }

  std::vector<Tag> CssQuery::select_all(const Tag &scope) const {
    std::vector<Tag> result;
    if (!scope.valid()) return result;

    const Document &doc = scope.document();
    const detail::CssContext context{&doc, scope, scope};

    const auto &data = doc.tag_data();
    for (int i = scope.index() + 1; i < int(data.size()); i++) {
      if (data[std::size_t(i)].end > scope.end()) break;
      const Tag tag(&doc, i);
      if (compiled_->matches(tag, context)) result.push_back(tag);
    }
    return result;
  }

  std::optional<Tag> CssQuery::select_first(const Document &doc) const {
    const detail::CssContext context{&doc, std::nullopt, std::nullopt};
    const auto &data = doc.tag_data();
    for (int i = 0; i < int(data.size()); i++) {
      const Tag tag(&doc, i);
      if (compiled_->matches(tag, context)) return tag;
    }
    return std::nullopt;
  }

  std::optional<Tag> CssQuery::select_first(const Tag &scope) const {
    if (!scope.valid()) return std::nullopt;

    const Document &doc = scope.document();
    const detail::CssContext context{&doc, scope, scope};
    const auto &data = doc.tag_data();
    for (int i = scope.index() + 1; i < int(data.size()); i++) {
      if (data[std::size_t(i)].end > scope.end()) break;
      const Tag tag(&doc, i);
      if (compiled_->matches(tag, context)) return tag;
    }
    return std::nullopt;
  }

  bool CssQuery::matches(const Tag &tag) const {
    if (!tag.valid() || !is_element(tag)) return false;
    const detail::CssContext context{&tag.document(), tag, tag};
    return compiled_->matches(tag, context);
  }

}  // namespace fh
