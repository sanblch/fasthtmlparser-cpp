// The parser: a port of z3l.FastHtmlParser/Core/ParsingCore.cs.
//
// A page is scanned once in fast mode: tags are matched to their end tags and nothing else is
// done. Well-formed markup needs nothing more. Where the HTML5 optional-end-tag rules matter
// (<li>one<li>two</ul>) the tags are left open inside an element; when its end tag (or the end
// of the document) finds one of the rule targets (li, p, td, tr, option, ...) still open, only
// that part is scanned again with the rules, from the outermost such tag up to that end tag.
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fh::detail {

namespace {

constexpr int kMaxDepth = 512;
constexpr int kRepairBudgetFactor = 8;

struct TagState {
    int key = -1;              // (int)HtmlTag, or -1 for a custom tag name
    int depth = 0;
    bool self_closed = false;  // <x/>: stays in the open list, but does not nest anything
    int prev_open = -1;        // the open tags as a linked list in document order
};

// Buffers reused by the parses of one thread: they are proportional to the page size, and
// touching fresh memory on every parse (allocation, zeroing, page faults) costs more than
// copying out the tags that were actually found. The Rust port does the same.
struct Scratch {
    std::vector<TagData> tags;
    std::vector<TagState> states;
};

Scratch& scratch() {
    thread_local Scratch buffers;
    return buffers;
}

struct ImpliedEndRule {
    std::array<bool, kTagCount> targets{};
    std::array<bool, kTagCount> stops{};
    std::vector<int> target_keys;

    ImpliedEndRule(std::initializer_list<HtmlTag> target_list, std::initializer_list<HtmlTag> stop_list) {
        for (HtmlTag tag : target_list) {
            targets[std::size_t(tag)] = true;
            target_keys.push_back(int(tag));
        }
        for (HtmlTag tag : stop_list) stops[std::size_t(tag)] = true;
    }

    bool any_open(const std::vector<int>& open_by_key) const {
        for (int key : target_keys) {
            if (open_by_key[std::size_t(key)] > 0) return true;
        }
        return false;
    }
};

struct RuleTables {
    std::array<bool, kTagCount> is_rule_target{};
    std::array<bool, kTagCount> closes_p{};
    std::array<bool, kTagCount> void_tags{};
    std::array<std::string, kTagCount> raw_text_storage;
    std::array<std::string_view, kTagCount> raw_text_ends{};
    std::array<const ImpliedEndRule*, kTagCount> implied{};

    std::unique_ptr<ImpliedEndRule> p_rule;

    RuleTables() {
        p_rule = std::make_unique<ImpliedEndRule>(std::initializer_list<HtmlTag>{HtmlTag::P},
                                                  std::initializer_list<HtmlTag>{
                                                      HtmlTag::Html, HtmlTag::Body, HtmlTag::Table, HtmlTag::Caption,
                                                      HtmlTag::Td, HtmlTag::Th, HtmlTag::Template, HtmlTag::Object,
                                                      HtmlTag::Applet, HtmlTag::Marquee, HtmlTag::Button});

        for (HtmlTag tag : {
                 HtmlTag::Address, HtmlTag::Article, HtmlTag::Aside, HtmlTag::Blockquote, HtmlTag::Center,
                 HtmlTag::Details, HtmlTag::Dialog, HtmlTag::Dir, HtmlTag::Div, HtmlTag::Dl, HtmlTag::Fieldset,
                 HtmlTag::Figcaption, HtmlTag::Figure, HtmlTag::Footer, HtmlTag::Form, HtmlTag::H1, HtmlTag::H2,
                 HtmlTag::H3, HtmlTag::H4, HtmlTag::H5, HtmlTag::H6, HtmlTag::Header, HtmlTag::Hgroup, HtmlTag::Hr,
                 HtmlTag::Listing, HtmlTag::Main, HtmlTag::Menu, HtmlTag::Nav, HtmlTag::Ol, HtmlTag::P, HtmlTag::Pre,
                 HtmlTag::Search, HtmlTag::Section, HtmlTag::Summary, HtmlTag::Table, HtmlTag::Ul, HtmlTag::Li,
                 HtmlTag::Dd, HtmlTag::Dt, HtmlTag::Plaintext, HtmlTag::Xmp})
            closes_p[std::size_t(tag)] = true;

        // Optional end tags (HTML5). The stop tags limit the scope of the rules.
        add({HtmlTag::Li}, {HtmlTag::Li},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Table, HtmlTag::Caption, HtmlTag::Td, HtmlTag::Th,
             HtmlTag::Template, HtmlTag::Object, HtmlTag::Applet, HtmlTag::Marquee, HtmlTag::Button, HtmlTag::Ul,
             HtmlTag::Ol, HtmlTag::Menu});
        add({HtmlTag::Dt, HtmlTag::Dd}, {HtmlTag::Dt, HtmlTag::Dd},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Table, HtmlTag::Caption, HtmlTag::Td, HtmlTag::Th,
             HtmlTag::Template, HtmlTag::Object, HtmlTag::Applet, HtmlTag::Marquee, HtmlTag::Button, HtmlTag::Dl});
        add({HtmlTag::Td, HtmlTag::Th}, {HtmlTag::Td, HtmlTag::Th},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Template, HtmlTag::Table, HtmlTag::Tr, HtmlTag::Tbody,
             HtmlTag::Thead, HtmlTag::Tfoot});
        add({HtmlTag::Tr}, {HtmlTag::Tr, HtmlTag::Td, HtmlTag::Th},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Template, HtmlTag::Table, HtmlTag::Tbody, HtmlTag::Thead,
             HtmlTag::Tfoot});
        add({HtmlTag::Tbody, HtmlTag::Thead, HtmlTag::Tfoot}, {HtmlTag::Tbody, HtmlTag::Thead, HtmlTag::Tfoot,
                                                               HtmlTag::Tr, HtmlTag::Td, HtmlTag::Th,
                                                               HtmlTag::Caption},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Template, HtmlTag::Table});
        add({HtmlTag::Option}, {HtmlTag::Option},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Template, HtmlTag::Select, HtmlTag::Datalist,
             HtmlTag::Optgroup});
        add({HtmlTag::Optgroup}, {HtmlTag::Option, HtmlTag::Optgroup},
            {HtmlTag::Html, HtmlTag::Body, HtmlTag::Template, HtmlTag::Select, HtmlTag::Datalist});

        for (HtmlTag tag : {HtmlTag::Area, HtmlTag::Base, HtmlTag::Br, HtmlTag::Col, HtmlTag::Embed, HtmlTag::Hr,
                            HtmlTag::Img, HtmlTag::Input, HtmlTag::Link, HtmlTag::Meta, HtmlTag::Source,
                            HtmlTag::Track, HtmlTag::Wbr, HtmlTag::Basefont, HtmlTag::Bgsound, HtmlTag::Frame,
                            HtmlTag::Keygen, HtmlTag::Param, HtmlTag::Isindex, HtmlTag::Doctype,
                            HtmlTag::XmlDeclaration})
            void_tags[std::size_t(tag)] = true;

        for (HtmlTag tag : {HtmlTag::Script, HtmlTag::Style, HtmlTag::Textarea, HtmlTag::Title, HtmlTag::Xmp,
                            HtmlTag::Iframe, HtmlTag::Noembed, HtmlTag::Noframes}) {
            const std::size_t index = std::size_t(tag);
            raw_text_storage[index] = "</" + std::string(kTagNames[index]);
            raw_text_ends[index] = raw_text_storage[index];
        }

        for (const ImpliedEndRule* rule : implied) {
            if (!rule) continue;
            for (int key : rule->target_keys) is_rule_target[std::size_t(key)] = true;
        }
    }

  private:
    // Registers the same rule for every tag that ends it.
    void add(std::initializer_list<HtmlTag> for_tags, std::initializer_list<HtmlTag> targets,
             std::initializer_list<HtmlTag> stops) {
        owned_.push_back(std::make_unique<ImpliedEndRule>(targets, stops));
        for (HtmlTag tag : for_tags) implied[std::size_t(tag)] = owned_.back().get();
    }

    std::vector<std::unique_ptr<ImpliedEndRule>> owned_;
};

