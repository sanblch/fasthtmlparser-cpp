// Probe: how many heap allocations the containers cost in the Find, CSS and
// XPath paths on the books page, and how much of a scrape is parse vs query.
//
//   g++ -std=c++20 -O3 -Iinclude research/alloc_probe.cpp build/libfh.a \
//       -o research/alloc_probe && ./research/alloc_probe fixtures

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include <fh/fasthtmlparser.hpp>

namespace {
  std::size_t allocations = 0;
  bool counting = false;
}

void *operator new(std::size_t size) {
  if (counting) allocations++;
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) {
  if (counting) allocations++;
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
  std::free(p);
}
void operator delete[](void *p) noexcept {
  std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
  std::free(p);
}

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

template <typename Task>
std::size_t allocations_of(Task &&task) {
  allocations = 0;
  counting = true;
  task();
  counting = false;
  return allocations;
}

void report(const char *name, double seconds, std::size_t allocs) {
  std::printf("%-34s %8.2f us %8zu allocs\n", name, seconds * 1e6, allocs);
}

int main(int argc, char **argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "fixtures";
  std::ifstream file(fixtures + "/books/index.html");
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string html = buffer.str();

  // Warm up the parser scratch and the compiled-query cache.
  {
    fh::Document warm = fh::Document::parse_borrowed(html);
    sink += warm.tag_count();
    sink += warm.css("article.product_pod").size();
    sink += warm.xpath("//article").size();
  }

  fh::Document doc = fh::Document::parse_borrowed(html);

  const fh::TagQuery pods_query =
      fh::TagQuery(fh::HtmlTag::Article).with_class("product_pod");

  const auto parse = [&] {
    fh::Document fresh = fh::Document::parse_borrowed(html);
    sink += fresh.tag_count();
  };
  const auto parse_and_tags = [&] {
    fh::Document fresh = fh::Document::parse_borrowed(html);
    sink += fresh.tags().size();
  };
  const auto find_all = [&] { sink += doc.find_all(pods_query).size(); };
  const auto find_view = [&] {
    std::size_t count = 0;
    for (const fh::Tag &tag : doc.find_all_view(pods_query)) count +=
        std::size_t(tag.index() + 1);
    sink += count;
  };

  const fh::CssQuery css_pods = fh::CssQuery::compile("article.product_pod");
  const fh::CssQuery css_title = fh::CssQuery::compile("h3 a");
  const fh::CssQuery css_price = fh::CssQuery::compile("p.price_color");
  const fh::CssQuery css_rating = fh::CssQuery::compile("p.star-rating");

  const auto css_all = [&] {
    sink += doc.css(css_pods).size();
    sink += doc.css(css_title).size();
    sink += doc.css(css_price).size();
    sink += doc.css(css_rating).size();
  };
  const auto css_descendant = [&] { sink += doc.css(css_title).size(); };
  const auto css_simple = [&] { sink += doc.css(css_pods).size(); };

  const fh::XPathQuery xp_pods = fh::XPathQuery::compile(
      "//article[contains(concat(' ', @class, ' '), ' product_pod ')]");
  const fh::XPathQuery xp_title = fh::XPathQuery::compile(".//h3/a/@title");
  const fh::XPathQuery xp_price = fh::XPathQuery::compile(
      ".//p[contains(concat(' ', @class, ' '), ' price_color ')]");
  const fh::XPathQuery xp_rating = fh::XPathQuery::compile(
      ".//p[contains(concat(' ', @class, ' '), ' star-rating ')]/@class");

  const auto xpath_all = [&] {
    for (const fh::Tag &pod : xp_pods.select_tags(doc)) {
      sink += pod.xpath_value(xp_title)->size();
      sink += pod.xpath_value(xp_price)->size();
      sink += pod.xpath_value(xp_rating)->size();
    }
  };
  const auto xpath_pods_only = [&] { sink += xp_pods.select_tags(doc).size(); };

  // The CSS tree (sibling links) is built lazily on the first combinator.
  doc.css(css_title);

  report("parse (borrowed)", measure(parse), allocations_of(parse));
  report("parse + Document::tags()", measure(parse_and_tags),
         allocations_of(parse_and_tags));
  report("find_all(article.product_pod)", measure(find_all),
         allocations_of(find_all));
  report("find_all_view(article.product_pod)", measure(find_view),
         allocations_of(find_view));
  report("css: 4 queries (scrape)", measure(css_all), allocations_of(css_all));
  report("css: article.product_pod", measure(css_simple),
         allocations_of(css_simple));
  report("css: h3 a (descendant)", measure(css_descendant),
         allocations_of(css_descendant));
  report("xpath: //article predicate", measure(xpath_pods_only),
         allocations_of(xpath_pods_only));
  report("xpath: 4 queries (scrape)", measure(xpath_all),
         allocations_of(xpath_all));

  std::printf("\nsink=%zu\n", sink);
  return 0;
}
