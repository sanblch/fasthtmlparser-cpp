// Benchmarks for the C++ port: the same task and corpus as the C# project
// (z3l.FastHtmlParser.Benchmarks) and the Rust port: parse the pages, then
// scrape books.toscrape.com (20 books: title, price, rating) and
// quotes.toscrape.com (10 quotes: text, author, tags) through the Find API, CSS
// selectors and XPath.
//
// Methodology follows the Rust benchmark: a warm-up that doubles the batch size
// until 100 ms, then five timed batches of 5x that size; the reported time is
// the median per call.
#include <fh/fasthtmlparser.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

  using Clock = std::chrono::steady_clock;

  // Keeps the results of the measured calls observable.
  volatile std::size_t sink = 0;

  std::string read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", path.c_str());
      std::exit(1);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
  }

  // The median time per call of five batches (as in the Rust benchmark).
  template <typename Task>
  double measure(Task &&task) {
    int iterations = 1;
    for (;;) {
      const auto start = Clock::now();
      for (int i = 0; i < iterations; i++) task();
      if (Clock::now() - start > std::chrono::milliseconds(100)) break;
      iterations *= 2;
      if (iterations > (1 << 20)) break;
    }

    std::vector<double> samples;
    samples.reserve(5);
    for (int batch = 0; batch < 5; batch++) {
      const int calls = iterations * 5;
      const auto start = Clock::now();
      for (int i = 0; i < calls; i++) task();
      const double elapsed =
          std::chrono::duration<double>(Clock::now() - start).count();
      samples.push_back(elapsed / calls);
    }
    return median(std::move(samples));
  }

  void report(const std::string &name, double seconds, std::size_t bytes) {
    std::printf("%-34s %10.1f us", name.c_str(), seconds * 1e6);
    if (bytes > 0 && seconds > 0) {
      std::printf("  %8.1f MB/s", double(bytes) / seconds / 1e6);
    }
    std::printf("\n");
  }

  // Parsing with a borrowed source: the same thing the C# and Rust documents
  // do.
  double measure_parse_borrowed(const std::string &page) {
    return measure([&] {
      fh::Document doc = fh::Document::parse_borrowed(page);
      sink += doc.tag_count();
    });
  }

  // Parsing a page that is already in memory: the document takes a copy of the
  // text, so the number is on the conservative side.
  double measure_parse_copied(const std::string &page) {
    return measure([&] {
      std::string copy = page;
      fh::Document doc = fh::Document::parse_owned(std::move(copy));
      sink += doc.tag_count();
    });
  }

  // --- the scraping task -----------------------------------------------------

  struct Book {
    std::string title;
    std::string price;
    std::string rating;

    bool operator==(const Book &other) const {
      return title == other.title && price == other.price
             && rating == other.rating;
    }
  };

  struct Quote {
    std::string text;
    std::string author;
    std::string tags;

    bool operator==(const Quote &other) const {
      return text == other.text && author == other.author && tags == other.tags;
    }
  };

  std::string last_word(const std::string &value) {
    const std::size_t space = value.rfind(' ');
    return space == std::string::npos ? value : value.substr(space + 1);
  }

  std::string required(const std::optional<fh::Tag> &tag, const char *what) {
    if (!tag) {
      std::fprintf(stderr, "missing %s\n", what);
      std::exit(1);
    }
    return tag->text();
  }

  std::vector<Book> books_find(const std::string &html) {
    fh::Document doc = fh::Document::parse_borrowed(html);
    std::vector<Book> result;
    result.reserve(20);

    for (const fh::Tag &pod : doc.find_all(
             fh::TagQuery(fh::HtmlTag::Article).with_class("product_pod"))) {
      const auto title = pod.find(fh::TagQuery(fh::HtmlTag::H3))
                             ->find(fh::TagQuery(fh::HtmlTag::A));
      const auto price =
          pod.find(fh::TagQuery(fh::HtmlTag::P).with_class("price_color"));
      const auto rating =
          pod.find(fh::TagQuery(fh::HtmlTag::P).with_class("star-rating"));
      result.push_back({title->attribute(fh::Attr::Title),
                        required(price, "price"),
                        last_word(rating->attribute(fh::Attr::Class))});
    }
    return result;
  }

  std::vector<Book> books_css(const std::string &html) {
    static const fh::CssQuery pods =
        fh::CssQuery::compile("article.product_pod");
    static const fh::CssQuery title = fh::CssQuery::compile("h3 a");
    static const fh::CssQuery price = fh::CssQuery::compile("p.price_color");
    static const fh::CssQuery rating = fh::CssQuery::compile("p.star-rating");

    fh::Document doc = fh::Document::parse_borrowed(html);
    std::vector<Book> result;
    result.reserve(20);

    for (const fh::Tag &pod : pods.select_all(doc)) {
      result.push_back(
          {pod.css_first(title)->attribute(fh::Attr::Title),
           required(pod.css_first(price), "price"),
           last_word(pod.css_first(rating)->attribute(fh::Attr::Class))});
    }
    return result;
  }

  std::vector<Book> books_xpath(const std::string &html) {
    static const fh::XPathQuery pods = fh::XPathQuery::compile(
        "//article[contains(concat(' ', @class, ' '), ' product_pod ')]");
    static const fh::XPathQuery title =
        fh::XPathQuery::compile(".//h3/a/@title");
    static const fh::XPathQuery price = fh::XPathQuery::compile(
        ".//p[contains(concat(' ', @class, ' '), ' price_color ')]");
    static const fh::XPathQuery rating = fh::XPathQuery::compile(
        ".//p[contains(concat(' ', @class, ' '), ' star-rating ')]/@class");

    fh::Document doc = fh::Document::parse_borrowed(html);
    std::vector<Book> result;
    result.reserve(20);

    for (const fh::Tag &pod : pods.select_tags(doc)) {
      result.push_back({*pod.xpath_value(title),
                        *pod.xpath_value(price),
                        last_word(*pod.xpath_value(rating))});
    }
    return result;
  }

  std::vector<Quote> quotes_find(const std::string &html) {
    fh::Document doc = fh::Document::parse_borrowed(html);
    std::vector<Quote> result;
    result.reserve(10);

    for (const fh::Tag &quote :
         doc.find_all(fh::TagQuery(fh::HtmlTag::Div).with_class("quote"))) {
      std::string tags;
      for (const fh::Tag &tag :
           quote.find_all(fh::TagQuery(fh::HtmlTag::A).with_class("tag"))) {
        if (!tags.empty()) tags.push_back(',');
        tags += tag.text();
      }
      result.push_back(
          {required(
               quote.find(fh::TagQuery(fh::HtmlTag::Span).with_class("text")),
               "text"),
           required(quote.find(
                        fh::TagQuery(fh::HtmlTag::Small).with_class("author")),
                    "author"),
           std::move(tags)});
    }
    return result;
  }

  std::vector<Quote> quotes_css(const std::string &html) {
    static const fh::CssQuery quotes = fh::CssQuery::compile("div.quote");
    static const fh::CssQuery text = fh::CssQuery::compile("span.text");
    static const fh::CssQuery author = fh::CssQuery::compile("small.author");
    static const fh::CssQuery tag = fh::CssQuery::compile("a.tag");

    fh::Document doc = fh::Document::parse_borrowed(html);
    std::vector<Quote> result;
    result.reserve(10);

    for (const fh::Tag &quote : quotes.select_all(doc)) {
      std::string tags;
      for (const fh::Tag &item : tag.select_all(quote)) {
        if (!tags.empty()) tags.push_back(',');
        tags += item.text();
      }
      result.push_back({required(quote.css_first(text), "text"),
                        required(quote.css_first(author), "author"),
                        std::move(tags)});
    }
    return result;
  }

}  // namespace