const RuleTables& rules() {
    static const RuleTables tables;
    return tables;
}

class Parser {
  public:
    explicit Parser(std::string_view source)
        : source_(source), rules_(rules()), tags_(scratch().tags), states_(scratch().states) {
        max_tags_ = std::size_t(std::count(source.begin(), source.end(), '<'));
        if (tags_.size() < max_tags_) {
            tags_.resize(max_tags_);
            states_.resize(max_tags_);
        }
        stack_.reserve(64);
        open_in_list_.resize(kTagCount);
        open_by_key_.resize(kTagCount);
        repair_budget_ = std::int64_t(kRepairBudgetFactor) * std::int64_t(source.size());

        std::string_view start = trim_start(source);
        if (start.size() >= 3 && static_cast<unsigned char>(start[0]) == 0xEF &&
            static_cast<unsigned char>(start[1]) == 0xBB && static_cast<unsigned char>(start[2]) == 0xBF) {
            start = start.substr(3);
        }
        is_xml_ = iequals_ascii_prefix(start, "<?xml");
    }

    ParseResult run() {
        if (max_tags_ == 0) return {};

        bool is_tag_started = false;
        int tag_start_position = 0;
        scan<false>(0, int(source_.size()), is_tag_started, tag_start_position);

        // The end of the document ends every tag still open (a truncated page keeps its text),
        // right before an unfinished tag if the source stops inside one.
        const int region_end = is_tag_started ? tag_start_position - 1 : int(source_.size());
        if (!is_xml_) {
            const int misnested = first_misnested(0, stack_top_);
            if (misnested >= 0) repair(misnested, region_end);
        }
        truncate_open_list(0, region_end - 1);
        stack_top_ = 0;

        // Copy out the tags that were found; the scratch keeps its capacity for the next parse.
        std::vector<TagData> tags(tags_.begin(), tags_.begin() + count_);
        // Unclosed tags have no content: InnerStart is 0, as in the original.
        for (TagData& tag : tags) {
            if (tag.inner_end == 0) tag.inner_start = 0;
        }

        return {std::move(tags), std::move(custom_names_)};
    }

