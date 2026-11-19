// Research: eager vectors vs C++20 coroutine generators vs C++23 ranges views
// for find_all-style queries over the flat tag list.
//
//   g++ -std=c++20 -O3 -Iinclude research/ranges_bench.cpp build/libfh.a \
//       -o research/ranges_bench && ./research/ranges_bench fixtures
//
// Every variant yields the same tags; the benchmark reports the median time
// per call and the number of heap allocations the call makes.

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <new>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <version>
#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
#include <generator>
#define FH_HAVE_STD_GENERATOR 1
#endif

#include <fh/fasthtmlparser.hpp>

// --- allocation counter -----------------------------------------------------

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

// --- C++20 coroutine generator ---------------------------------------------

template <typename T>
class Generator {
 public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    T value{};
    Generator get_return_object() {
      return Generator{handle_type::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept {
      return {};
    }
    std::suspend_always final_suspend() noexcept {
      return {};
    }
    std::suspend_always yield_value(T value) {
      this->value = value;
      return {};
    }
    void return_void() {}
    void unhandled_exception() {
      std::terminate();
    }
  };

  explicit Generator(handle_type handle) : handle_(handle) {}
  Generator(Generator &&other) noexcept : handle_(other.handle_) {
    other.handle_ = {};
  }
  Generator(const Generator &) = delete;
  ~Generator() {
    if (handle_) handle_.destroy();
  }

  // Pulls the next value; false when the coroutine is done.
  bool next(T &out) {
    if (!handle_) return false;
    handle_.resume();
    if (handle_.done()) {
      handle_ = {};
      return false;
    }
    out = handle_.promise().value;
    return true;
  }

 private:
  handle_type handle_;
};

Generator<fh::Tag> lazy_find_all(const fh::Document &doc,
                                 const fh::TagQuery &query) {
  for (std::size_t i = 0; i < doc.tag_count(); i++) {
    const fh::Tag tag = doc.tag(static_cast<int>(i));
    if (tag.is(query)) co_yield tag;
  }
}

// --- a lazy view straight over the parsed tag data -------------------------

// Matches the eager find_all exactly, allocates nothing and does not need
// Document::tags() (the vector of Tag handles built on first use).
class FindView : public std::ranges::view_interface<FindView> {
 public:
  class iterator {
   public:
    using value_type = fh::Tag;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    iterator() = default;
    iterator(const fh::Document *document,
             const fh::TagQuery *query,
             int index,
             int stop)
        : document_(document), query_(query), index_(index), stop_(stop) {
      skip();
    }

    fh::Tag operator*() const {
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
      while (index_ < stop_ && !document_->tag(index_).is(*query_)) index_++;
    }

    const fh::Document *document_ = nullptr;
    const fh::TagQuery *query_ = nullptr;
    int index_ = 0;
    int stop_ = 0;
  };

  FindView(const fh::Document &document, const fh::TagQuery &query)
      : document_(&document), query_(&query), stop_(int(document.tag_count())) {}

  iterator begin() const {
    return iterator(document_, query_, 0, stop_);
  }
  iterator end() const {
    return iterator(document_, query_, stop_, stop_);
  }

 private:
  const fh::Document *document_ = nullptr;
  const fh::TagQuery *query_ = nullptr;
  int stop_ = 0;
};

// --- measurement ------------------------------------------------------------

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

template <typename Range>
std::size_t consume(Range &&range) {
  std::size_t count = 0;
  for (const fh::Tag &tag : range) {
    sink += std::size_t(tag.index() + 1);
    count++;
  }
  return count;
}

int main(int argc, char **argv) {
  const std::string fixtures = argc > 1 ? argv[1] : "fixtures";
  std::ifstream file(fixtures + "/books/index.html");
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const std::string html = buffer.str();

  fh::Document doc = fh::Document::parse_borrowed(html);
  namespace views = std::views;
  (void)doc.tags();  // build once, as the range view needs it

  struct Case {
    const char *name;
    fh::TagQuery query;
  };
  const Case cases[] = {
      {"article.product_pod", fh::TagQuery(fh::HtmlTag::Article).with_class(
                                  "product_pod")},
      {"a", fh::TagQuery(fh::HtmlTag::A)},
      {"all tags", fh::TagQuery{}},
  };

  std::printf("%-18s %-9s %10s %10s %8s\n", "query", "variant", "us/call",
              "MB/s?", "allocs");
  for (const Case &item : cases) {
    const fh::TagQuery &q = item.query;

    const auto eager = [&] {
      std::vector<fh::Tag> result;
      for (std::size_t i = 0; i < doc.tag_count(); i++) {
        const fh::Tag tag = doc.tag(int(i));
        if (tag.is(q)) result.push_back(tag);
      }
      sink += result.size();
    };
    const auto lazy = [&] {
      Generator<fh::Tag> generator = lazy_find_all(doc, q);
      fh::Tag tag;
      std::size_t count = 0;
      while (generator.next(tag)) {
        sink += std::size_t(tag.index() + 1);
        count++;
      }
      sink += count;
    };
    const auto ranged = [&] {
      sink += consume(doc.tags() | views::filter(
                                     [&](const fh::Tag &tag) {
                                       return tag.is(q);
                                     }));
    };

    const std::size_t n = consume(doc.tags() | views::filter(
                                                [&](const fh::Tag &tag) {
                                                  return tag.is(q);
                                                }));
    std::printf("%-18s %-9s %10.1f %10s %8zu   (%zu results)\n", item.name,
                "eager", measure(eager) * 1e6, "", allocations_of(eager), n);
    std::printf("%-18s %-9s %10.1f %10s %8zu\n", item.name, "generator",
                measure(lazy) * 1e6, "", allocations_of(lazy));
    std::printf("%-18s %-9s %10.1f %10s %8zu\n", item.name, "ranges",
                measure(ranged) * 1e6, "", allocations_of(ranged));

    const auto viewed = [&] {
      sink += consume(FindView(doc, q));
    };
    std::printf("%-18s %-9s %10.1f %10s %8zu\n", item.name, "view",
                measure(viewed) * 1e6, "", allocations_of(viewed));

#ifdef FH_HAVE_STD_GENERATOR
    const auto std_generator = [&] {
      auto make = [](const fh::Document &document,
                     const fh::TagQuery &query) -> std::generator<fh::Tag> {
        for (std::size_t i = 0; i < document.tag_count(); i++) {
          const fh::Tag tag = document.tag(int(i));
          if (tag.is(query)) co_yield tag;
        }
      };
      std::size_t count = 0;
      for (const fh::Tag &tag : make(doc, q)) {
        sink += std::size_t(tag.index() + 1);
        count++;
      }
      sink += count;
    };
    std::printf("%-18s %-9s %10.1f %10s %8zu\n", item.name, "std::gen",
                measure(std_generator) * 1e6, "",
                allocations_of(std_generator));
#endif
  }

  std::printf("\nsink=%zu\n", sink);
  return 0;
}