int main(int argc, char **argv) {
  std::string fixtures = "fixtures";
  bool check_only = false;
  bool xpath_only = false;
  for (int i = 1; i < argc; i++) {
    if (std::string_view(argv[i]) == "--check") {
      check_only = true;
    } else if (std::string_view(argv[i]) == "--only=xpath") {
      xpath_only = true;
    } else {
      fixtures = argv[i];
    }
  }

  const std::string quotes = read_file(fixtures + "/quotes/page1.html");
  const std::string laptops = read_file(fixtures + "/webscraper/laptops.html");
  const std::string books = read_file(fixtures + "/books/index.html");

  std::string books_x10;
  books_x10.reserve(books.size() * 10);
  for (int i = 0; i < 10; i++) books_x10 += books;

  std::string books_no_end_tags = books;
  for (const std::string tag : {"</li>", "</p>"}) {
    std::size_t position = 0;
    while ((position = books_no_end_tags.find(tag, position))
           != std::string::npos) {
      books_no_end_tags.erase(position, tag.size());
    }
  }

  // The three ways of scraping must return the same records before anything is
  // timed.
  const std::vector<Book> find_books = books_find(books);
  const std::vector<Book> css_books = books_css(books);
  const std::vector<Book> xpath_books = books_xpath(books);
  const std::vector<Quote> find_quotes = quotes_find(quotes);
  const std::vector<Quote> css_quotes = quotes_css(quotes);

  if (find_books.empty() || !(find_books == css_books)
      || !(find_books == xpath_books)) {
    std::fprintf(stderr,
                 "the Find, CSS and XPath scrapes returned different books\n");
    return 1;
  }
  if (find_quotes.empty() || !(find_quotes == css_quotes)) {
    std::fprintf(stderr,
                 "the Find and CSS scrapes returned different quotes\n");
    return 1;
  }

  if (check_only) {
    std::printf(
        "the Find, CSS and XPath scrapes return the same %zu books and %zu "
        "quotes\n",
        find_books.size(),
        find_quotes.size());
    return 0;
  }

  // A single scenario, for quick A/B runs of compiler flags.
  if (xpath_only) {
    report("books: XPath",
           measure([&] {
             const auto result = books_xpath(books);
             sink += result.size();
           }),
           books.size());
    return 0;
  }

  std::printf(
      "FastHtmlParser C++ — %zu tags in the books page, %zu books, %zu "
      "quotes\n\n",
      [&] {
        fh::Document doc(books);
        return doc.tag_count();
      }(),
      find_books.size(),
      find_quotes.size());

  std::printf("Parse (source borrowed, as in C# and Rust)\n");
  report("quotes 11 KB", measure_parse_borrowed(quotes), quotes.size());
  report("laptops 38 KB", measure_parse_borrowed(laptops), laptops.size());
  report("books 51 KB", measure_parse_borrowed(books), books.size());
  report(
      "books x10 510 KB", measure_parse_borrowed(books_x10), books_x10.size());
  report("books, no </li> </p>",
         measure_parse_borrowed(books_no_end_tags),
         books_no_end_tags.size());

  std::printf("\nParse (source copied into the document)\n");
  report("quotes 11 KB", measure_parse_copied(quotes), quotes.size());
  report("laptops 38 KB", measure_parse_copied(laptops), laptops.size());
  report("books 51 KB", measure_parse_copied(books), books.size());
  report("books x10 510 KB", measure_parse_copied(books_x10), books_x10.size());
  report("books, no </li> </p>",
         measure_parse_copied(books_no_end_tags),
         books_no_end_tags.size());

  std::printf("\nScrape (parse + extract)\n");
  report("books: Find",
         measure([&] {
           const auto result = books_find(books);
           sink += result.size();
         }),
         books.size());
  report("books: CSS",
         measure([&] {
           const auto result = books_css(books);
           sink += result.size();
         }),
         books.size());
  report("books: XPath",
         measure([&] {
           const auto result = books_xpath(books);
           sink += result.size();
         }),
         books.size());
  report("quotes: Find",
         measure([&] {
           const auto result = quotes_find(quotes);
           sink += result.size();
         }),
         quotes.size());
  report("quotes: CSS",
         measure([&] {
           const auto result = quotes_css(quotes);
           sink += result.size();
         }),
         quotes.size());

  return 0;
}
