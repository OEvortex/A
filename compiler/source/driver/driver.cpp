#include "driver/driver.hpp"

#include <fstream>
#include <ios>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include "backend/interpreter.hpp"
#include "backend/native/native_codegen.hpp"
#include "backend/llvm/llvm_backend.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/source.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitCompileError = 1;
constexpr int kExitRuntimeError = 2;
constexpr int kExitNoInput = 66;

}  // namespace

int runSource(const std::string& displayPath, std::string source, const DriverConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err) {
  DiagnosticSink sink;

  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(displayPath, std::move(source));

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  if (config.checkOnly) return kExitOk;

  Interpreter interp(graph, result, sink, in, out, err);
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

int buildFile(const std::string& path, const std::string& outputPath,
              const DriverConfig& config, const std::string& backend, std::ostream& err) {
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

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  if (backend == "llvm") {
    if (!buildLLVMModule(graph, result, outputPath, err)) {
      return kExitCompileError;
    }
  } else {
    if (!buildNativeBinary(graph, result, outputPath, err)) {
      return kExitCompileError;
    }
  }
  return kExitOk;
}

int emitLLVMFile(const std::string& path, const std::string& outputPath,
                 const DriverConfig& config, std::ostream& err) {
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

  DiagnosticSink sink;
  ModuleLoaderOptions options;
  options.modulePaths = config.modulePaths;
  options.stdlibRoot = config.stdlibRoot;
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  sink.flush(err, config.useColor);
  if (sink.hasErrors()) return kExitCompileError;

  std::ofstream out(outputPath);
  if (!out) {
    err << "error: cannot open '" << outputPath << "' for writing\n";
    return kExitCompileError;
  }

  if (!emitLLVMIR(graph, result, out)) {
    return kExitCompileError;
  }
  return kExitOk;
}

}  // namespace a
