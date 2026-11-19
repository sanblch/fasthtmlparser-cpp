// XPath 1.0: a port of Core/XPath/*.cs (XPathNode, XPathCompiler,
// XPathExpressions).
#include "internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace fh::detail {

  // ---------------------------------------------------------------------------
  // XNode
  // ---------------------------------------------------------------------------

  enum class XNodeKind : std::uint8_t {
    Root,
    Element,
    Comment,
    Text,
    Attribute
  };

  struct XNode {
    XNodeKind kind = XNodeKind::Root;
    const Document *document = nullptr;
    Tag tag;  // element / comment: the tag; text / attribute: the parent
              // element
    int start = 0;
    int length = 0;
    int value_start = 0;
    int value_length = 0;

    static XNode root(const Document &doc) {
      XNode node;
      node.kind = XNodeKind::Root;
      node.document = &doc;
      return node;
    }

    static XNode from_tag(const Tag &value) {
      XNode node;
      node.kind = value.key() == int(HtmlTag::Comment) ? XNodeKind::Comment
                                                       : XNodeKind::Element;
      node.document = &value.document();
      node.tag = value;
      return node;
    }

    static XNode text(const Document &doc,
                      const Tag &parent,
                      int text_start,
                      int text_length) {
      XNode node;
      node.kind = XNodeKind::Text;
      node.document = &doc;
      node.tag = parent;
      node.start = text_start;
      node.length = text_length;
      return node;
    }

    static XNode attribute(const Tag &owner,
                           int name_start,
                           int name_length,
                           int attribute_value_start,
                           int attribute_value_length) {
      XNode node;
      node.kind = XNodeKind::Attribute;
      node.document = &owner.document();
      node.tag = owner;
      node.start = name_start;
      node.length = name_length;
      node.value_start = attribute_value_start;
      node.value_length = attribute_value_length;
      return node;
    }

    std::int64_t key() const {
      switch (kind) {
        case XNodeKind::Root:
          return -1;
        case XNodeKind::Element:
        case XNodeKind::Comment:
          return std::int64_t(tag.start()) << 32;
        case XNodeKind::Attribute:
          return (std::int64_t(tag.start()) << 32) | std::uint32_t(start);
        default:
          return std::int64_t(start) << 32;
      }
    }

    int range_start() const {
      if (kind == XNodeKind::Root) return -1;
      if (kind == XNodeKind::Text) return start;
      return tag.start();
    }

    int range_end() const {
      switch (kind) {
        case XNodeKind::Root:
          return std::numeric_limits<int>::max();
        case XNodeKind::Element:
        case XNodeKind::Comment:
          return tag.end();
        case XNodeKind::Text:
          return start + length - 1;
        default:
          return tag
              .start();  // attribute: the children of its element follow it
      }
    }

    std::string_view attribute_name() const {
      return std::string_view(document->source())
          .substr(std::size_t(start), std::size_t(length));
    }

    std::string_view attribute_raw_value() const {
      if (value_start < 0) return {};
      return std::string_view(document->source())
          .substr(std::size_t(value_start), std::size_t(value_length));
    }
  };

  // ---------------------------------------------------------------------------
  // NodeTest
  // ---------------------------------------------------------------------------

  enum class TestKind : std::uint8_t {
    Name,
    Prefix,     // dc:*
    Wildcard,   // *
    Node,       // node()
    Text,       // text()
    Comment,    // comment()
    Pi,         // processing-instruction(): never matches
    Container,  // internal: the root and elements
  };

  struct NodeTest {
    TestKind kind = TestKind::Node;
    std::string name;
    TagQuery query;

    NodeTest() = default;
    NodeTest(TestKind kind_, std::string name_)
        : kind(kind_), name(std::move(name_)) {
      if (kind == TestKind::Name) query = TagQuery(name);
    }

    static const NodeTest &any_node() {
      static const NodeTest test(TestKind::Node, {});
      return test;
    }
    static const NodeTest &containers() {
      static const NodeTest test(TestKind::Container, {});
      return test;
    }

    bool matches_text() const {
      return kind == TestKind::Node || kind == TestKind::Text;
    }
    bool matches_tags() const {
      return kind == TestKind::Name || kind == TestKind::Prefix
             || kind == TestKind::Wildcard || kind == TestKind::Node
             || kind == TestKind::Comment || kind == TestKind::Container;
    }

    bool matches_tag(const Tag &tag) const {
      switch (kind) {
        case TestKind::Name:
          return query.matches(tag);
        case TestKind::Prefix:
          return is_element(tag) && tag.name().size() >= name.size()
                 && iequals(tag.name().substr(0, name.size()), name);
        case TestKind::Wildcard:
        case TestKind::Container:
          return is_element(tag);
        case TestKind::Node:
          return is_element(tag) || is_comment(tag);
        case TestKind::Comment:
          return is_comment(tag);
        default:
          return false;
      }
    }

    bool matches_attribute(std::string_view attribute) const {
      switch (kind) {
        case TestKind::Name:
          return iequals(attribute, name);
        case TestKind::Prefix:
          return attribute.size() >= name.size()
                 && iequals(attribute.substr(0, name.size()), name);
        case TestKind::Wildcard:
        case TestKind::Node:
          return true;
        default:
          return false;
      }
    }

    bool matches_node(const XNode &node, bool attribute_axis) const {
      switch (node.kind) {
        case XNodeKind::Root:
          return kind == TestKind::Node || kind == TestKind::Container;
        case XNodeKind::Element:
        case XNodeKind::Comment:
          return matches_tag(node.tag);
        case XNodeKind::Text:
          return matches_text();
        default:
          return attribute_axis ? matches_attribute(node.attribute_name())
                                : kind == TestKind::Node;
      }
    }
  };

  // ---------------------------------------------------------------------------
  // XNav: tree navigation over the flat tag list
  // ---------------------------------------------------------------------------

  namespace {

    bool try_get_content(const XNode &node,
                         int &first,
                         int &end,
                         int &child_depth,
                         int &text_start,
                         int &text_end) {
      if (node.kind == XNodeKind::Root) {
        first = 0;
        end = std::numeric_limits<int>::max();
        child_depth = 0;
        text_start = 0;
        text_end = int(node.document->source().size());
        return true;
      }

      if (node.kind == XNodeKind::Element) {
        const Tag &tag = node.tag;
        if (tag.inner_start() > 0 && tag.inner_end() > 0) {
          first = tag.index() + 1;
          end = tag.end();
          child_depth = tag.depth() + 1;
          text_start = tag.inner_start();
          text_end = tag.inner_end() + 1;
          return true;
        }
      }

      first = end = child_depth = text_start = text_end = 0;
      return false;
    }

    void add_text(const Document &document,
                  const Tag &parent,
                  int start,
                  int end,
                  std::vector<XNode> &output) {
      if (end > start)
        output.push_back(XNode::text(document, parent, start, end - start));
    }

    void add_children(const XNode &node,
                      const NodeTest &test,
                      std::vector<XNode> &output) {
      const bool want_text = test.matches_text();
      const bool want_tags = test.matches_tags();
      if (!want_text && !want_tags) return;

      int first, end, child_depth, p, text_end;
      if (!try_get_content(node, first, end, child_depth, p, text_end)) return;

      const Document &document = *node.document;
      const auto &data = document.tag_data();
      const Tag parent = node.kind == XNodeKind::Element ? node.tag : Tag();

      for (int i = first; i < int(data.size()); i++) {
        const TagData &tag = data[std::size_t(i)];
        if (tag.end > end) break;
        if (tag.depth != child_depth) continue;

        const Tag handle(&document, i);
        if (want_text && tag.start > p)
          add_text(document, parent, p, std::min(tag.start, text_end), output);
        if (want_tags && test.matches_tag(handle))
          output.push_back(XNode::from_tag(handle));

        p = tag.end + 1;
      }

      if (want_text && text_end > p)
        add_text(document, parent, p, text_end, output);
    }

    void add_descendants_with_text(const XNode &node,
                                   const NodeTest &test,
                                   std::vector<XNode> &output) {
      std::vector<XNode> children;
      add_children(node, NodeTest::any_node(), children);

      for (const XNode &child : children) {
        if (test.matches_node(child, false)) output.push_back(child);
        if (child.kind == XNodeKind::Element)
          add_descendants_with_text(child, test, output);
      }
    }

    void add_descendants(const XNode &node,
                         const NodeTest &test,
                         std::vector<XNode> &output) {
      if (test.matches_text()) {
        add_descendants_with_text(node, test, output);
        return;
      }

      int first, end, child_depth, text_start, text_end;
      if (!test.matches_tags()
          || !try_get_content(
              node, first, end, child_depth, text_start, text_end))
        return;

      const Document &document = *node.document;
      const auto &data = document.tag_data();
      for (int i = first; i < int(data.size()); i++) {
        const TagData &tag = data[std::size_t(i)];
        if (tag.end > end) break;

        const Tag handle(&document, i);
        if (test.matches_tag(handle)) output.push_back(XNode::from_tag(handle));
      }
    }

    bool try_get_parent(const XNode &node, XNode &parent) {
      switch (node.kind) {
        case XNodeKind::Root:
          return false;
        case XNodeKind::Text:
        case XNodeKind::Attribute:
          parent = node.tag.valid() ? XNode::from_tag(node.tag)
                                    : XNode::root(*node.document);
          return true;
        default:
          parent = node.tag.depth() > 0 && node.tag.parent_index() >= 0
                       ? XNode::from_tag(*node.tag.parent())
                       : XNode::root(*node.document);
          return true;
      }
    }

    void add_attributes(const XNode &node,
                        const NodeTest &test,
                        std::vector<XNode> &output) {
      if (node.kind != XNodeKind::Element) return;

      const Tag &tag = node.tag;
      AttributeScanner scanner(tag);
      const std::string_view source = tag.document().source();
      int ns = 0, nl = 0, vs = 0, vl = 0;
      while (scanner.next(ns, nl, vs, vl)) {
        if (test.matches_attribute(
                source.substr(std::size_t(ns), std::size_t(nl))))
          output.push_back(XNode::attribute(tag, ns, nl, vs, vl));
      }
    }

    bool try_get_attribute(const XNode &node,
                           std::string_view name,
                           std::string_view &raw) {
      raw = {};
      if (node.kind != XNodeKind::Element) return false;

      const Tag &tag = node.tag;
      AttributeScanner scanner(tag);
      const std::string_view source = tag.document().source();
      int ns = 0, nl = 0, vs = 0, vl = 0;
      while (scanner.next(ns, nl, vs, vl)) {
        if (iequals(source.substr(std::size_t(ns), std::size_t(nl)), name)) {
          raw = vs < 0 ? std::string_view{}
                       : source.substr(std::size_t(vs), std::size_t(vl));
          return true;
        }
      }
      return false;
    }

    std::string string_value(const XNode &node) {
      switch (node.kind) {
        case XNodeKind::Root:
          return clear_tags(node.document->source());
        case XNodeKind::Element:
          return node.tag.text();
        case XNodeKind::Attribute:
          return node.value_start < 0
                     ? ""
                     : decode_attribute(node.attribute_raw_value());
        case XNodeKind::Text: {
          const std::string_view text =
              std::string_view(node.document->source())
                  .substr(std::size_t(node.start), std::size_t(node.length));
          if (node.tag.valid() && is_raw_text(node.tag.key()))
            return normalize_newlines(text);
          return decode_html(text);
        }
        default: {  // comment
          const Tag &tag = node.tag;
          std::string_view source =
              std::string_view(tag.document().source())
                  .substr(std::size_t(tag.start()),
                          std::size_t(tag.end() - tag.start() + 1));
          if (source.starts_with("<!--")) source = source.substr(4);
          if (source.ends_with("-->"))
            source = source.substr(0, source.size() - 3);
          return std::string(source);
        }
      }
    }

    std::string node_name(const XNode &node) {
      switch (node.kind) {
        case XNodeKind::Element:
          return std::string(node.tag.name());
        case XNodeKind::Attribute:
          return to_lower(node.attribute_name());
        default:
          return {};
      }
    }

  }  // namespace

  // ---------------------------------------------------------------------------
  // Values and expressions
  // ---------------------------------------------------------------------------

  enum class XType : std::uint8_t { NodeSet, Number, String, Boolean };

  struct EvalState {
    const Document *document = nullptr;
    mutable std::optional<std::vector<XNode>> all;

    const std::vector<XNode> &all_nodes() const {
      if (!all) {
        all.emplace();
        add_descendants(XNode::root(*document), NodeTest::any_node(), *all);
      }
      return *all;
    }
  };

  struct Ctx {
    XNode node;
    int position = 1;
    int size = 1;
    EvalState *state = nullptr;
  };

  struct NodeSet {
    std::vector<XNode> nodes;
  };

  using Value =
      std::variant<std::monostate, bool, double, std::string, NodeSet>;

  namespace {

    bool to_boolean(const Value &value) {
      if (const bool *b = std::get_if<bool>(&value)) return *b;
      if (const double *d = std::get_if<double>(&value))
        return *d != 0 && !std::isnan(*d);
      if (const std::string *s = std::get_if<std::string>(&value))
        return !s->empty();
      if (const NodeSet *n = std::get_if<NodeSet>(&value))
        return !n->nodes.empty();
      return false;
    }

    double string_to_number(std::string_view value) {
      const std::string_view s = trim(value);
      if (s.empty() || s[0] == '+') return std::nan("");

      const char *begin = s.data();
      const char *end = s.data() + s.size();
      double result = 0;
      const auto [ptr, ec] = std::from_chars(begin, end, result);
      if (ec != std::errc() || ptr != end) return std::nan("");
      return result;
    }

    double to_number(const Value &value) {
      if (const double *d = std::get_if<double>(&value)) return *d;
      if (const bool *b = std::get_if<bool>(&value)) return *b ? 1 : 0;
      if (const std::string *s = std::get_if<std::string>(&value))
        return string_to_number(*s);
      if (const NodeSet *n = std::get_if<NodeSet>(&value)) {
        return n->nodes.empty() ? std::nan("")
                                : string_to_number(string_value(n->nodes[0]));
      }
      return std::nan("");
    }

    std::string number_to_string(double d) {
      if (std::isnan(d)) return "NaN";
      if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
      if (d == 0) return "0";

      char buffer[64];
      const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), d);
      std::string s(buffer, ptr);
      if (ec != std::errc()) return s;

      const std::size_t e = s.find_first_of("eE");
      if (e == std::string::npos) return s;

      const int exponent = std::stoi(s.substr(e + 1));
      std::string mantissa = s.substr(0, e);
      const bool negative = !mantissa.empty() && mantissa[0] == '-';
      if (negative) mantissa = mantissa.substr(1);

      const std::size_t point = mantissa.find('.');
      std::string digits =
          point == std::string::npos
              ? mantissa
              : mantissa.substr(0, point) + mantissa.substr(point + 1);
      const int point_at =
          int(point == std::string::npos ? mantissa.size() : point) + exponent;

      std::string result;
      if (point_at <= 0) {
        result = "0." + std::string(std::size_t(-point_at), '0') + digits;
      } else if (std::size_t(point_at) >= digits.size()) {
        result =
            digits + std::string(std::size_t(point_at) - digits.size(), '0');
      } else {
        result = digits.substr(0, std::size_t(point_at)) + "."
                 + digits.substr(std::size_t(point_at));
      }

      return negative ? "-" + result : result;
    }

    std::string to_string(const Value &value) {
      if (const std::string *s = std::get_if<std::string>(&value)) return *s;
      if (const bool *b = std::get_if<bool>(&value))
        return *b ? "true" : "false";
      if (const double *d = std::get_if<double>(&value))
        return number_to_string(*d);
      if (const NodeSet *n = std::get_if<NodeSet>(&value)) {
        return n->nodes.empty() ? "" : string_value(n->nodes[0]);
      }
      return {};
    }

    // Document order, no duplicates.
    void sort_unique(std::vector<XNode> &nodes) {
      if (nodes.size() < 2) return;

      bool sorted = true;
      for (std::size_t i = 1; i < nodes.size(); i++) {
        if (nodes[i - 1].key() >= nodes[i].key()) {
          sorted = false;
          break;
        }
      }
      if (sorted) return;

      std::sort(nodes.begin(), nodes.end(), [](const XNode &a, const XNode &b) {
        return a.key() < b.key();
      });

      std::size_t w = 1;
      for (std::size_t i = 1; i < nodes.size(); i++) {
        if (nodes[i].key() != nodes[w - 1].key()) nodes[w++] = nodes[i];
      }
      nodes.resize(w);
    }

  }  // namespace

  struct XPathExpr {
    virtual ~XPathExpr() = default;
    virtual XType type() const = 0;
    virtual bool uses_position() const {
      return false;
    }
    virtual Value eval(const Ctx &ctx) const = 0;

    virtual bool eval_bool(const Ctx &ctx) const {
      return to_boolean(eval(ctx));
    }
    double eval_number(const Ctx &ctx) const {
      return to_number(eval(ctx));
    }
    std::string eval_string(const Ctx &ctx) const {
      return to_string(eval(ctx));
    }

    NodeSet eval_nodes(const Ctx &ctx) const {
      Value value = eval(ctx);
      if (NodeSet *set = std::get_if<NodeSet>(&value)) return std::move(*set);
      throw std::logic_error("The XPath expression does not give a node-set.");
    }

    // A predicate: a number compares with the position, anything else is
    // converted to boolean.
    bool eval_predicate(const Ctx &ctx) const {
      if (type() == XType::Boolean) return eval_bool(ctx);
      Value value = eval(ctx);
      if (double *d = std::get_if<double>(&value)) return *d == ctx.position;
      return to_boolean(value);
    }

    bool is_positional() const {
      return type() == XType::Number || uses_position();
    }
  };

  namespace {

    struct LiteralExpr : XPathExpr {
      std::string value;
      explicit LiteralExpr(std::string value_) : value(std::move(value_)) {}
      XType type() const override {
        return XType::String;
      }
      Value eval(const Ctx &) const override {
        return value;
      }
    };

    struct NumberExpr : XPathExpr {
      double value;
      explicit NumberExpr(double value_) : value(value_) {}
      XType type() const override {
        return XType::Number;
      }
      Value eval(const Ctx &) const override {
        return value;
      }
    };

    struct OrExpr : XPathExpr {
      std::shared_ptr<XPathExpr> left, right;
      OrExpr(std::shared_ptr<XPathExpr> l, std::shared_ptr<XPathExpr> r)
          : left(std::move(l)), right(std::move(r)) {}
      XType type() const override {
        return XType::Boolean;
      }
      bool uses_position() const override {
        return left->uses_position() || right->uses_position();
      }
      Value eval(const Ctx &ctx) const override {
        return eval_bool(ctx);
      }
      bool eval_bool(const Ctx &ctx) const override {
        return left->eval_bool(ctx) || right->eval_bool(ctx);
      }
    };

    struct AndExpr : XPathExpr {
      std::shared_ptr<XPathExpr> left, right;
      AndExpr(std::shared_ptr<XPathExpr> l, std::shared_ptr<XPathExpr> r)
          : left(std::move(l)), right(std::move(r)) {}
      XType type() const override {
        return XType::Boolean;
      }
      bool uses_position() const override {
        return left->uses_position() || right->uses_position();
      }
      Value eval(const Ctx &ctx) const override {
        return eval_bool(ctx);
      }
      bool eval_bool(const Ctx &ctx) const override {
        return left->eval_bool(ctx) && right->eval_bool(ctx);
      }
    };

    enum class CompareOp : std::uint8_t { Eq, Neq, Lt, Le, Gt, Ge };
    enum class ArithOp : std::uint8_t { Add, Sub, Mul, Div, Mod };

    bool compare_atoms(CompareOp op, const Value &a, const Value &b) {
      if (op == CompareOp::Eq || op == CompareOp::Neq) {
        bool equal;
        const bool a_bool = std::holds_alternative<bool>(a);
        const bool b_bool = std::holds_alternative<bool>(b);
        const bool a_double = std::holds_alternative<double>(a);
        const bool b_double = std::holds_alternative<double>(b);

        if (a_bool || b_bool) {
          equal = to_boolean(a) == to_boolean(b);
        } else if (a_double || b_double) {
          equal = to_number(a) == to_number(b);
        } else {
          equal = to_string(a) == to_string(b);
        }
        return op == CompareOp::Eq ? equal : !equal;
      }

      const double x = to_number(a);
      const double y = to_number(b);
      switch (op) {
        case CompareOp::Lt:
          return x < y;
        case CompareOp::Le:
          return x <= y;
        case CompareOp::Gt:
          return x > y;
        default:
          return x >= y;
      }
    }

    bool compare_values(CompareOp op, const Value &a, const Value &b) {
      const NodeSet *nodes_a = std::get_if<NodeSet>(&a);
      const NodeSet *nodes_b = std::get_if<NodeSet>(&b);

      if (nodes_a) {
        if (nodes_b) {
          if (nodes_a->nodes.empty() || nodes_b->nodes.empty()) return false;

          for (const XNode &node : nodes_a->nodes) {
            const std::string value = string_value(node);
            for (const XNode &other : nodes_b->nodes) {
              if (compare_atoms(op, value, string_value(other))) return true;
            }
          }
          return false;
        }

        // nodes on the left, an atom on the right
        if (std::holds_alternative<bool>(b)) {
          const Value as_bool = !nodes_a->nodes.empty();
          return compare_atoms(op, as_bool, b);
        }
        for (const XNode &node : nodes_a->nodes) {
          Value value = string_value(node);
          if (std::holds_alternative<double>(b)) value = to_number(value);
          if (compare_atoms(op, value, b)) return true;
        }
        return false;
      }

      if (nodes_b) {
        if (std::holds_alternative<bool>(a)) {
          const Value as_bool = !nodes_b->nodes.empty();
          return compare_atoms(op, a, as_bool);
        }
        for (const XNode &node : nodes_b->nodes) {
          Value value = string_value(node);
          if (std::holds_alternative<double>(a)) value = to_number(value);
          if (compare_atoms(op, a, value)) return true;
        }
        return false;
      }

      return compare_atoms(op, a, b);
    }

    struct CompareExpr : XPathExpr {
      CompareOp op;
      std::shared_ptr<XPathExpr> left, right;

      CompareExpr(CompareOp op_,
                  std::shared_ptr<XPathExpr> l,
                  std::shared_ptr<XPathExpr> r)
          : op(op_), left(std::move(l)), right(std::move(r)) {}

      XType type() const override {
        return XType::Boolean;
      }
      bool uses_position() const override {
        return left->uses_position() || right->uses_position();
      }
      Value eval(const Ctx &ctx) const override {
        return eval_bool(ctx);
      }
      bool eval_bool(const Ctx &ctx) const override {
        return compare_values(op, left->eval(ctx), right->eval(ctx));
      }
    };

    struct ArithExpr : XPathExpr {
      ArithOp op;
      std::shared_ptr<XPathExpr> left, right;

      ArithExpr(ArithOp op_,
                std::shared_ptr<XPathExpr> l,
                std::shared_ptr<XPathExpr> r)
          : op(op_), left(std::move(l)), right(std::move(r)) {}

      XType type() const override {
        return XType::Number;
      }
      bool uses_position() const override {
        return left->uses_position() || right->uses_position();
      }

      Value eval(const Ctx &ctx) const override {
        const double x = left->eval_number(ctx);
        const double y = right->eval_number(ctx);
        switch (op) {
          case ArithOp::Add:
            return x + y;
          case ArithOp::Sub:
            return x - y;
          case ArithOp::Mul:
            return x * y;
          case ArithOp::Div:
            return x / y;
          default:
            return std::fmod(x, y);
        }
      }
    };

    struct NegateExpr : XPathExpr {
      std::shared_ptr<XPathExpr> operand;
      explicit NegateExpr(std::shared_ptr<XPathExpr> value)
          : operand(std::move(value)) {}
      XType type() const override {
        return XType::Number;
      }
      bool uses_position() const override {
        return operand->uses_position();
      }
      Value eval(const Ctx &ctx) const override {
        return -operand->eval_number(ctx);
      }
    };

    struct UnionExpr : XPathExpr {
      std::shared_ptr<XPathExpr> left, right;
      UnionExpr(std::shared_ptr<XPathExpr> l, std::shared_ptr<XPathExpr> r)
          : left(std::move(l)), right(std::move(r)) {}
      XType type() const override {
        return XType::NodeSet;
      }
      bool uses_position() const override {
        return left->uses_position() || right->uses_position();
      }

      Value eval(const Ctx &ctx) const override {
        NodeSet result;
        result.nodes = left->eval_nodes(ctx).nodes;
        const NodeSet right_nodes = right->eval_nodes(ctx);
        result.nodes.insert(result.nodes.end(),
                            right_nodes.nodes.begin(),
                            right_nodes.nodes.end());
        sort_unique(result.nodes);
        return result;
      }
    };

    // @name = 'literal': compares the attribute in place, without building a node-set.
    struct AttributeEqualsExpr : XPathExpr {
      std::string name;
      std::string value;
      bool negate = false;

      AttributeEqualsExpr(std::string name_, std::string value_, bool negate_)
          : name(std::move(name_)), value(std::move(value_)), negate(negate_) {}

      XType type() const override {
        return XType::Boolean;
      }
      Value eval(const Ctx &ctx) const override {
        return eval_bool(ctx);
      }

      bool eval_bool(const Ctx &ctx) const override {
        std::string_view raw;
        if (!try_get_attribute(ctx.node, name, raw)) return false;
        return attribute_equals(raw, value) != negate;
      }
    };

    // [@name]: whether the element has the attribute.
    struct AttributeExistsExpr : XPathExpr {
      std::string name;
      explicit AttributeExistsExpr(std::string value)
          : name(std::move(value)) {}
      XType type() const override {
        return XType::Boolean;
      }
      Value eval(const Ctx &ctx) const override {
        return eval_bool(ctx);
      }
      bool eval_bool(const Ctx &ctx) const override {
        std::string_view raw;
        return try_get_attribute(ctx.node, name, raw);
      }
    };

    enum class Axis : std::uint8_t {
      Child,
      Descendant,
      DescendantOrSelf,
      Self,
      Parent,
      Ancestor,
      AncestorOrSelf,
      FollowingSibling,
      PrecedingSibling,
      Following,
      Preceding,
      Attribute,
    };

    void filter_predicates(
        std::vector<XNode> &nodes,
        const std::vector<std::shared_ptr<XPathExpr>> &predicates,
        EvalState &state) {
      for (const auto &predicate : predicates) {
        const int size = int(nodes.size());
        int w = 0;
        for (int i = 0; i < size; i++) {
          if (predicate->eval_predicate(
                  Ctx{nodes[std::size_t(i)], i + 1, size, &state}))
            nodes[std::size_t(w++)] = nodes[std::size_t(i)];
        }
        nodes.resize(std::size_t(w));
      }
    }

    struct Step {
      Axis axis = Axis::Child;
      NodeTest test;
      std::vector<std::shared_ptr<XPathExpr>> predicates;

      bool is_reverse() const {
        return axis == Axis::Ancestor || axis == Axis::AncestorOrSelf
               || axis == Axis::Preceding || axis == Axis::PrecedingSibling;
      }

      bool has_positional_predicates() const {
        for (const auto &predicate : predicates) {
          if (predicate->is_positional()) return true;
        }
        return false;
      }

      std::vector<XNode> apply(const std::vector<XNode> &input,
                               EvalState &state) const {
        std::vector<XNode> result;

        for (const XNode &node : input) {
          std::vector<XNode> buffer;
          collect(node, buffer, state);
          select(0, buffer, state);
          result.insert(result.end(), buffer.begin(), buffer.end());
        }

        sort_unique(result);
        return result;
      }

      // The nodes of the axis that pass the test, in axis order (reverse axes:
      // nearest first).
      void collect(const XNode &node,
                   std::vector<XNode> &output,
                   EvalState &state) const {
        switch (axis) {
          case Axis::Child:
            add_children(node, test, output);
            break;
          case Axis::Descendant:
            add_descendants(node, test, output);
            break;
          case Axis::DescendantOrSelf:
            if (test.matches_node(node, false)) output.push_back(node);
            add_descendants(node, test, output);
            break;
          case Axis::Self:
            if (test.matches_node(node, false)) output.push_back(node);
            break;
          case Axis::Parent: {
            XNode parent;
            if (try_get_parent(node, parent)
                && test.matches_node(parent, false))
              output.push_back(parent);
            break;
          }
          case Axis::Ancestor:
          case Axis::AncestorOrSelf: {
            XNode current = node;
            if (axis == Axis::AncestorOrSelf
                && test.matches_node(current, false))
              output.push_back(current);
            XNode parent;
            while (try_get_parent(current, parent)) {
              current = parent;
              if (test.matches_node(current, false)) output.push_back(current);
            }
            break;
          }
          case Axis::FollowingSibling:
          case Axis::PrecedingSibling:
            collect_siblings(
                node, test, output, axis == Axis::FollowingSibling);
            break;
          case Axis::Following:
            collect_following(node, test, output, state);
            break;
          case Axis::Preceding:
            collect_preceding(node, test, output, state);
            break;
          case Axis::Attribute:
            add_attributes(node, test, output);
            break;
        }
      }

      // Applies the predicates (and the reverse-axis order) to output[start..].
      void select(std::size_t start,
                  std::vector<XNode> &output,
                  EvalState &state) const {
        bool needs_filter = !predicates.empty() || is_reverse();
        if (!needs_filter) return;

        std::vector<XNode> selected(output.begin() + std::ptrdiff_t(start),
                                    output.end());
        filter_predicates(selected, predicates, state);
        if (is_reverse()) std::reverse(selected.begin(), selected.end());

        output.resize(start);
        output.insert(output.end(), selected.begin(), selected.end());
      }

      static void collect_siblings(const XNode &node,
                                   const NodeTest &test,
                                   std::vector<XNode> &output,
                                   bool following) {
        if (node.kind == XNodeKind::Root || node.kind == XNodeKind::Attribute)
          return;

        if (!test.matches_text() && node.kind != XNodeKind::Text) {
          // tags only: walk the flat list
          const Tag &tag = node.tag;
          const auto &data = tag.document().tag_data();
          const int depth = tag.depth();
          const int step = following ? 1 : -1;
          for (int i = tag.index() + step; i >= 0 && i < int(data.size());
               i += step) {
            const TagData &item = data[std::size_t(i)];
            if (item.depth < depth) break;
            const Tag handle(&tag.document(), i);
            if (item.depth == depth && test.matches_tag(handle))
              output.push_back(XNode::from_tag(handle));
          }
          return;
        }

        XNode parent;
        if (!try_get_parent(node, parent)) return;

        std::vector<XNode> siblings;
        add_children(parent, NodeTest::any_node(), siblings);

        const std::int64_t key = node.key();
        std::size_t index = siblings.size();
        for (std::size_t i = 0; i < siblings.size(); i++) {
          if (siblings[i].key() == key) {
            index = i;
            break;
          }
        }
        if (index == siblings.size()) return;

        if (following) {
          for (std::size_t i = index + 1; i < siblings.size(); i++) {
            if (test.matches_node(siblings[i], false))
              output.push_back(siblings[i]);
          }
        } else {
          for (std::size_t i = index; i > 0; i--) {
            if (test.matches_node(siblings[i - 1], false))
              output.push_back(siblings[i - 1]);
          }
        }
      }

      static void collect_following(const XNode &node,
                                    const NodeTest &test,
                                    std::vector<XNode> &output,
                                    EvalState &state) {
        if (node.kind == XNodeKind::Root) return;
        const int after = node.range_end();

        if (test.matches_text()) {
          for (const XNode &item : state.all_nodes()) {
            if (item.range_start() > after && test.matches_node(item, false))
              output.push_back(item);
          }
          return;
        }

        const Document &document = *node.document;
        const auto &data = document.tag_data();
        const int first =
            (node.kind == XNodeKind::Element || node.kind == XNodeKind::Comment)
                ? node.tag.index() + 1
                : 0;
        for (int i = first; i < int(data.size()); i++) {
          const TagData &tag = data[std::size_t(i)];
          const Tag handle(&document, i);
          if (tag.start > after && test.matches_tag(handle))
            output.push_back(XNode::from_tag(handle));
        }
      }

      static void collect_preceding(const XNode &node,
                                    const NodeTest &test,
                                    std::vector<XNode> &output,
                                    EvalState &state) {
        if (node.kind == XNodeKind::Root) return;
        const int before = node.range_start();

        if (test.matches_text()) {
          const std::vector<XNode> &all = state.all_nodes();
          for (std::size_t i = all.size(); i > 0; i--) {
            if (all[i - 1].range_end() < before
                && test.matches_node(all[i - 1], false))
              output.push_back(all[i - 1]);
          }
          return;
        }

        const Document &document = *node.document;
        const auto &data = document.tag_data();
        const int last =
            (node.kind == XNodeKind::Element || node.kind == XNodeKind::Comment)
                ? node.tag.index() - 1
                : int(data.size()) - 1;
        for (int i = last; i >= 0; i--) {
          const TagData &tag = data[std::size_t(i)];
          const Tag handle(&document, i);
          if (tag.end < before && test.matches_tag(handle))
            output.push_back(XNode::from_tag(handle));
        }
      }
    };

    // A location path, absolute or relative, optionally starting from a filter
    // expression: (//a)[1]/@href.
    struct PathExpr : XPathExpr {
      std::shared_ptr<XPathExpr> start;
      bool absolute = false;
      std::vector<Step> steps;

      PathExpr(std::shared_ptr<XPathExpr> start_,
               bool absolute_,
               std::vector<Step> steps_)
          : start(std::move(start_)),
            absolute(absolute_),
            steps(std::move(steps_)) {}

      XType type() const override {
        return XType::NodeSet;
      }
      bool uses_position() const override {
        return start ? start->uses_position() : false;
      }

      Value eval(const Ctx &ctx) const override {
        std::vector<XNode> current;
        if (start) {
          current = start->eval_nodes(ctx).nodes;
        } else if (absolute) {
          current.push_back(XNode::root(*ctx.state->document));
        } else {
          current.push_back(ctx.node);
        }

        for (const Step &step : steps) {
          if (current.empty()) break;
          current = step.apply(current, *ctx.state);
        }

        NodeSet result;
        result.nodes = std::move(current);
        return result;
      }
    };

    // A primary expression with predicates: (//li)[last()].
    struct FilterExpr : XPathExpr {
      std::shared_ptr<XPathExpr> primary;
      std::vector<std::shared_ptr<XPathExpr>> predicates;

      FilterExpr(std::shared_ptr<XPathExpr> primary_,
                 std::vector<std::shared_ptr<XPathExpr>> predicates_)
          : primary(std::move(primary_)), predicates(std::move(predicates_)) {}

      XType type() const override {
        return XType::NodeSet;
      }
      bool uses_position() const override {
        return primary->uses_position();
      }

      Value eval(const Ctx &ctx) const override {
        NodeSet result;
        result.nodes = primary->eval_nodes(ctx).nodes;
        filter_predicates(result.nodes, predicates, *ctx.state);
        return result;
      }
    };

    enum class Fn : std::uint8_t {
      Last,
      Position,
      Count,
      Name,
      LocalName,
      String,
      Concat,
      StartsWith,
      EndsWith,
      Contains,
      SubstringBefore,
      SubstringAfter,
      Substring,
      StringLength,
      NormalizeSpace,
      Translate,
      LowerCase,
      UpperCase,
      Not,
      True,
      False,
      Boolean,
      Number,
      Sum,
      Floor,
      Ceiling,
      Round,
    };

    struct FunctionInfo {
      Fn fn;
      int min_args;
      int max_args;
      XType type;
    };

    const std::unordered_map<std::string, FunctionInfo> &function_table() {
      static const std::unordered_map<std::string, FunctionInfo> table = {
          {"last", {Fn::Last, 0, 0, XType::Number}},
          {"position", {Fn::Position, 0, 0, XType::Number}},
          {"count", {Fn::Count, 1, 1, XType::Number}},
          {"name", {Fn::Name, 0, 1, XType::String}},
          {"local-name", {Fn::LocalName, 0, 1, XType::String}},
          {"string", {Fn::String, 0, 1, XType::String}},
          {"concat",
           {Fn::Concat, 2, std::numeric_limits<int>::max(), XType::String}},
          {"starts-with", {Fn::StartsWith, 2, 2, XType::Boolean}},
          {"ends-with", {Fn::EndsWith, 2, 2, XType::Boolean}},
          {"contains", {Fn::Contains, 2, 2, XType::Boolean}},
          {"substring-before", {Fn::SubstringBefore, 2, 2, XType::String}},
          {"substring-after", {Fn::SubstringAfter, 2, 2, XType::String}},
          {"substring", {Fn::Substring, 2, 3, XType::String}},
          {"string-length", {Fn::StringLength, 0, 1, XType::Number}},
          {"normalize-space", {Fn::NormalizeSpace, 0, 1, XType::String}},
          {"translate", {Fn::Translate, 3, 3, XType::String}},
          {"lower-case", {Fn::LowerCase, 1, 1, XType::String}},
          {"upper-case", {Fn::UpperCase, 1, 1, XType::String}},
          {"not", {Fn::Not, 1, 1, XType::Boolean}},
          {"true", {Fn::True, 0, 0, XType::Boolean}},
          {"false", {Fn::False, 0, 0, XType::Boolean}},
          {"boolean", {Fn::Boolean, 1, 1, XType::Boolean}},
          {"number", {Fn::Number, 0, 1, XType::Number}},
          {"sum", {Fn::Sum, 1, 1, XType::Number}},
          {"floor", {Fn::Floor, 1, 1, XType::Number}},
          {"ceiling", {Fn::Ceiling, 1, 1, XType::Number}},
          {"round", {Fn::Round, 1, 1, XType::Number}},
      };
      return table;
    }

    double round_half_up(double value) {
      if (std::isnan(value) || std::isinf(value)) return value;
      return std::floor(value + 0.5);
    }

    struct FunctionExpr : XPathExpr {
      Fn fn;
      std::vector<std::shared_ptr<XPathExpr>> args;
      XType result_type;

      FunctionExpr(Fn fn_,
                   std::vector<std::shared_ptr<XPathExpr>> args_,
                   XType type_)
          : fn(fn_), args(std::move(args_)), result_type(type_) {}

      XType type() const override {
        return result_type;
      }

      bool uses_position() const override {
        if (fn == Fn::Position || fn == Fn::Last) return true;
        for (const auto &arg : args) {
          if (arg->uses_position()) return true;
        }
        return false;
      }

      bool eval_bool(const Ctx &ctx) const override {
        if (result_type == XType::Boolean) return std::get<bool>(call(ctx));
        return XPathExpr::eval_bool(ctx);
      }

      Value eval(const Ctx &ctx) const override {
        return call(ctx);
      }

     private:
      std::string arg_string(int index, const Ctx &ctx) const {
        return args[std::size_t(index)]->eval_string(ctx);
      }

      std::string context_or_arg_string(const Ctx &ctx) const {
        return args.empty() ? string_value(ctx.node) : arg_string(0, ctx);
      }

      Value call(const Ctx &ctx) const {
        switch (fn) {
          case Fn::Last:
            return double(ctx.size);
          case Fn::Position:
            return double(ctx.position);
          case Fn::Count:
            return double(args[0]->eval_nodes(ctx).nodes.size());

          case Fn::Name:
          case Fn::LocalName: {
            std::string name;
            if (args.empty()) {
              name = node_name(ctx.node);
            } else {
              const NodeSet nodes = args[0]->eval_nodes(ctx);
              name = nodes.nodes.empty() ? "" : node_name(nodes.nodes[0]);
            }
            if (fn == Fn::LocalName) {
              const std::size_t colon = name.rfind(':');
              return colon == std::string::npos ? name : name.substr(colon + 1);
            }
            return name;
          }

          case Fn::String:
            return context_or_arg_string(ctx);

          case Fn::Concat: {
            std::string result;
            for (const auto &arg : args) result += arg->eval_string(ctx);
            return result;
          }

          case Fn::StartsWith:
            return arg_string(0, ctx).starts_with(arg_string(1, ctx));
          case Fn::EndsWith:
            return arg_string(0, ctx).ends_with(arg_string(1, ctx));
          case Fn::Contains:
            return arg_string(0, ctx).find(arg_string(1, ctx))
                   != std::string::npos;

          case Fn::SubstringBefore: {
            const std::string s = arg_string(0, ctx);
            const std::size_t index = s.find(arg_string(1, ctx));
            return index == std::string::npos ? std::string{}
                                              : s.substr(0, index);
          }

          case Fn::SubstringAfter: {
            const std::string s = arg_string(0, ctx);
            const std::string t = arg_string(1, ctx);
            const std::size_t index = s.find(t);
            return index == std::string::npos ? std::string{}
                                              : s.substr(index + t.size());
          }

          case Fn::Substring:
            return substring(ctx);

          case Fn::StringLength:
            return double(context_or_arg_string(ctx).size());

          case Fn::NormalizeSpace: {
            const std::string value = context_or_arg_string(ctx);
            std::string result;
            bool pending_space = false;
            for (char c : value) {
              if (is_space(c)) {
                if (!result.empty()) pending_space = true;
              } else {
                if (pending_space) {
                  result.push_back(' ');
                  pending_space = false;
                }
                result.push_back(c);
              }
            }
            return result;
          }

          case Fn::Translate: {
            const std::string s = arg_string(0, ctx);
            const std::string from = arg_string(1, ctx);
            const std::string to = arg_string(2, ctx);
            std::string result;
            for (char c : s) {
              const std::size_t index = from.find(c);
              if (index == std::string::npos) {
                result.push_back(c);
              } else if (index < to.size()) {
                result.push_back(to[index]);
              }
            }
            return result;
          }

          case Fn::LowerCase: {
            std::string value = arg_string(0, ctx);
            for (char &c : value) c = ascii_lower(c);
            return value;
          }
          case Fn::UpperCase: {
            std::string value = arg_string(0, ctx);
            for (char &c : value) c = ascii_upper(c);
            return value;
          }

          case Fn::Not:
            return !args[0]->eval_bool(ctx);
          case Fn::True:
            return true;
          case Fn::False:
            return false;
          case Fn::Boolean:
            return args[0]->eval_bool(ctx);

          case Fn::Number:
            return args.empty() ? string_to_number(string_value(ctx.node))
                                : args[0]->eval_number(ctx);

          case Fn::Sum: {
            double sum = 0;
            for (const XNode &node : args[0]->eval_nodes(ctx).nodes)
              sum += string_to_number(string_value(node));
            return sum;
          }

          case Fn::Floor:
            return std::floor(args[0]->eval_number(ctx));
          case Fn::Ceiling:
            return std::ceil(args[0]->eval_number(ctx));
          case Fn::Round:
            return round_half_up(args[0]->eval_number(ctx));
        }
        throw std::logic_error("unknown XPath function");
      }

      std::string substring(const Ctx &ctx) const {
        // XPath 1.0: the chars at 1-based positions p with round(start) <= p <
        // round(start) + round(length)
        const std::string s = arg_string(0, ctx);
        const double from = round_half_up(args[1]->eval_number(ctx));
        const double to = args.size() > 2
                              ? from + round_half_up(args[2]->eval_number(ctx))
                              : std::numeric_limits<double>::infinity();
        if (std::isnan(from) || std::isnan(to)) return "";

        const int first = from <= 1                 ? 0
                          : from > double(s.size()) ? int(s.size())
                                                    : int(from) - 1;
        const int end = to >= double(s.size()) + 1 ? int(s.size())
                        : to <= 1                  ? 0
                                                   : int(to) - 1;

        return end > first
                   ? s.substr(std::size_t(first), std::size_t(end - first))
                   : "";
      }
    };

    // ---------------------------------------------------------------------------
    // Compiler
    // ---------------------------------------------------------------------------

    enum class Tok : std::uint8_t {
      End,
      Slash,
      DoubleSlash,
      LParen,
      RParen,
      LBracket,
      RBracket,
      Dot,
      DotDot,
      At,
      Comma,
      ColonColon,
      Pipe,
      Plus,
      Minus,
      Eq,
      Neq,
      Lt,
      Le,
      Gt,
      Ge,
      Star,      // name test *
      Multiply,  // operator *
      And,
      Or,
      Div,
      Mod,
      Name,
      Literal,
      Number,
      Dollar,
    };

    struct Token {
      Tok kind = Tok::End;
      std::string text;
      double number = 0;
      int position = 0;
    };

    class XPathCompiler {
     public:
      explicit XPathCompiler(std::string_view source)
          : source_(source), tokens_(tokenize(source)) {}

      std::shared_ptr<XPathExpr> compile() {
        auto expr = parse_expr();
        if (peek().kind != Tok::End)
          throw error("unexpected " + describe(peek()));
        return expr;
      }

     private:
      static bool is_name_start(char c) {
        return is_ascii_alpha(c) || c == '_'
               || static_cast<unsigned char>(c) >= 0x80;
      }
      static bool is_name_char(char c) {
        return is_name_start(c) || is_ascii_digit(c) || c == '-' || c == '.';
      }

      static std::vector<Token> tokenize(std::string_view s) {
        std::vector<Token> tokens;
        std::size_t i = 0;
        const auto error = [&](const std::string &message,
                               std::size_t position) -> std::invalid_argument {
          return std::invalid_argument(
              "Invalid XPath \"" + std::string(s) + "\" at position "
              + std::to_string(position) + ": " + message + ".");
        };

        while (true) {
          while (i < s.size() && is_space(s[i])) i++;
          if (i >= s.size()) {
            tokens.push_back(Token{Tok::End, {}, 0, int(i)});
            return tokens;
          }

          const std::size_t start = i;
          const char c = s[i];
          const char next = i + 1 < s.size() ? s[i + 1] : '\0';

          const bool operator_expected =
              !tokens.empty()
              && (tokens.back().kind == Tok::RParen
                  || tokens.back().kind == Tok::RBracket
                  || tokens.back().kind == Tok::Dot
                  || tokens.back().kind == Tok::DotDot
                  || tokens.back().kind == Tok::Star
                  || tokens.back().kind == Tok::Name
                  || tokens.back().kind == Tok::Literal
                  || tokens.back().kind == Tok::Number);

          Tok kind = Tok::End;
          std::string text;
          double number = 0;

          switch (c) {
            case '/':
              if (next == '/') {
                kind = Tok::DoubleSlash;
                i += 2;
              } else {
                kind = Tok::Slash;
                i++;
              }
              break;
            case '(':
              kind = Tok::LParen;
              i++;
              break;
            case ')':
              kind = Tok::RParen;
              i++;
              break;
            case '[':
              kind = Tok::LBracket;
              i++;
              break;
            case ']':
              kind = Tok::RBracket;
              i++;
              break;
            case '@':
              kind = Tok::At;
              i++;
              break;
            case ',':
              kind = Tok::Comma;
              i++;
              break;
            case '|':
              kind = Tok::Pipe;
              i++;
              break;
            case '+':
              kind = Tok::Plus;
              i++;
              break;
            case '-':
              kind = Tok::Minus;
              i++;
              break;
            case '=':
              kind = Tok::Eq;
              i++;
              break;
            case '$':
              kind = Tok::Dollar;
              i++;
              break;
            case '*':
              kind = operator_expected ? Tok::Multiply : Tok::Star;
              i++;
              break;
            case '!':
              if (next != '=') throw error("'!' must be followed by '='", i);
              kind = Tok::Neq;
              i += 2;
              break;
            case '<':
              if (next == '=') {
                kind = Tok::Le;
                i += 2;
              } else {
                kind = Tok::Lt;
                i++;
              }
              break;
            case '>':
              if (next == '=') {
                kind = Tok::Ge;
                i += 2;
              } else {
                kind = Tok::Gt;
                i++;
              }
              break;
            case ':':
              if (next != ':') throw error("unexpected ':'", i);
              kind = Tok::ColonColon;
              i += 2;
              break;
            case '"':
            case '\'': {
              const std::size_t close = s.find(c, i + 1);
              if (close == std::string_view::npos)
                throw error("unterminated string literal", i);
              kind = Tok::Literal;
              text = std::string(s.substr(i + 1, close - i - 1));
              i = close + 1;
              break;
            }
            case '.':
              if (next == '.') {
                kind = Tok::DotDot;
                i += 2;
              } else if (!is_ascii_digit(next)) {
                kind = Tok::Dot;
                i++;
              }
              break;
            default:
              break;
          }

          if (kind == Tok::End) {
            if (is_ascii_digit(c) || c == '.') {
              while (i < s.size() && is_ascii_digit(s[i])) i++;
              if (i < s.size() && s[i] == '.') {
                i++;
                while (i < s.size() && is_ascii_digit(s[i])) i++;
              }
              kind = Tok::Number;
              number = std::stod(std::string(s.substr(start, i - start)));
            } else if (is_name_start(c)) {
              i++;
              while (i < s.size()) {
                if (is_name_char(s[i])) {
                  i++;
                } else if (s[i] == ':' && i + 1 < s.size() && s[i + 1] != ':'
                           && (is_name_start(s[i + 1]) || s[i + 1] == '*')) {
                  i += 2;
                  if (s[i - 1] == '*') break;
                } else {
                  break;
                }
              }

              text = std::string(s.substr(start, i - start));
              kind = Tok::Name;
              if (operator_expected) {
                if (text == "and") {
                  kind = Tok::And;
                } else if (text == "or") {
                  kind = Tok::Or;
                } else if (text == "div") {
                  kind = Tok::Div;
                } else if (text == "mod") {
                  kind = Tok::Mod;
                } else {
                  throw error("expected an operator, found '" + text + "'",
                              start);
                }
              }
            } else {
              throw error(std::string("unexpected character '") + c + "'", i);
            }
          }

          tokens.push_back(Token{kind, std::move(text), number, int(start)});
        }
      }

      const Token &peek(int offset = 0) const {
        const std::size_t index = std::min<std::size_t>(
            std::size_t(index_ + offset), tokens_.size() - 1);
        return tokens_[index];
      }

      Token advance() {
        return tokens_[std::size_t(index_++)];
      }

      bool accept(Tok kind) {
        if (peek().kind != kind) return false;
        index_++;
        return true;
      }

      void expect(Tok kind, std::string_view what) {
        if (!accept(kind))
          throw error("expected " + std::string(what) + ", found "
                      + describe(peek()));
      }

      std::string describe(const Token &token) const {
        switch (token.kind) {
          case Tok::End:
            return "the end of the expression";
          case Tok::Name:
          case Tok::Literal:
            return "'" + token.text + "'";
          case Tok::Number:
            return number_to_string(token.number);
          default:
            return "'"
                   + std::string(trim(source_.substr(
                       std::size_t(token.position),
                       std::min<std::size_t>(
                           2, source_.size() - std::size_t(token.position)))))
                   + "'";
        }
      }

      std::invalid_argument error(const std::string &message) const {
        return error(message, peek().position);
      }

      std::invalid_argument error(const std::string &message,
                                  std::size_t position) const {
        return std::invalid_argument(
            "Invalid XPath \"" + std::string(source_) + "\" at position "
            + std::to_string(position) + ": " + message + ".");
      }

      // Grammar (XPath 1.0, section 3)

      std::shared_ptr<XPathExpr> parse_expr() {
        return parse_or();
      }

      std::shared_ptr<XPathExpr> parse_or() {
        auto left = parse_and();
        while (accept(Tok::Or))
          left = std::make_shared<OrExpr>(left, parse_and());
        return left;
      }

      std::shared_ptr<XPathExpr> parse_and() {
        auto left = parse_equality();
        while (accept(Tok::And))
          left = std::make_shared<AndExpr>(left, parse_equality());
        return left;
      }

      std::shared_ptr<XPathExpr> parse_equality() {
        auto left = parse_relational();
        while (true) {
          CompareOp op;
          if (accept(Tok::Eq)) {
            op = CompareOp::Eq;
          } else if (accept(Tok::Neq)) {
            op = CompareOp::Neq;
          } else {
            return left;
          }

          left = make_equality(op, left, parse_relational());
        }
      }

      // @name = 'literal' is compared in place; everything else by the general rules.
      static std::shared_ptr<XPathExpr> make_equality(
          CompareOp op,
          std::shared_ptr<XPathExpr> left,
          std::shared_ptr<XPathExpr> right) {
        std::string name;
        if (try_get_attribute_name(left, name)) {
          if (auto *literal = dynamic_cast<LiteralExpr *>(right.get()))
            return std::make_shared<AttributeEqualsExpr>(
                name, literal->value, op == CompareOp::Neq);
        }
        if (try_get_attribute_name(right, name)) {
          if (auto *literal = dynamic_cast<LiteralExpr *>(left.get()))
            return std::make_shared<AttributeEqualsExpr>(
                name, literal->value, op == CompareOp::Neq);
        }

        return std::make_shared<CompareExpr>(op, left, right);
      }

      // Whether the expression is a bare @name (one attribute step, no
      // predicates).
      static bool try_get_attribute_name(const std::shared_ptr<XPathExpr> &expr,
                                         std::string &name) {
        auto *path = dynamic_cast<PathExpr *>(expr.get());
        if (!path || path->start || path->absolute || path->steps.size() != 1)
          return false;

        const Step &step = path->steps[0];
        if (step.axis != Axis::Attribute || !step.predicates.empty()
            || step.test.kind != TestKind::Name)
          return false;

        name = step.test.name;
        return true;
      }

      std::shared_ptr<XPathExpr> parse_relational() {
        auto left = parse_additive();
        while (true) {
          CompareOp op;
          if (accept(Tok::Lt)) {
            op = CompareOp::Lt;
          } else if (accept(Tok::Le)) {
            op = CompareOp::Le;
          } else if (accept(Tok::Gt)) {
            op = CompareOp::Gt;
          } else if (accept(Tok::Ge)) {
            op = CompareOp::Ge;
          } else {
            return left;
          }

          left = std::make_shared<CompareExpr>(op, left, parse_additive());
        }
      }

      std::shared_ptr<XPathExpr> parse_additive() {
        auto left = parse_multiplicative();
        while (true) {
          if (accept(Tok::Plus)) {
            left = std::make_shared<ArithExpr>(
                ArithOp::Add, left, parse_multiplicative());
          } else if (accept(Tok::Minus)) {
            left = std::make_shared<ArithExpr>(
                ArithOp::Sub, left, parse_multiplicative());
          } else {
            return left;
          }
        }
      }

      std::shared_ptr<XPathExpr> parse_multiplicative() {
        auto left = parse_unary();
        while (true) {
          if (accept(Tok::Multiply)) {
            left =
                std::make_shared<ArithExpr>(ArithOp::Mul, left, parse_unary());
          } else if (accept(Tok::Div)) {
            left =
                std::make_shared<ArithExpr>(ArithOp::Div, left, parse_unary());
          } else if (accept(Tok::Mod)) {
            left =
                std::make_shared<ArithExpr>(ArithOp::Mod, left, parse_unary());
          } else {
            return left;
          }
        }
      }

      std::shared_ptr<XPathExpr> parse_unary() {
        if (accept(Tok::Minus))
          return std::make_shared<NegateExpr>(parse_unary());
        return parse_union();
      }

      std::shared_ptr<XPathExpr> parse_union() {
        auto left = parse_path();
        while (peek().kind == Tok::Pipe) {
          const std::size_t position = std::size_t(advance().position);
          auto right = parse_path();
          if (left->type() != XType::NodeSet || right->type() != XType::NodeSet)
            throw error("'|' needs node-sets on both sides", position);
          left = std::make_shared<UnionExpr>(left, right);
        }
        return left;
      }

      static bool is_node_type(const std::string &name) {
        return name == "node" || name == "text" || name == "comment"
               || name == "processing-instruction";
      }

      std::shared_ptr<XPathExpr> parse_path() {
        const Token &token = peek();
        const bool is_filter =
            token.kind == Tok::Literal || token.kind == Tok::Number
            || token.kind == Tok::LParen || token.kind == Tok::Dollar
            || (token.kind == Tok::Name && peek(1).kind == Tok::LParen
                && !is_node_type(token.text));

        if (!is_filter) return parse_location_path();

        const std::size_t position = std::size_t(token.position);
        auto primary = parse_primary();
        auto predicates = parse_predicates();
        if (!predicates.empty()) {
          if (primary->type() != XType::NodeSet)
            throw error("a predicate needs a node-set", position);
          primary =
              std::make_shared<FilterExpr>(primary, std::move(predicates));
        }

        if (peek().kind != Tok::Slash && peek().kind != Tok::DoubleSlash)
          return primary;

        if (primary->type() != XType::NodeSet)
          throw error("'/' needs a node-set on the left", position);

        std::vector<Step> steps;
        parse_relative_steps(steps, true);
        return std::make_shared<PathExpr>(
            primary, false, optimize(std::move(steps)));
      }

      std::shared_ptr<XPathExpr> parse_location_path() {
        std::vector<Step> steps;
        bool absolute = false;

        if (accept(Tok::Slash)) {
          absolute = true;
          if (can_start_step(peek())) parse_relative_steps(steps, false);
        } else if (peek().kind == Tok::DoubleSlash) {
          absolute = true;
          parse_relative_steps(steps, true);
        } else {
          if (!can_start_step(peek()))
            throw error("unexpected " + describe(peek()));
          parse_relative_steps(steps, false);
        }

        return std::make_shared<PathExpr>(
            nullptr, absolute, optimize(std::move(steps)));
      }

      static bool can_start_step(const Token &token) {
        return token.kind == Tok::Dot || token.kind == Tok::DotDot
               || token.kind == Tok::At || token.kind == Tok::Star
               || token.kind == Tok::Name;
      }

      // Steps separated by '/' or '//'.
      void parse_relative_steps(std::vector<Step> &steps, bool after_slash) {
        if (!after_slash) steps.push_back(parse_step());

        while (true) {
          if (accept(Tok::Slash)) {
            steps.push_back(parse_step());
          } else if (accept(Tok::DoubleSlash)) {
            Step descendant;
            descendant.axis = Axis::DescendantOrSelf;
            descendant.test = NodeTest::any_node();
            steps.push_back(std::move(descendant));
            steps.push_back(parse_step());
          } else {
            return;
          }
        }
      }

      static const std::unordered_map<std::string, Axis> &axes() {
        static const std::unordered_map<std::string, Axis> map = {
            {"child", Axis::Child},
            {"descendant", Axis::Descendant},
            {"descendant-or-self", Axis::DescendantOrSelf},
            {"self", Axis::Self},
            {"parent", Axis::Parent},
            {"ancestor", Axis::Ancestor},
            {"ancestor-or-self", Axis::AncestorOrSelf},
            {"following-sibling", Axis::FollowingSibling},
            {"preceding-sibling", Axis::PrecedingSibling},
            {"following", Axis::Following},
            {"preceding", Axis::Preceding},
            {"attribute", Axis::Attribute},
        };
        return map;
      }

      Step parse_step() {
        Step step;
        if (accept(Tok::Dot)) {
          step.axis = Axis::Self;
          step.test = NodeTest::any_node();
          return step;
        }
        if (accept(Tok::DotDot)) {
          step.axis = Axis::Parent;
          step.test = NodeTest::any_node();
          return step;
        }

        step.axis = Axis::Child;
        if (accept(Tok::At)) {
          step.axis = Axis::Attribute;
        } else if (peek().kind == Tok::Name
                   && peek(1).kind == Tok::ColonColon) {
          const Token name = advance();
          auto it = axes().find(name.text);
          if (it == axes().end()) {
            throw error(name.text == "namespace"
                            ? "the namespace axis is not supported"
                            : "unknown axis '" + name.text + "'",
                        std::size_t(name.position));
          }
          step.axis = it->second;
          advance();
        }

        step.test = parse_node_test();
        step.predicates = parse_predicates();
        return step;
      }

      NodeTest parse_node_test() {
        if (accept(Tok::Star)) return NodeTest(TestKind::Wildcard, {});

        const Token token = peek();
        if (token.kind != Tok::Name)
          throw error("expected a node test, found " + describe(token));
        advance();

        const std::string name = token.text;
        if (peek().kind == Tok::LParen && is_node_type(name)) {
          advance();
          if (name == "processing-instruction") accept(Tok::Literal);
          expect(Tok::RParen, "')'");

          if (name == "node") return NodeTest::any_node();
          if (name == "text") return NodeTest(TestKind::Text, {});
          if (name == "comment") return NodeTest(TestKind::Comment, {});
          return NodeTest(TestKind::Pi, {});
        }

        if (name.size() >= 2 && name.ends_with(":*"))
          return NodeTest(TestKind::Prefix, name.substr(0, name.size() - 1));

        return NodeTest(TestKind::Name, name);
      }

      std::vector<std::shared_ptr<XPathExpr>> parse_predicates() {
        if (peek().kind != Tok::LBracket) return {};

        std::vector<std::shared_ptr<XPathExpr>> predicates;
        while (accept(Tok::LBracket)) {
          auto predicate = parse_expr();
          expect(Tok::RBracket, "']'");

          // [@name]: an existence check instead of a node-set
          std::string name;
          if (try_get_attribute_name(predicate, name))
            predicate = std::make_shared<AttributeExistsExpr>(name);

          predicates.push_back(std::move(predicate));
        }
        return predicates;
      }

      std::shared_ptr<XPathExpr> parse_primary() {
        const Token token = advance();
        switch (token.kind) {
          case Tok::Literal:
            return std::make_shared<LiteralExpr>(token.text);
          case Tok::Number:
            return std::make_shared<NumberExpr>(token.number);
          case Tok::LParen: {
            auto expr = parse_expr();
            expect(Tok::RParen, "')'");
            return expr;
          }
          case Tok::Dollar:
            throw error("variables are not supported",
                        std::size_t(token.position));
          default: {
            const std::string &name = token.text;
            auto it = function_table().find(name);
            if (it == function_table().end())
              throw error("unknown function '" + name + "()'",
                          std::size_t(token.position));

            const FunctionInfo &function = it->second;
            expect(Tok::LParen, "'('");
            std::vector<std::shared_ptr<XPathExpr>> args;
            if (!accept(Tok::RParen)) {
              do {
                args.push_back(parse_expr());
              } while (accept(Tok::Comma));
              expect(Tok::RParen, "')'");
            }

            if (int(args.size()) < function.min_args
                || int(args.size()) > function.max_args)
              throw error("wrong number of arguments for " + name + "()",
                          std::size_t(token.position));

            if ((function.fn == Fn::Count || function.fn == Fn::Sum
                 || function.fn == Fn::Name || function.fn == Fn::LocalName)
                && !args.empty() && args[0]->type() != XType::NodeSet)
              throw error(name + "() needs a node-set argument",
                          std::size_t(token.position));

            return std::make_shared<FunctionExpr>(
                function.fn, std::move(args), function.type);
          }
        }
      }

      // //x is descendant-or-self::node()/child::x. Without positional
      // predicates on x it is the same as descendant::x: one linear scan of the
      // tag list.
      static std::vector<Step> optimize(std::vector<Step> steps) {
        for (std::size_t i = 0; i + 1 < steps.size();) {
          const Step &step = steps[i];
          if (step.axis != Axis::DescendantOrSelf
              || step.test.kind != TestKind::Node || !step.predicates.empty()) {
            i++;
            continue;
          }

          const Step &next = steps[i + 1];
          if (next.axis == Axis::Child && !next.has_positional_predicates()) {
            Step combined;
            combined.axis = Axis::Descendant;
            combined.test = next.test;
            combined.predicates = next.predicates;
            steps[i] = std::move(combined);
            steps.erase(steps.begin() + std::ptrdiff_t(i + 1));
          } else if (next.axis == Axis::Child || next.axis == Axis::Attribute) {
            Step containers;
            containers.axis = Axis::DescendantOrSelf;
            containers.test = NodeTest::containers();
            steps[i] = std::move(containers);
            i++;
          } else {
            i++;
          }
        }

        return steps;
      }

      std::string_view source_;
      std::vector<Token> tokens_;
      std::size_t index_ = 0;
    };

  }  // namespace

  // ---------------------------------------------------------------------------
  // Public entry points
  // ---------------------------------------------------------------------------

  namespace {

    Value evaluate(const XPathExpr &expr, const XNode &context) {
      EvalState state;
      state.document = context.document;
      return expr.eval(Ctx{context, 1, 1, &state});
    }

  }  // namespace

  std::shared_ptr<const XPathExpr> compile_xpath(std::string_view expression) {
    XPathCompiler compiler(expression);
    return compiler.compile();
  }

  // The functions below are used by XPathQuery.

  std::vector<Tag> xpath_select_tags(const XPathExpr &expr,
                                     const XNode &context) {
    if (expr.type() != XType::NodeSet)
      throw std::invalid_argument("The XPath expression does not give tags.");

    Value result = evaluate(expr, context);
    NodeSet *nodes = std::get_if<NodeSet>(&result);
    if (!nodes) return {};

    std::vector<Tag> tags;
    for (const XNode &node : nodes->nodes) {
      if (node.kind == XNodeKind::Element || node.kind == XNodeKind::Comment)
        tags.push_back(node.tag);
    }
    return tags;
  }

  std::vector<std::string> xpath_select_values(const XPathExpr &expr,
                                               const XNode &context) {
    Value result = evaluate(expr, context);
    if (NodeSet *nodes = std::get_if<NodeSet>(&result)) {
      std::vector<std::string> values;
      values.reserve(nodes->nodes.size());
      for (const XNode &node : nodes->nodes)
        values.push_back(string_value(node));
      return values;
    }
    if (std::holds_alternative<std::monostate>(result)) return {};
    return {to_string(result)};
  }

  std::optional<std::string> xpath_select_value(const XPathExpr &expr,
                                                const XNode &context) {
    Value result = evaluate(expr, context);
    if (NodeSet *nodes = std::get_if<NodeSet>(&result)) {
      return nodes->nodes.empty()
                 ? std::nullopt
                 : std::optional<std::string>(string_value(nodes->nodes[0]));
    }
    if (std::holds_alternative<std::monostate>(result)) return std::nullopt;
    return to_string(result);
  }

}  // namespace fh::detail