  private:
    // Allocates an index for a custom name: one string per distinct name per document.
    int intern_lower_name(std::string_view name) {
        if (name.empty()) return -1;
        std::string lower = to_lower(name);
        auto it = custom_index_.find(lower);
        if (it != custom_index_.end()) return it->second;

        const int index = int(custom_names_.size());
        custom_names_.push_back(lower);
        custom_index_.emplace(std::move(lower), index);
        custom_open_.emplace(index, 0);  // the count of open tags with this name
        return index;
    }

    int find_custom_name(std::string_view name) const {
        if (name.empty()) return -1;
        auto it = custom_index_.find(to_lower(name));
        return it == custom_index_.end() ? -1 : it->second;
    }

    void adjust_open_custom(int name_index, int delta) {
        if (name_index < 0) return;
        auto it = custom_open_.find(name_index);
        if (it != custom_open_.end()) it->second += delta;
    }

    // The outermost target of the rule below the first stop tag, or -1.
    int scope_start(const ImpliedEndRule& rule) const {
        int from = -1;
        for (int s = stack_top_ - 1; s >= 0; s--) {
            const int key = states_[std::size_t(stack_[std::size_t(s)])].key;
            if (key < 0) continue;
            if (rule.targets[std::size_t(key)]) {
                from = s;
            } else if (rule.stops[std::size_t(key)]) {
                break;
            }
        }
        return from;
    }

    // Among the tags left open at stack positions [from, to), the outermost one a rule should
    // have ended; -1 if there is none.
    int first_misnested(int from, int to) const {
        for (int s = from; s < to - 1; s++) {
            const int key = states_[std::size_t(stack_[std::size_t(s)])].key;
            if (key < 0 || !rules_.is_rule_target[std::size_t(key)]) continue;

            for (int inner = s + 1; inner < to; inner++) {
                const int inner_key = states_[std::size_t(stack_[std::size_t(inner)])].key;
                if (inner_key >= 0 && rules_.implied[std::size_t(inner_key)] &&
                    rules_.implied[std::size_t(inner_key)]->targets[std::size_t(key)])
                    return s;
            }
        }
        return -1;
    }

