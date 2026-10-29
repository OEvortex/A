#include <iostream>
#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "ir/ir.hpp"
#include "name-resolution/module_loader.hpp"
#include "optimization/optimizer.hpp"
#include "type-checker/sema.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

struct Pipeline {
  a::ModuleGraph graph;
  a::SemaResult semaResult;
  a::ir::Program program;
};

bool buildProgram(const std::string& source, Pipeline& pipeline) {
  a::DiagnosticSink sink;
  a::ModuleLoaderOptions options;
  a::ModuleLoader loader(std::move(options), sink);
  pipeline.graph = loader.load("test.a", source);
  if (sink.hasErrors()) return false;
  a::Sema sema(pipeline.graph, sink);
  pipeline.semaResult = sema.analyze();
  if (sink.hasErrors()) return false;
  pipeline.program = a::ir::lowerProgram(pipeline.graph, pipeline.semaResult);
  return true;
}

std::string functionByName(const a::ir::Program& program, const std::string& needle) {
  for (const auto& function : program.functions) {
    if (contains(function.name, needle)) return a::ir::printFunction(function);
  }
  return {};
}

void testLowering() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn add(a: int, b: int) -> int:
    return a + b

total = 0
i = 0
while i < 3:
    total = total + i
    i = i + 1
print(total)
print(add(1, 2))
)",
                                pipeline);
  check(ok, "lowering: program analyzes");
  if (!ok) return;

  const std::string text = a::ir::printProgram(pipeline.program);
  check(contains(text, "fn @"), "lowering: functions are printed");
  check(contains(text, "param a"), "lowering: parameters become Param instructions");
  check(contains(text, "phi [%"), "lowering: loop headers contain phi nodes");
  check(contains(text, "branch %"), "lowering: conditional flow uses branches");
  check(!functionByName(pipeline.program, "add").empty(), "lowering: function symbols exist");
}

void testConstantFolding() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn constants() -> int:
    x = 2 + 3 * 4
    return x

print(constants())
)",
                                pipeline);
  check(ok, "fold: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::constantFold(pipeline.program);
  check(stats.constantsFolded >= 2, "fold: binary expressions are folded");
  const std::string text = functionByName(pipeline.program, "constants");
  check(contains(text, "const 14"), "fold: 2 + 3 * 4 becomes const 14");
}

void testDeadCodeElimination() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn unused(x: int) -> int:
    y = x * 2
    return x

print(unused(3))
)",
                                pipeline);
  check(ok, "dce: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::deadCodeElimination(pipeline.program);
  check(stats.deadInstructions >= 1, "dce: unused pure instruction is removed");
  const std::string text = functionByName(pipeline.program, "unused");
  check(!contains(text, "* %0"), "dce: dead multiplication no longer printed");
}

void testInlining() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn add(a: int, b: int) -> int:
    return a + b

print(add(1, 2))
)",
                                pipeline);
  check(ok, "inline: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::inlineFunctions(pipeline.program);
  check(stats.functionsInlined >= 1, "inline: single-block function is inlined");
  const std::string main = functionByName(pipeline.program, "main");
  check(!contains(main, "call add"), "inline: call site is replaced");
}

void testLoopInvariantMotion() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
total = 0
i = 0
while i < 3:
    total = total + i * 7
    i = i + 1
print(total)
)",
                                pipeline);
  check(ok, "licm: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::loopInvariantCodeMotion(pipeline.program);
  check(stats.loopsHoisted >= 1, "licm: loop-invariant constant is hoisted");
  const std::string main = functionByName(pipeline.program, "main");
  check(contains(main, "const 7"), "licm: hoisted constant remains in the preheader");
}

void testRunAll() {
  Pipeline pipeline;
  const bool ok = buildProgram(R"(
fn double(x: int) -> int:
    return x * 2

total = 0
i = 0
while i < 4:
    total = total + double(3)
    i = i + 1
print(total)
)",
                                pipeline);
  check(ok, "runAll: program analyzes");
  if (!ok) return;

  const auto stats = a::ir::runAll(pipeline.program);
  check(stats.constantsFolded + stats.functionsInlined + stats.loopsHoisted > 0,
        "runAll: pipeline reports work");
  const std::string main = functionByName(pipeline.program, "main");
  check(!contains(main, "call double"), "runAll: helper call inlined");
}

}  // namespace

int main() {
  testLowering();
  testConstantFolding();
  testDeadCodeElimination();
  testInlining();
  testLoopInvariantMotion();
  testRunAll();

  if (failures == 0) {
    std::cout << "ir tests passed\n";
    return 0;
  }
  std::cerr << failures << " test(s) failed\n";
  return 1;
}