namespace fh {

  XPathQuery::XPathQuery(std::string expression,
                         std::shared_ptr<const detail::XPathExpr> expr)
      : expression_(std::move(expression)), compiled_(std::move(expr)) {}

  XPathQuery XPathQuery::compile(std::string_view expression) {
    auto compiled = detail::compile_xpath(expression);
    return XPathQuery(std::string(expression), std::move(compiled));
  }

  XPathQuery XPathQuery::cached(std::string_view expression) {
    static std::mutex mutex;
    static std::unordered_map<std::string, XPathQuery> cache;

    std::lock_guard<std::mutex> lock(mutex);
    const std::string key(expression);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    XPathQuery query = compile(expression);
    if (cache.size() >= 1024) cache.clear();
    cache.emplace(key, query);
    return query;
  }

  std::vector<Tag> XPathQuery::select_tags(const Document &doc) const {
    return detail::xpath_select_tags(*compiled_, detail::XNode::root(doc));
  }

  std::vector<Tag> XPathQuery::select_tags(const Tag &scope) const {
    return detail::xpath_select_tags(*compiled_,
                                     detail::XNode::from_tag(scope));
  }

  std::vector<std::string> XPathQuery::select_values(
      const Document &doc) const {
    return detail::xpath_select_values(*compiled_, detail::XNode::root(doc));
  }

  std::vector<std::string> XPathQuery::select_values(const Tag &scope) const {
    return detail::xpath_select_values(*compiled_,
                                       detail::XNode::from_tag(scope));
  }

  std::optional<std::string> XPathQuery::select_value(
      const Document &doc) const {
    return detail::xpath_select_value(*compiled_, detail::XNode::root(doc));
  }

  std::optional<std::string> XPathQuery::select_value(const Tag &scope) const {
    return detail::xpath_select_value(*compiled_,
                                      detail::XNode::from_tag(scope));
  }

}  // namespace fh
