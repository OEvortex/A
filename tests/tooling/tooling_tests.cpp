#include <iostream>
#include <string>

#include "diagnostics/diagnostics.hpp"
#include "tooling/docgen.hpp"
#include "tooling/formatter.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void checkEqual(const std::string& actual, const std::string& expected,
                const std::string& message) {
  if (actual == expected) return;
  std::cerr << "FAIL: " << message << "\n";
  std::cerr << "--- expected ---\n" << expected;
  std::cerr << "--- actual ---\n" << actual;
  std::cerr << "----------------\n";
  ++failures;
}

const char* kSource = R"(## Adds two numbers.
## The result is an i32.
fn add( a:i32,b:i32 )->i32:
  return a+b

# A leading comment.
var hex=0xFF # trailing comment
var big=1_000
var text="a\tb"
)";

const char* kFormatted = R"(## Adds two numbers.
## The result is an i32.
fn add(a: i32, b: i32) -> i32:
    return a + b

# A leading comment.
var hex = 0xFF  # trailing comment
var big = 1_000
var text = "a\tb"
)";

void testFormatter() {
  a::DiagnosticSink sink;
  a::tooling::FormatResult first = a::tooling::formatSource(kSource, sink);
  check(first.ok, "formatter succeeds on valid source");
  checkEqual(first.text, kFormatted, "formatter canonicalizes spacing and comments");

  a::DiagnosticSink secondSink;
  a::tooling::FormatResult second = a::tooling::formatSource(first.text, secondSink);
  check(second.ok, "formatted output parses");
  checkEqual(second.text, first.text, "formatter is idempotent");

  a::DiagnosticSink badSink;
  a::tooling::FormatResult bad = a::tooling::formatSource("fn broken(:\n", badSink);
  check(!bad.ok && bad.text.empty(), "formatter refuses to emit output for broken input");

  a::DiagnosticSink commentSink;
  const std::string commented =
      "fn f() -> void:\n"
      "    # keep me\n"
      "    var values = [1, 2]  # inline\n";
  a::tooling::FormatResult commentResult = a::tooling::formatSource(commented, commentSink);
  check(commentResult.ok, "formatter handles interior comments");
  check(commentResult.text.find("# keep me") != std::string::npos,
        "interior comment preserved");
  check(commentResult.text.find("# inline") != std::string::npos, "trailing comment preserved");
}

void testDocgen() {
  const std::string source = R"(
## Utility helpers.

## Adds two numbers.
##
## The result is an i32.
fn add(a: i32, b: i32) -> i32:
    return a + b

## A two-dimensional point.
class Point:
    ## Horizontal position.
    x: i32
    y: i32

    ## Squared distance from the origin.
    fn length_squared(self) -> i32:
        return self.x * self.x + self.y * self.y

## Available colors.
enum Color:
    ## The color red.
    Red
    Green

## The mathematical constant.
const PI: f64 = 3.14159
)";

  a::DiagnosticSink sink;
  a::tooling::DocOptions options;
  options.sourceName = "geometry.a";
  a::tooling::DocResult result = a::tooling::extractDocs(source, sink, options);
  check(result.ok, "doc generator succeeds on valid source");
  check(result.module.items.size() == 4, "doc generator indexes every declaration");

  const std::string markdown = a::tooling::renderMarkdown(result.module);
  check(markdown.find("# geometry") != std::string::npos, "markdown has a module title");
  check(markdown.find("## Functions") != std::string::npos, "markdown has a function section");
  check(markdown.find("Adds two numbers.") != std::string::npos, "function doc is rendered");
  check(markdown.find("fn add(a: i32, b: i32) -> i32") != std::string::npos,
        "function signature is rendered");
  check(markdown.find("Horizontal position.") != std::string::npos, "field doc is rendered");
  check(markdown.find("Squared distance from the origin.") != std::string::npos,
        "method doc is rendered");
  check(markdown.find("The color red.") != std::string::npos, "variant doc is rendered");
  check(markdown.find("The mathematical constant.") != std::string::npos,
        "constant doc is rendered");

  const std::string json = a::tooling::renderJson(result.module);
  check(json.find("\"module\":\"geometry\"") != std::string::npos, "json names the module");
  check(json.find("\"kind\":\"function\"") != std::string::npos, "json includes item kinds");
  check(json.back() == '\n', "json output is newline terminated");
}

}  // namespace

int main() {
  testFormatter();
  testDocgen();
  if (failures == 0) {
    std::cout << "tooling tests passed\n";
    return 0;
  }
  std::cerr << failures << " tooling test(s) failed\n";
  return 1;
}
