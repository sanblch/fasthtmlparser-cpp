// HTML -> text: HtmlToTextCore, ClearTagsLineBreaks and the raw-text / comment helpers of ParsingCore.
#include "internal.hpp"

namespace fh::detail {

namespace {

enum class LineBreakKind : std::uint8_t {
    None,          // inline tag (b, span, a, ...): removed, words are not split
    Space,         // table cell: word separator
    Line,          // <br> and block elements (div, li, tr, ...): new line
    Paragraph,     // p, h1-h6: new paragraph (blank line)
    SkipContent,   // script, style, noscript: contents are not text
};

constexpr const char* kTagNameEnd = " \t\r\n\f/";

struct LineBreakWriter {
    char* buffer;
    std::size_t length = 0;
    int pending_breaks = 0;
    bool pending_space = false;

    void char_(char c) {
        if (is_space(c)) {
            pending_space = true;
            return;
        }

        if (length > 0) {
            if (pending_breaks > 0) {
                const int count = std::min(pending_breaks, 2);  // at most one blank line
                for (int n = 0; n < count; n++) buffer[length++] = '\n';
            } else if (pending_space) {
                buffer[length++] = ' ';
            }
        }

        pending_breaks = 0;
        pending_space = false;
        buffer[length++] = c;
    }

    void break_(int count) { pending_breaks = std::max(pending_breaks, count); }
    void line_break() { pending_breaks++; }
};

struct TextTables {
    LineBreakKind kinds[kTagCount];
};

const TextTables& text_tables() {
    static const TextTables tables = [] {
        TextTables t{};
        for (int i = 0; i < kTagCount; i++) t.kinds[i] = LineBreakKind::None;

        const auto set = [&t](HtmlTag tag, LineBreakKind kind) { t.kinds[int(tag)] = kind; };

        for (HtmlTag tag : {HtmlTag::P, HtmlTag::H1, HtmlTag::H2, HtmlTag::H3, HtmlTag::H4, HtmlTag::H5, HtmlTag::H6})
            set(tag, LineBreakKind::Paragraph);

        for (HtmlTag tag : {
                 HtmlTag::Br, HtmlTag::Div, HtmlTag::Li, HtmlTag::Ul, HtmlTag::Ol, HtmlTag::Dl, HtmlTag::Dt,
                 HtmlTag::Dd, HtmlTag::Table, HtmlTag::Caption, HtmlTag::Tr, HtmlTag::Thead, HtmlTag::Tbody,
                 HtmlTag::Tfoot, HtmlTag::Section, HtmlTag::Article, HtmlTag::Aside, HtmlTag::Nav, HtmlTag::Header,
                 HtmlTag::Footer, HtmlTag::Main, HtmlTag::Address, HtmlTag::Blockquote, HtmlTag::Pre, HtmlTag::Hr,
                 HtmlTag::Figure, HtmlTag::Figcaption, HtmlTag::Form, HtmlTag::Fieldset, HtmlTag::Legend,
                 HtmlTag::Details, HtmlTag::Summary, HtmlTag::Dialog, HtmlTag::Option, HtmlTag::Html, HtmlTag::Head,
                 HtmlTag::Body, HtmlTag::Title, HtmlTag::Hgroup, HtmlTag::Search, HtmlTag::Menu, HtmlTag::Center,
                 HtmlTag::Dir, HtmlTag::Listing, HtmlTag::Xmp, HtmlTag::Plaintext, HtmlTag::Multicol,
                 HtmlTag::Frameset, HtmlTag::Marquee})
            set(tag, LineBreakKind::Line);

        set(HtmlTag::Td, LineBreakKind::Space);
        set(HtmlTag::Th, LineBreakKind::Space);

        for (HtmlTag tag : {HtmlTag::Script, HtmlTag::Style, HtmlTag::Noscript, HtmlTag::Template, HtmlTag::Noframes,
                            HtmlTag::Noembed})
            set(tag, LineBreakKind::SkipContent);

        return t;
    }();
    return tables;
}

}  // namespace

bool is_markup_start(std::string_view value, std::size_t i) {
    if (i + 1 >= value.size()) return false;
    const char next = value[i + 1];
    return is_ascii_alpha(next) || next == '/' || next == '!' || next == '?';
}

int find_comment_end(std::string_view source, int content_start) {
    const std::size_t start = std::min<std::size_t>(std::size_t(content_start), source.size());
    const std::string_view content = source.substr(start);

    if (!content.empty() && content[0] == '>') return content_start;
    if (content.size() >= 2 && content[0] == '-' && content[1] == '>') return content_start + 1;

    std::size_t from = 0;
    while (from < content.size()) {
        const std::size_t dashes = content.find("--", from);
        if (dashes == std::string_view::npos) break;

        const std::size_t position = dashes + 2;
        if (position < content.size() && content[position] == '>') return content_start + int(position);
        if (position + 1 < content.size() && content[position] == '!' && content[position + 1] == '>')
            return content_start + int(position) + 1;

        from = dashes + 1;
    }

    return int(source.size()) - 1;
}

int find_raw_text_end(std::string_view source, int from, std::string_view end_tag) {
    int position_from = from;
    while (position_from >= 0 && std::size_t(position_from) < source.size()) {
        const std::size_t found = ifind(source, end_tag, std::size_t(position_from));
        if (found == std::string_view::npos) break;

        const std::size_t after = found + end_tag.size();
        // "</script>", "</script >", "</script/>" end it; "</scripts" does not.
        if (after >= source.size() || source[after] == '>' || source[after] == '/' || is_space(source[after])) {
            return int(found);
        }

        position_from = int(after);
    }

    return int(source.size());
}

std::string_view invisible_content_end(std::string_view tag) {
    // Called for every tag while extracting text: no dictionary, the six names are compared directly.
    if (tag.empty()) return {};
    const char first = ascii_lower(tag[0]);
    if (first != 's' && first != 't' && first != 'n') return {};

    std::size_t name_length = 1;
    while (name_length < tag.size() && name_length <= 8 && is_ascii_alpha(tag[name_length])) name_length++;
    if (name_length < tag.size() && !(tag[name_length] == '/' || is_space(tag[name_length]))) return {};

    const std::string_view name = tag.substr(0, name_length);
    std::string_view end;

    switch (name_length) {
        case 5:
            if (iequals(name, "style")) end = "</style";
            break;
        case 6:
            if (iequals(name, "script")) end = "</script";
            break;
        case 7:
            if (iequals(name, "noembed")) end = "</noembed";
            break;
        case 8:
            if (iequals(name, "noscript")) end = "</noscript";
            else if (iequals(name, "template")) end = "</template";
            else if (iequals(name, "noframes")) end = "</noframes";
            break;
        default:
            break;
    }

    if (!end.empty() && trim_end(tag).ends_with('/')) return {};  // <script src="x" />: no content
    return end;
}

int skip_markup(std::string_view value, std::size_t i) {
    const std::string_view rest = value.substr(i + 1);

    if (rest.starts_with("!--")) return find_comment_end(value, int(i) + 4);

    const std::size_t close = rest.find('>');
    if (close == std::string_view::npos) return -1;
    const std::size_t tag_end = i + 1 + close;

    if (const std::string_view end_tag = invisible_content_end(rest.substr(0, close)); !end_tag.empty()) {
        return find_raw_text_end(value, int(tag_end) + 1, end_tag) - 1;
    }

    return int(tag_end);
}

std::size_t strip_tags(std::string_view value, char* destination) {
    std::size_t dst = 0;
    bool prev_whitespace = true;  // left trim

    for (std::size_t i = 0; i < value.size(); i++) {
        const char c = value[i];

        if (c == '<' && is_markup_start(value, i)) {
            const int last = skip_markup(value, i);
            if (last < 0) break;
            i = std::size_t(last);
        } else if (is_space(c)) {
            if (!prev_whitespace) {
                destination[dst++] = ' ';
                prev_whitespace = true;
            }
        } else {
            destination[dst++] = c;
            prev_whitespace = false;
        }
    }

    if (dst > 0 && destination[dst - 1] == ' ') dst--;

    return dst;
}

std::string clear_tags(std::string_view value) {
    if (value.empty()) return {};

    std::string buffer(value.size(), '\0');
    const std::size_t length = strip_tags(value, buffer.data());
    if (length == 0) return {};

    const std::string_view stripped = std::string_view(buffer.data(), length);
    if (stripped.find('&') != std::string_view::npos) return decode_html(stripped);
    return std::string(stripped);
}

std::string clear_tags_line_breaks(std::string_view value) {
    if (value.empty()) return {};

    // Output never exceeds 2x the input: a line break is written only before a text char
    // and needs a tag (>= 3 chars) in the input.
    std::string buffer(value.size() * 2 + 8, '\0');
    LineBreakWriter writer{buffer.data()};

    const TextTables& tables = text_tables();
    char decoded[4];
    std::size_t i = 0;

    while (i < value.size()) {
        const char c = value[i];

        if (c == '&') {
            int written = 0;
            const int consumed = decode_reference(value.substr(i + 1), decoded, written);
            if (consumed > 0) {
                for (int k = 0; k < written; k++) writer.char_(decoded[k]);
                i += 1 + std::size_t(consumed);
                continue;
            }

            writer.char_(c);
            i++;
            continue;
        }

        if (c != '<' || !is_markup_start(value, i)) {
            writer.char_(c);
            i++;
            continue;
        }

        const std::string_view rest = value.substr(i);

        if (rest.starts_with("<!--")) {
            const std::size_t comment_end = rest.substr(4).find("-->");
            if (comment_end == std::string_view::npos) break;
            i += 4 + comment_end + 3;
            continue;
        }

        const std::size_t close = rest.find('>');
        if (close == std::string_view::npos) break;  // unterminated tag: the rest is markup

        const std::string_view tag = rest.substr(1, close - 1);
        i += close + 1;

        const bool is_closing = !tag.empty() && tag[0] == '/';
        std::string_view name = is_closing ? tag.substr(1) : tag;
        const std::size_t name_length = name.find_first_of(std::string_view(kTagNameEnd, 6));
        if (name_length != std::string_view::npos) name = name.substr(0, name_length);

        const int key = lookup_tag_name(name);
        if (key < 0) continue;  // unknown tag: just removed

        switch (tables.kinds[key]) {
            case LineBreakKind::Line:
                if (key == int(HtmlTag::Br)) {
                    writer.line_break();  // every <br> counts
                } else {
                    writer.break_(1);
                }
                break;

            case LineBreakKind::Paragraph:
                writer.break_(2);
                break;

            case LineBreakKind::Space:
                writer.char_(' ');
                break;

            case LineBreakKind::SkipContent:
                if (!is_closing && !trim_end(tag).ends_with('/')) {
                    const std::string end_tag = "</" + std::string(kTagNames[key]);
                    const std::size_t end = ifind(value, end_tag, i);
                    i = end == std::string_view::npos ? value.size() : end;
                }
                break;

            case LineBreakKind::None:
                break;
        }
    }

    return std::string(buffer.data(), writer.length);
}

bool is_raw_text(int key) {
    if (key < 0) return false;
    switch (HtmlTag(key)) {
        case HtmlTag::Script:
        case HtmlTag::Style:
        case HtmlTag::Textarea:
        case HtmlTag::Title:
        case HtmlTag::Xmp:
        case HtmlTag::Iframe:
        case HtmlTag::Noembed:
        case HtmlTag::Noframes:
            return true;
        default:
            return false;
    }
}

}  // namespace fh::detail