    void truncate_open_list(int first, int end) {
        int k = last_open_;
        while (k >= first) {
            TagState& state = states_[std::size_t(k)];
            TagData& tag = tags_[std::size_t(k)];

            if (state.key >= 0) {
                open_in_list_[std::size_t(state.key)]--;
            } else {
                adjust_open_custom(tag.custom, -1);
            }

            if (end >= 0 && !state.self_closed && tag.inner_end == 0) {
                tag.inner_end = end;
                tag.end = end;
            }

            k = state.prev_open;
        }
        last_open_ = k;
    }

    // Ends the outermost target of the rule (and everything opened inside it) up to the first stop tag.
    void close_in_scope(const ImpliedEndRule& rule, int end, bool update_open_by_key) {
        const int from = scope_start(rule);
        if (from < 0) return;

        const int first = stack_[std::size_t(from)];

        while (stack_top_ > from) {
            const int key = states_[std::size_t(stack_[std::size_t(--stack_top_)])].key;
            if (key == int(HtmlTag::P)) open_p_--;
            if (update_open_by_key && key >= 0) open_by_key_[std::size_t(key)]--;
        }

        truncate_open_list(first, end);
    }

    void apply_rule_in_fast_mode(const ImpliedEndRule& rule, int tag_start_position) {
        const int from = scope_start(rule);
        if (from < 0) return;

        const int misnested = first_misnested(from, stack_top_);
        if (misnested >= 0) repair(misnested, tag_start_position - 1);

        close_in_scope(rule, tag_start_position - 2, false);
    }

    // Scans again with the optional-end-tag rules from the tag at stack position `stack_position`
    // up to `region_end`: the tags from it on are replaced, the ones before it are kept.
    void repair(int stack_position, int region_end) {
        const int first = stack_[std::size_t(stack_position)];
        const int from = tags_[std::size_t(first)].start;

        const std::int64_t cost = std::int64_t(region_end) - from;
        if (cost > repair_budget_) return;
        repair_budget_ -= cost;

        truncate_open_list(first, -1);

        stack_top_ = stack_position;
        count_ = first;

        std::fill(open_by_key_.begin(), open_by_key_.end(), 0);
        open_p_ = 0;
        for (int s = 0; s < stack_position; s++) {
            const int key = states_[std::size_t(stack_[std::size_t(s)])].key;
            if (key < 0) continue;
            open_by_key_[std::size_t(key)]++;
            if (key == int(HtmlTag::P)) open_p_++;
        }

        bool ignored_started = false;
        int ignored_position = 0;
        scan<true>(from, region_end, ignored_started, ignored_position);
    }

    static std::string_view extract_tag_name(std::string_view tag_source) {
        tag_source = trim(tag_source);

        if (tag_source.starts_with("!--")) return tag_source.substr(0, 3);

        // The name ends at whitespace or '/': <br/> is "br", as <br /> is.
        std::size_t index = 0;
        while (index < tag_source.size() && !is_space(tag_source[index]) && tag_source[index] != '/') index++;
        return tag_source.substr(0, index);
    }

