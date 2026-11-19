// Tests for the C++ port, mirroring the checks of the C# test suite.
#include <fh/fasthtmlparser.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

  int failures = 0;

#define CHECK(condition)                               \
  do {                                                 \
    if (!(condition)) {                                \
      std::cerr << __FILE__ << ":" << __LINE__         \
                << ": CHECK failed: " #condition "\n"; \
      failures++;                                      \
    }                                                  \
  } while (false)

#define CHECK_EQ(actual, expected)                                           \
  do {                                                                       \
    const auto a_ = (actual);                                                \
    const auto e_ = (expected);                                              \
    if (!(a_ == e_)) {                                                       \
      std::cerr << __FILE__ << ":" << __LINE__                               \
                << ": CHECK_EQ failed: " #actual << " != " #expected         \
                << "\n  actual:   " << a_ << "\n  expected: " << e_ << "\n"; \
      failures++;                                                            \
    }                                                                        \
  } while (false)

  std::string read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::cerr << "cannot open " << path << "\n";
      std::exit(2);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  // --- core ------------------------------------------------------------------

  void test_basic_parse() {
    fh::Document doc = fh::Document::parse(
        "<html><body><div class=\"a "
        "b\"><span>Hello</span></div></body></html>");

    const auto div = fh::find_by_name(doc, fh::HtmlTag::Div);
    CHECK(div.has_value());
    CHECK_EQ(div->name(), std::string_view("div"));
    CHECK(div->is_class("a"));
    CHECK(div->is_class("b"));
    CHECK(!div->is_class("c"));
    CHECK_EQ(div->attribute("class"), std::string("a b"));
    CHECK_EQ(div->text(), std::string("Hello"));

    const auto span = div->find(fh::TagQuery(fh::HtmlTag::Span));
    CHECK(span.has_value());
    CHECK_EQ(span->text(), std::string("Hello"));
    CHECK_EQ(span->parent_index(), div->index());
    CHECK_EQ(span->depth(), div->depth() + 1);
  }

  void test_parse_borrowed() {
    // The borrowed document must produce exactly the same tags as the copying
    // one.
    const std::string html =
        "<ul><li>one<li>two</ul><p class=x>a &amp; b</p><my-tag>c</my-tag>";

    fh::Document borrowed = fh::Document::parse_borrowed(html);
    fh::Document copied = fh::Document::parse(html);

    CHECK_EQ(borrowed.source().data(), html.data());
    CHECK_EQ(borrowed.tag_count(), copied.tag_count());
    for (std::size_t i = 0; i < borrowed.tag_count(); i++) {
      const fh::Tag a = borrowed.tag(i);
      const fh::Tag b = copied.tag(i);
      CHECK_EQ(a.name(), b.name());
      CHECK_EQ(a.start(), b.start());
      CHECK_EQ(a.end(), b.end());
      CHECK_EQ(a.inner_start(), b.inner_start());
      CHECK_EQ(a.inner_end(), b.inner_end());
      CHECK_EQ(a.parent_index(), b.parent_index());
      CHECK_EQ(a.text(), b.text());
    }

    const auto item = borrowed.find(fh::TagQuery(fh::HtmlTag::Li));
    CHECK(item.has_value());
    CHECK_EQ(item->text(), std::string("one"));
    CHECK_EQ(borrowed.css_first("p.x")->text(), std::string("a & b"));
    const auto tag_text = borrowed.xpath_value("//my-tag/text()");
    CHECK(tag_text.has_value());
    CHECK_EQ(*tag_text, std::string("c"));
    CHECK_EQ(fh::find_by_name(borrowed, "my-tag")->text(), std::string("c"));
  }

  void test_entities() {
    CHECK_EQ(fh::html_decode("&amp;"), std::string("&"));
    CHECK_EQ(fh::html_decode("&lt;p&gt;"), std::string("<p>"));
    CHECK_EQ(fh::html_decode("&#65;&#x42;"), std::string("AB"));
    CHECK_EQ(fh::html_decode("&notit;"), std::string("\xC2\xACit;"));
    CHECK_EQ(fh::html_decode("&copy2024"),
             std::string("\xC2\xA9"
                         "2024"));
    CHECK_EQ(fh::html_decode("?a=1&copy=2", true), std::string("?a=1&copy=2"));
    CHECK_EQ(fh::html_decode("&#x20AC;"), std::string("\xE2\x82\xAC"));

    fh::Document doc("<p title=\"It&#39;s\">a &amp; b</p>");
    const auto p = fh::find_by_name(doc, fh::HtmlTag::P);
    CHECK(p.has_value());
    CHECK_EQ(p->attribute("title"), std::string("It's"));
    CHECK_EQ(p->text(), std::string("a & b"));
  }

  void test_comments_and_raw_text() {
    fh::Document doc(
        "<!doctype html><!-- x <div> y --><div>a<script>if (a < b) "
        "{}</script>b</div>");

    CHECK_EQ(doc.tag_count(), std::size_t(4));  // doctype, comment, div, script
    CHECK_EQ(doc.tag(1).name(), std::string_view("!--"));
    const auto div = fh::find_by_name(doc, fh::HtmlTag::Div);
    CHECK_EQ(div->text(), std::string("ab"));
    const auto script = fh::find_by_name(doc, fh::HtmlTag::Script);
    CHECK_EQ(script->text(), std::string("if (a < b) {}"));
  }

  void test_unclosed_tags() {
    fh::Document doc("<div><p>one<p>two");
    const auto paragraphs = doc.find_all(fh::TagQuery(fh::HtmlTag::P));
    CHECK_EQ(paragraphs.size(), std::size_t(2));
    CHECK_EQ(paragraphs[0].text(), std::string("one"));
    CHECK_EQ(paragraphs[1].text(), std::string("two"));

    fh::Document truncated("<div><p>text");
    const auto p = fh::find_by_name(truncated, fh::HtmlTag::P);
    CHECK(p.has_value());
    CHECK_EQ(p->text(), std::string("text"));
  }

  void test_misnested_lists() {
    fh::Document doc("<ul><li>one<li>two</ul>");
    const auto items = doc.find_all(fh::TagQuery(fh::HtmlTag::Li));
    CHECK_EQ(items.size(), std::size_t(2));
    CHECK_EQ(items[0].text(), std::string("one"));
    CHECK_EQ(items[1].text(), std::string("two"));
    CHECK_EQ(items[0].depth(), 1);

    fh::Document with_span("<ul><li><span>a<li>b</ul>");
    const auto items2 = with_span.find_all(fh::TagQuery(fh::HtmlTag::Li));
    CHECK_EQ(items2.size(), std::size_t(2));
    CHECK_EQ(items2[0].text(), std::string("a"));
    CHECK_EQ(items2[1].text(), std::string("b"));
  }

  void test_attributes() {
    fh::Document doc(
        "<a href=/p?a=1&b=2 title='see id=5' data-x=\"<p>\" id=t>link</a>"
        "<input disabled>");
    const auto a = fh::find_by_name(doc, fh::HtmlTag::A);
    CHECK_EQ(a->attribute("href"), std::string("/p?a=1&b=2"));
    CHECK_EQ(a->attribute("title"), std::string("see id=5"));
    CHECK_EQ(a->attribute("data-x"), std::string("<p>"));
    CHECK_EQ(a->attribute("id"), std::string("t"));
    CHECK_EQ(a->attribute("missing"), std::string(""));

    const auto input = fh::find_by_name(doc, fh::HtmlTag::Input);
    CHECK(input.has_value());
    CHECK_EQ(input->attribute("disabled"), std::string(""));
    CHECK_EQ(input->inner_start(), 0);  // void element

    // "id" inside another value is not an attribute
    fh::Document tricky("<div title=\"see id=5\">x</div>");
    CHECK_EQ(fh::find_by_name(tricky, fh::HtmlTag::Div)->attribute("id"),
             std::string(""));
  }

  void test_links() {
    fh::Document doc(
        "<a href=\"/a/b?x=1\">a</a><a href=\"c\">b</a><a href=\"#f\">c</a>"
        "<a href=\"//cdn/x\">d</a><a href=\"https://e/f\">e</a>",
        "https://site/dir/page.html");
    const auto links = doc.find_all(fh::TagQuery(fh::HtmlTag::A));
    CHECK_EQ(links.size(), std::size_t(5));
    CHECK_EQ(links[0].attribute_href(), std::string("https://site/a/b?x=1"));
    CHECK_EQ(links[1].attribute_href(), std::string("https://site/dir/c"));
    CHECK_EQ(links[2].attribute_href(),
             std::string("https://site/dir/page.html#f"));
    CHECK_EQ(links[3].attribute_href(), std::string("https://cdn/x"));
    CHECK_EQ(links[4].attribute_href(), std::string("https://e/f"));
  }

  void test_css() {
    fh::Document doc(
        "<div class=\"card\"><h3><a href=\"/1\" title='T1'>x</a></h3>"
        "<p class=\"price\">£10</p><p class=\"rating four\">4</p></div>"
        "<div class=\"card\"><h3><a href=\"/2\" title='T2'>y</a></h3>"
        "<p class=\"price\">£20</p></div>");

    CHECK_EQ(doc.css("div.card").size(), std::size_t(2));
    CHECK_EQ(doc.css("div.card h3 a").size(), std::size_t(2));
    CHECK_EQ(doc.css_first("p.price")->text(), std::string("£10"));
    CHECK_EQ(doc.css("p.rating").size(), std::size_t(1));
    CHECK_EQ(doc.css("[class~=four]").size(), std::size_t(1));
    CHECK_EQ(doc.css("[class^=rat]").size(), std::size_t(1));
    CHECK_EQ(doc.css("a[href='/2']").size(), std::size_t(1));
    CHECK_EQ(doc.css("div.card:has(> p.rating)").size(), std::size_t(1));
    CHECK_EQ(
        doc.css("div.card:nth-child(2) h3 a")[0].attribute(fh::Attr::Title),
        std::string("T2"));
    CHECK_EQ(doc.css("p.price:last-of-type").size(), std::size_t(1));
    CHECK_EQ(doc.css("p:last-of-type").size(), std::size_t(2));
    CHECK_EQ(doc.css("div > p:first-child").size(), std::size_t(0));

    const auto cards = doc.css("div.card");
    CHECK_EQ(cards[0].css("> h3 a")[0].attribute("href"), std::string("/1"));

    CHECK(doc.css_first("a[href='http' i]") == std::nullopt);
    CHECK(doc.css_first("a[title^='t']") == std::nullopt);
    CHECK(doc.css_first("a[title^='t' i]").has_value());
  }

  void test_xpath() {
    fh::Document doc(
        "<ul id=\"menu\"><li>a</li><li>b</li><li>c</li></ul>"
        "<div class=\"card\"><span class=\"price\">10</span></div>"
        "<div class=\"card\"><span class=\"price\">20</span></div>");

    CHECK_EQ(doc.xpath("//ul[@id='menu']/li").size(), std::size_t(3));
    CHECK_EQ(doc.xpath("//li[position() <= 2]").size(), std::size_t(2));
    CHECK_EQ(doc.xpath_values("//li[last()]/text()").size(), std::size_t(1));
    CHECK_EQ(*doc.xpath_value("//li[last()]/text()"), std::string("c"));
    CHECK_EQ(*doc.xpath_value("count(//li)"), std::string("3"));
    CHECK_EQ(*doc.xpath_value("//div[@class='card'][1]/span"),
             std::string("10"));
    CHECK(doc.xpath_values("//span/@class")
          == std::vector<std::string>({"price", "price"}));
    CHECK_EQ(doc.xpath("//div[span = '20']").size(), std::size_t(1));
    CHECK_EQ(*doc.xpath_value("normalize-space('  a   b ')"),
             std::string("a b"));
    CHECK_EQ(*doc.xpath_value("concat('a', 'b', 'c')"), std::string("abc"));
    CHECK_EQ(doc.xpath("//ul/li | //span").size(), std::size_t(5));

    const auto ul = fh::find_by_name(doc, fh::HtmlTag::Ul);
    CHECK_EQ(ul->xpath(".//li").size(), std::size_t(3));
    CHECK_EQ(*ul->xpath_value("count(li)"), std::string("3"));
  }

  // --- the scraping task from the article ------------------------------------

  struct Book {
    std::string title, price, rating;
    bool operator==(const Book &other) const {
      return title == other.title && price == other.price
             && rating == other.rating;
    }
  };

  struct Quote {
    std::string text, author, tags;
    bool operator==(const Quote &other) const {
      return text == other.text && author == other.author && tags == other.tags;
    }
  };

  std::string last_word(const std::string &value) {
    const std::size_t space = value.rfind(' ');
    return space == std::string::npos ? value : value.substr(space + 1);
  }

  std::vector<Book> books_find(const std::string &html) {
    fh::Document doc(html);
    std::vector<Book> result;
    for (const fh::Tag &pod : doc.find_all(
             fh::TagQuery(fh::HtmlTag::Article).with_class("product_pod"))) {
      result.push_back(
          {pod.find(fh::TagQuery(fh::HtmlTag::H3))
               ->find(fh::TagQuery(fh::HtmlTag::A))
               ->attribute(fh::Attr::Title),
           pod.find(fh::TagQuery(fh::HtmlTag::P).with_class("price_color"))
               ->text(),
           last_word(
               pod.find(fh::TagQuery(fh::HtmlTag::P).with_class("star-rating"))
                   ->attribute(fh::Attr::Class))});
    }
    return result;
  }

  std::vector<Book> books_css(const std::string &html) {
    fh::Document doc(html);
    std::vector<Book> result;
    for (const fh::Tag &pod : doc.css("article.product_pod")) {
      result.push_back(
          {pod.css_first("h3 a")->attribute(fh::Attr::Title),
           pod.css_first("p.price_color")->text(),
           last_word(
               pod.css_first("p.star-rating")->attribute(fh::Attr::Class))});
    }
    return result;
  }

  std::vector<Book> books_xpath(const std::string &html) {
    fh::Document doc(html);
    std::vector<Book> result;
    for (const fh::Tag &pod : doc.xpath("//article[contains(concat(' ', "
                                        "@class, ' '), ' product_pod ')]")) {
      result.push_back(
          {*pod.xpath_value(".//h3/a/@title"),
           *pod.xpath_value(
               ".//p[contains(concat(' ', @class, ' '), ' price_color ')]"),
           last_word(*pod.xpath_value(".//p[contains(concat(' ', @class, ' '), "
                                      "' star-rating ')]/@class"))});
    }
    return result;
  }

  std::vector<Quote> quotes_find(const std::string &html) {
    fh::Document doc(html);
    std::vector<Quote> result;
    for (const fh::Tag &quote :
         doc.find_all(fh::TagQuery(fh::HtmlTag::Div).with_class("quote"))) {
      std::string tags;
      for (const fh::Tag &tag :
           quote.find_all(fh::TagQuery(fh::HtmlTag::A).with_class("tag"))) {
        if (!tags.empty()) tags.push_back(',');
        tags += tag.text();
      }
      result.push_back(
          {quote.find(fh::TagQuery(fh::HtmlTag::Span).with_class("text"))
               ->text(),
           quote.find(fh::TagQuery(fh::HtmlTag::Small).with_class("author"))
               ->text(),
           std::move(tags)});
    }
    return result;
  }

  void test_scrape(const std::string &fixtures) {
    const std::string books = read_file(fixtures + "/books/index.html");
    const std::string quotes = read_file(fixtures + "/quotes/page1.html");

    const auto find_books = books_find(books);
    CHECK_EQ(find_books.size(), std::size_t(20));
    CHECK_EQ(find_books[0].title, std::string("A Light in the Attic"));
    CHECK_EQ(find_books[0].price, std::string("£51.77"));
    CHECK_EQ(find_books[0].rating, std::string("Three"));
    CHECK(find_books == books_css(books));
    CHECK(find_books == books_xpath(books));

    // No end tags left out must give the same records.
    std::string sloppy = books;
    for (const std::string tag : {"</li>", "</p>"}) {
      std::size_t position = 0;
      while ((position = sloppy.find(tag, position)) != std::string::npos)
        sloppy.erase(position, tag.size());
    }
    CHECK(find_books == books_find(sloppy));

    const auto find_quotes = quotes_find(quotes);
    CHECK_EQ(find_quotes.size(), std::size_t(10));
    CHECK_EQ(find_quotes[0].author, std::string("Albert Einstein"));
    CHECK(find_quotes[0].text.starts_with(
        "\xE2\x80\x9CThe world as we have created it"));
    CHECK_EQ(find_quotes[0].tags,
             std::string("change,deep-thoughts,thinking,world"));
  }

  // The CSS selectors and XPath expressions of the original test suite, run by
  // the C# library and saved in expected_queries.txt: the port must give the
  // same ids in the same order.
  std::vector<std::string> read_lines(const std::string &path) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
    return lines;
  }

  std::string join_ids(const std::vector<fh::Tag> &tags) {
    std::string result;
    for (const fh::Tag &tag : tags) {
      if (!result.empty()) result.push_back(',');
      result += tag.attribute(fh::Attr::Id);
    }
    return result;
  }

  void test_query_parity(const std::string &data) {
    fh::Document css_doc(read_file(data + "/css_page.html"));
    fh::Document xpath_doc(read_file(data + "/xpath_page.html"));

    std::vector<std::string> actual;
    for (const std::string &selector :
         read_lines(data + "/css_selectors.txt")) {
      if (selector.empty()) continue;
      std::string ids;
      try {
        ids = join_ids(css_doc.css(selector));
      } catch (const std::exception &) {
        ids = "ERROR";
      }
      actual.push_back("CSS\t" + selector + "\t" + ids);
    }

    for (const std::string &expression :
         read_lines(data + "/xpath_expressions.txt")) {
      if (expression.empty()) continue;
      std::string ids;
      try {
        ids = join_ids(xpath_doc.xpath(expression));
      } catch (const std::exception &) {
        ids = "ERROR";
      }
      actual.push_back("XP\t" + expression + "\t" + ids);
    }

    for (const std::string &expression :
         read_lines(data + "/xpath_values.txt")) {
      if (expression.empty()) continue;
      std::string value;
      try {
        const auto result = xpath_doc.xpath_value(expression);
        value = result ? *result : "<null>";
        for (char &c : value) {
          if (c == '\n') c = ' ';
        }
      } catch (const std::exception &) {
        value = "ERROR";
      }
      actual.push_back("VAL\t" + expression + "\t" + value);
    }

    const std::vector<std::string> expected =
        read_lines(data + "/expected_queries.txt");
    CHECK_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size() && i < expected.size(); i++) {
      if (actual[i] != expected[i]) {
        std::cerr << "parity mismatch at line " << i + 1
                  << "\n  expected: " << expected[i]
                  << "\n  actual:   " << actual[i] << "\n";
        failures++;
      }
    }
  }

}  // namespace

int main(int argc, char **argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "fixtures";
  const std::string data = argc > 2 ? argv[2] : "tests/data";

  test_basic_parse();
  test_parse_borrowed();
  test_entities();
  test_comments_and_raw_text();
  test_unclosed_tags();
  test_misnested_lists();
  test_attributes();
  test_links();
  test_css();
  test_xpath();
  test_scrape(fixtures);
  test_query_parity(data);

  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }

  std::fprintf(stderr, "%d check(s) failed\n", failures);
  return 1;
}
