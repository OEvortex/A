#include "driver/driver.hpp"

#include <fstream>
#include <ios>
#include <istream>
#include <ostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "ast/ast.hpp"
#include "backend/interpreter.hpp"
#include "backend/native/native_codegen.hpp"
#include "backend/llvm/llvm_backend.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "lexer/source.hpp"
#include "tooling/docgen.hpp"
#include "tooling/formatter.hpp"
#include "lexer/token.hpp"
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

  const bool json = config.jsonDiagnostics && config.checkOnly;
  if (sink.hasErrors()) {
    if (json) {
      sink.writeJson(out);
    } else {
      sink.flush(err, config.useColor);
    }
    return kExitCompileError;
  }
  if (!json) sink.flush(err, config.useColor);

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  if (json) {
    sink.writeJson(out);
  } else {
    sink.flush(err, config.useColor);
  }
  if (sink.hasErrors()) return kExitCompileError;

  if (config.checkOnly) return kExitOk;

  if (config.backend == "llvm" && isLLVMSupported(graph)) {
    // Run directly via high-speed LLVM code generation
    return runLLVMModule(graph, result, in, out, err);
  }

  if (isNativeSupported(graph)) {
    int code = runNativeBinary(graph, result, in, out, err);
    if (code >= 0) {
      sink.flush(err, config.useColor);
      return code;
    }
    // The native emitter accepted the AST but g++ could not compile the
    // result (e.g. a type mismatch it did not anticipate): fall back to the
    // interpreter so the program still runs correctly.
  }

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

int testFile(const std::string& path, const DriverConfig& config, std::istream& in,
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

  Interpreter interp(graph, result, sink, in, out, err);
  int code = interp.runTests(out);
  sink.flush(err, config.useColor);
  return code;
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

int formatFile(const std::string& path, const FormatFileOptions& options, std::ostream& out,
               std::ostream& err) {
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
  const std::string source = buffer.str();

  DiagnosticSink sink;
  sink.addFile(std::make_shared<SourceFile>(path, source));

  tooling::FormatOptions formatOptions;
  formatOptions.indentWidth = options.indentWidth;
  tooling::FormatResult result = tooling::formatSource(source, sink, formatOptions);
  sink.flush(err, options.useColor);
  if (!result.ok || sink.hasErrors()) return kExitCompileError;

  if (options.checkOnly) {
    if (result.text != source) {
      err << path << ": needs formatting\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  if (options.writeInPlace) {
    std::ofstream target(path, std::ios::binary | std::ios::trunc);
    if (!target) {
      err << "error: cannot open '" << path << "' for writing\n";
      return kExitCompileError;
    }
    target << result.text;
    if (!target) {
      err << "error: failed to write '" << path << "'\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  out << result.text;
  return kExitOk;
}

int documentFile(const std::string& path, const DocFileOptions& options, std::ostream& out,
                 std::ostream& err) {
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
  const std::string source = buffer.str();

  DiagnosticSink sink;
  sink.addFile(std::make_shared<SourceFile>(path, source));

  tooling::DocOptions docOptions;
  docOptions.sourceName = path;
  tooling::DocResult result = tooling::extractDocs(source, sink, docOptions);
  sink.flush(err, options.useColor);
  if (!result.ok || sink.hasErrors()) return kExitCompileError;

  const std::string text =
      options.json ? tooling::renderJson(result.module) : tooling::renderMarkdown(result.module);

  if (!options.outputPath.empty()) {
    std::ofstream target(options.outputPath, std::ios::binary | std::ios::trunc);
    if (!target) {
      err << "error: cannot open '" << options.outputPath << "' for writing\n";
      return kExitCompileError;
    }
    target << text;
    if (!target) {
      err << "error: failed to write '" << options.outputPath << "'\n";
      return kExitCompileError;
    }
    return kExitOk;
  }

  out << text;
  return kExitOk;
}

}  // namespace a
