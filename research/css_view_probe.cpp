// Probe: can a CSS selection be expressed lazily as "the tags for which
// is_css(query) holds", and what does that cost?
//
//   g++ -std=c++20 -O3 -Iinclude research/css_view_probe.cpp build/libfh.a \
//       -o research/css_view_probe && ./research/css_view_probe fixtures

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fh/fasthtmlparser.hpp>

using Clock = std::chrono::steady_clock;
std::size_t sink = 0;

template <typename Task>
double measure(Task &&task) {
  int iterations = 1;
  for (;;) {
    const auto start = Clock::now();
    for (int i = 0; i < iterations; i++) task();
    if (Clock::now() - start > std::chrono::milliseconds(50)) break;
    iterations *= 2;
  }
  std::vector<double> samples;
  for (int batch = 0; batch < 5; batch++) {
    const auto start = Clock::now();
    for (int i = 0; i < iterations * 5; i++) task();
    samples.push_back(
        std::chrono::duration<double>(Clock::now() - start).count()
        / (iterations * 5));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

int main(int argc, char **argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "fixtures";
  std::ifstream file(fixtures + "/books/index.html");
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string html = buffer.str();

  fh::Document doc = fh::Document::parse_borrowed(html);
  namespace views = std::views;
  (void)doc.tags();

  const char *selectors[] = {
      "article.product_pod",
      "h3 a",
      "p.price_color",
      "a",
      "article > h3",
      ":scope",
      "div",
      "p:first-child",
  };

  std::printf("%-22s %7s %7s %10s %10s\n",
              "selector",
              "eager",
              "lazy",
              "us eager",
              "us lazy");
  for (const char *text : selectors) {
    const fh::CssQuery query = fh::CssQuery::compile(text);

    const auto eager = [&] { sink += doc.css(query).size(); };
    const auto lazy = [&] {
      std::size_t count = 0;
      for (const fh::Tag &tag :
           doc.tags() | views::filter([&](const fh::Tag &t) {
             return t.is_css(query);
           })) {
        sink += std::size_t(tag.index() + 1);
        count++;
      }
      sink += count;
    };

    const std::size_t eager_count = doc.css(query).size();
    std::size_t lazy_count = 0;
    for (const fh::Tag &tag : doc.tags()) {
      if (tag.is_css(query)) lazy_count++;
    }
    std::printf("%-22s %7zu %7zu %10.2f %10.2f\n",
                text,
                eager_count,
                lazy_count,
                measure(eager) * 1e6,
                measure(lazy) * 1e6);
  }
  std::printf("sink=%zu\n", sink);
  return 0;
}
