#include "a/driver/driver.hpp"

#include <fstream>
#include <ios>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include "a/common/diagnostics.hpp"
#include "a/common/source.hpp"
#include "a/frontend/lexer.hpp"
#include "a/frontend/parser.hpp"
#include "a/runtime/interpreter.hpp"
#include "a/sema/sema.hpp"

namespace a {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitCompileError = 1;
constexpr int kExitRuntimeError = 2;
constexpr int kExitNoInput = 66;

}  // namespace

int runSource(const std::string& displayPath, std::string source, const DriverConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err) {
  SourceFile file(displayPath, std::move(source));
  DiagnosticSink sink(file);

  Lexer lexer(file.text(), sink);
  Parser parser(lexer.tokenize(), sink);
  std::unique_ptr<Program> program = parser.parseProgram();
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(*program, sink);
  SemaResult result = sema.analyze();
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  if (config.checkOnly) return kExitOk;

  Interpreter interp(*program, result, sink, in, out, err);
  int code = interp.run();
  sink.flush(err, config.useColor);
  return code;
}

int runFile(const std::string& path, const DriverConfig& config, std::istream& in,
            std::ostream& out, std::ostream& err) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err << "error: cannot open '" << path << "'\n";
    return kExitNoInput;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad()) {
    err << "error: failed to read '" << path << "'\n";
    return kExitNoInput;
  }

  return runSource(path, buffer.str(), config, in, out, err);
}

}  // namespace a
