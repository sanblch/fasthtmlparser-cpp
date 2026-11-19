// Relative link resolution (a simplified port of UriCore.CorrectLink): character references are
// decoded, the link is made absolute against the current URL (or the page URL), and
// percent-escapes are kept as written.
#include "internal.hpp"

namespace fh::detail {

namespace {

std::string remove_dot_segments(std::string_view path) {
    std::string output;
    output.reserve(path.size());

    std::size_t i = 0;
    while (i < path.size()) {
        const std::string_view rest = path.substr(i);

        if (rest.starts_with("../")) {
            i += 3;
        } else if (rest.starts_with("./")) {
            i += 2;
        } else if (rest.starts_with("/./")) {
            i += 2;
        } else if (rest == "/.") {
            output.push_back('/');
            break;
        } else if (rest.starts_with("/../")) {
            i += 3;
            const std::size_t slash = output.rfind('/');
            output.erase(slash == std::string::npos ? 0 : slash);
        } else if (rest == "/..") {
            const std::size_t slash = output.rfind('/');
            output.erase(slash == std::string::npos ? 0 : slash);
            output.push_back('/');
            break;
        } else if (rest == "." || rest == "..") {
            break;
        } else {
            std::size_t next = rest.find('/', rest.starts_with('/') ? 1 : 0);
            if (next == std::string_view::npos) next = rest.size();
            output.append(rest.substr(0, next));
            i += next;
        }
    }

    return output;
}

}  // namespace

std::string resolve_link(std::string_view raw, std::string_view page_url, std::string_view current_url) {
    std::string value = std::string(trim(decode_html(raw)));
    const std::string base = current_url.empty() ? std::string(page_url) : std::string(current_url);

    if (value.empty()) return base;
    if (base.empty()) return value;

    // An absolute URL (a scheme before any '/', '?' or '#').
    const std::size_t colon = value.find(':');
    const std::size_t separator = value.find_first_of("/?#");
    if (colon != std::string::npos && (separator == std::string::npos || colon < separator)) return value;

    std::string origin;
    std::string base_path;
    const std::size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        base_path = base;
    } else {
        const std::size_t path_start = base.find('/', scheme_end + 3);
        if (path_start == std::string::npos) {
            origin = base;
            base_path = "/";
        } else {
            origin = base.substr(0, path_start);
            base_path = base.substr(path_start);
        }
    }

    // A network-path reference: //host/path.
    if (value.rfind("//", 0) == 0) {
        const std::size_t scheme_pos = base.find(':');
        const std::string scheme = scheme_pos == std::string::npos ? "https" : base.substr(0, scheme_pos);
        return scheme + ":" + value;
    }

    // A fragment only: keep the base without its fragment.
    if (value[0] == '#') return base.substr(0, base.find('#')) + value;

    // Split the base path from its query and fragment.
    const std::size_t base_cut = base_path.find_first_of("?#");
    if (base_cut != std::string::npos) base_path.resize(base_cut);

    // The reference path and its own query / fragment.
    const std::size_t value_cut = value.find_first_of("?#");
    const std::string suffix = value_cut == std::string::npos ? "" : value.substr(value_cut);
    const std::string value_path = value_cut == std::string::npos ? value : value.substr(0, value_cut);

    std::string path;
    if (value_path.empty()) {
        path = base_path;
    } else if (value_path[0] == '/') {
        path = value_path;
    } else {
        const std::size_t slash = base_path.rfind('/');
        path = (slash == std::string::npos ? "/" : base_path.substr(0, slash + 1)) + value_path;
    }

    return origin + remove_dot_segments(path) + suffix;
}

}  // namespace fh::detail