    // Scans source_[from..to); tags still open at `to` stay on the stack.
    template <bool Rules>
    void scan(int from, int to, bool& is_tag_started, int& tag_start_position) {
        const std::string_view source = source_;
        const int pgl = to;
        const bool is_xml = is_xml_;

        tag_start_position = 0;
        is_tag_started = false;

        int i = from;
        while (i < pgl) {
            if (!is_tag_started) {
                // Outside a tag only '<' matters.
                const std::size_t lt = source.find('<', std::size_t(i));
                if (lt == std::string_view::npos || lt >= std::size_t(pgl)) break;
                i = int(lt);

                if (source.substr(std::size_t(i) + 1).starts_with("!--")) {
                    // <!-- ... -->: one "!--" tag; its content is not markup and nests nothing.
                    const int comment_end = find_comment_end(source, i + 4);
                    const int comment_parent = stack_top_ > 0 ? stack_[std::size_t(stack_top_ - 1)] : -1;
                    const int depth = comment_parent >= 0 ? states_[std::size_t(comment_parent)].depth + 1 : 0;

                    TagData& tag = tags_[std::size_t(count_)];
                    tag = TagData{};
                    tag.start = i;
                    tag.end = comment_end;
                    tag.inner_start = comment_end + 1;
                    tag.inner_end = 0;
                    tag.parent = comment_parent;
                    tag.depth = std::int16_t(depth);
                    tag.key = std::int16_t(int(HtmlTag::Comment));
                    tag.custom = -1;

                    states_[std::size_t(count_)] = TagState{int(HtmlTag::Comment), depth, false, -1};
                    count_++;

                    i = comment_end + 1;
                    continue;
                }

                // As in HTML5, '<' starts markup only before a letter, '/', '!' or '?'.
                if (!is_markup_start(source, std::size_t(i))) {
                    i++;
                    continue;
                }

                is_tag_started = true;
                tag_start_position = i + 1;
                i++;
                continue;
            }

            // Inside a tag only '<', '=' and '>' matter.
            const std::size_t next = source.find_first_of(std::string_view("<=>", 3), std::size_t(i));
            if (next == std::string_view::npos || next >= std::size_t(pgl)) break;

            i = int(next);
            const char current = source[std::size_t(i)];

            if (current == '<') {
                // An unfinished tag: start over at this '<' (it may also start a comment).
                is_tag_started = false;
                continue;
            }

            if (current == '=') {
                // A quoted attribute value may contain '<' and '>': jump over it entirely.
                int q = i + 1;
                while (q < pgl && is_space(source[std::size_t(q)])) q++;
                if (q < pgl && (source[std::size_t(q)] == '"' || source[std::size_t(q)] == '\'')) {
                    const char quote = source[std::size_t(q)];
                    const std::size_t close_quote = source.find(quote, std::size_t(q) + 1);
                    if (close_quote != std::string_view::npos && close_quote < std::size_t(pgl)) i = int(close_quote);
                }
            } else {  // '>'
                if (source[std::size_t(tag_start_position)] == '/') {
                    const std::string_view closing_span =
                        trim(source.substr(std::size_t(tag_start_position) + 1,
                                           std::size_t(i - tag_start_position - 1)));
                    if (!closing_span.empty()) {
                        const int closing_key = lookup_tag_name(closing_span);
                        const int closing_name = closing_key >= 0 ? -1 : find_custom_name(closing_span);
                        bool repaired = false;

                        while (true) {
                            const bool any_open =
                                closing_key >= 0
                                    ? open_in_list_[std::size_t(closing_key)] > 0
                                    : (closing_name >= 0 && custom_open_.count(closing_name) != 0 &&
                                       custom_open_[closing_name] > 0);

                            bool matched = false;
                            bool repair_done = false;

                            for (int j = any_open ? last_open_ : -1; j >= 0; j = states_[j].prev_open) {
                                const bool is_match = closing_key >= 0 ? states_[j].key == closing_key
                                                                       : tags_[j].custom == closing_name;
                                if (!is_match) continue;

                                const int inner_end = tag_start_position - 2;

                                // Tags left open inside it: if the optional-end-tag rules apply to
                                // one of them, scan its part again with the rules.
                                if (!Rules && !is_xml && !repaired && stack_top_ > 0 &&
                                    stack_[std::size_t(stack_top_ - 1)] > j) {
                                    int above = stack_top_ - 1;
                                    while (above > 0 && stack_[std::size_t(above - 1)] > j) above--;

                                    const int target = first_misnested(above, stack_top_);
                                    if (target >= 0) {
                                        repair(target, tag_start_position - 1);
                                        repaired = true;
                                        // match the end tag again, to what is open now
                                        repair_done = true;
                                        break;
                                    }
                                }

                                tags_[j].inner_end = inner_end;
                                tags_[j].end = i;

                                while (stack_top_ > 0 && stack_[std::size_t(stack_top_ - 1)] >= j) {
                                    const int popped_key = states_[std::size_t(stack_[std::size_t(--stack_top_)])].key;
                                    if (popped_key == int(HtmlTag::P)) open_p_--;
                                    if (Rules && popped_key >= 0) open_by_key_[std::size_t(popped_key)]--;
                                }

                                truncate_open_list(j, inner_end);
                                matched = true;
                                break;
                            }

                            if (repair_done) continue;  // the end tag is matched again
                            (void)matched;
                            break;
                        }
                    }
                } else {
                    const std::string_view tag_source =
                        source.substr(std::size_t(tag_start_position), std::size_t(i - tag_start_position));
                    const std::string_view tag_name = extract_tag_name(tag_source);

                    int key = lookup_tag_name(tag_name);
                    int custom = -1;
                    if (key < 0) custom = intern_lower_name(tag_name);

                    if (!is_xml && key >= 0) {
                        if (const ImpliedEndRule* rule = rules_.implied[std::size_t(key)]) {
                            if (Rules) {
                                if (rule->any_open(open_by_key_)) close_in_scope(*rule, tag_start_position - 2, true);
                            } else if (stack_top_ > 0) {
                                const int top_key = states_[std::size_t(stack_[std::size_t(stack_top_ - 1)])].key;
                                if (top_key >= 0 && rule->targets[std::size_t(top_key)])
                                    apply_rule_in_fast_mode(*rule, tag_start_position);
                            }
                        }

                        // A block element ends an open <p>.
                        if (open_p_ > 0 && rules_.closes_p[std::size_t(key)]) {
                            if (Rules) {
                                close_in_scope(*rules_.p_rule, tag_start_position - 2, true);
                            } else {
                                apply_rule_in_fast_mode(*rules_.p_rule, tag_start_position);
                            }
                        }
                    }

                    // The innermost open tag is the parent.
                    const int parent_index = stack_top_ > 0 ? stack_[std::size_t(stack_top_ - 1)] : -1;
                    const int level = parent_index >= 0 ? states_[std::size_t(parent_index)].depth + 1 : 0;

                    TagData& tag = tags_[std::size_t(count_)];
                    tag = TagData{};
                    tag.start = tag_start_position - 1;
                    tag.end = i;
                    tag.inner_start = i + 1;
                    tag.inner_end = 0;
                    tag.parent = parent_index;
                    tag.depth = std::int16_t(level);
                    tag.key = std::int16_t(key);
                    tag.custom = custom;

                    // Void elements (<br>, <img>, <meta>, ...) and declarations (<!doctype>) never
                    // contain anything.
                    const bool is_void = !is_xml && key >= 0 && rules_.void_tags[std::size_t(key)];

                    const std::string_view trimmed_source = trim_end(tag_source);
                    const bool is_self_closing = is_void || (!trimmed_source.empty() && trimmed_source.back() == '/');

                    if (is_void) {
                        states_[std::size_t(count_)] = TagState{key, level, false, -1};
                    } else {
                        states_[std::size_t(count_)] = TagState{key, level, is_self_closing, last_open_};
                        last_open_ = count_;

                        // how many open tags have this name
                        if (key >= 0) {
                            open_in_list_[std::size_t(key)]++;
                        } else {
                            adjust_open_custom(custom, 1);
                        }
                    }

                    count_++;

                    if (!is_self_closing) {
                        // Deeper than kMaxDepth the tag is not nested.
                        if (stack_top_ < kMaxDepth) {
                            // the stack is popped by moving stack_top_; the vector keeps its size
                            if (std::size_t(stack_top_) < stack_.size()) {
                                stack_[std::size_t(stack_top_)] = count_ - 1;
                            } else {
                                stack_.push_back(count_ - 1);
                            }
                            stack_top_++;
                            if (key == int(HtmlTag::P)) open_p_++;
                            if (Rules && key >= 0) open_by_key_[std::size_t(key)]++;
                        }

                        // <script>, <style>, <textarea>, <title>, ...: the content is text.
                        if (!is_xml && key >= 0 && !rules_.raw_text_ends[std::size_t(key)].empty()) {
                            i = find_raw_text_end(source, i + 1, rules_.raw_text_ends[std::size_t(key)]) - 1;
                        }
                    }
                }

                is_tag_started = false;
            }

            i++;
        }
    }

    std::string_view source_;
    const RuleTables& rules_;
    bool is_xml_ = false;

    std::vector<TagData>& tags_;      // the thread's scratch
    std::vector<TagState>& states_;   // the thread's scratch
    std::size_t max_tags_ = 0;
    std::vector<int> stack_;
    std::vector<int> open_by_key_;
    std::vector<int> open_in_list_;
    std::unordered_map<int, int> custom_open_;
    std::unordered_map<std::string, int> custom_index_;
    std::vector<std::string> custom_names_;

    int count_ = 0;
    int stack_top_ = 0;
    int last_open_ = -1;
    int open_p_ = 0;
    std::int64_t repair_budget_ = 0;
};

}  // namespace

ParseResult parse(std::string_view source) {
    return Parser(source).run();
}

}  // namespace fh::detail
