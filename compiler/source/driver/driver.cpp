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

int formatFile(const std::string& path, std::ostream& out, std::ostream& err) {
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
  Lexer lexer(buffer.str(), sink, 0);
  std::vector<Token> tokens = lexer.tokenize();
  sink.flush(err, false);
  if (sink.hasErrors()) return kExitCompileError;

  const int kIndentWidth = 4;
  int indentLevel = 0;
  bool needIndent = false;
  bool firstToken = true;

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& tok = tokens[i];
    switch (tok.kind) {
      case Tok::Indent:
        ++indentLevel;
        break;
      case Tok::Dedent:
        if (indentLevel > 0) --indentLevel;
        break;
      case Tok::Newline:
        if (!firstToken) out << '\n';
        needIndent = true;
        firstToken = false;
        break;
      case Tok::Eof:
        break;
      default: {
        if (needIndent) {
          for (int j = 0; j < indentLevel * kIndentWidth; ++j) out << ' ';
          needIndent = false;
        } else if (!firstToken) {
          const Token& prev = tokens[i - 1];
          bool needSpace = false;
          if (tok.kind == Tok::Ident || tok.kind == Tok::Int || tok.kind == Tok::Float ||
              tok.kind == Tok::String || tok.kind == Tok::Char || tok.kind == Tok::True ||
              tok.kind == Tok::False) {
            if (prev.kind == Tok::Ident || prev.kind == Tok::Int || prev.kind == Tok::Float ||
                prev.kind == Tok::String || prev.kind == Tok::Char || prev.kind == Tok::True ||
                prev.kind == Tok::False || prev.kind == Tok::RParen ||
                prev.kind == Tok::RBracket || prev.kind == Tok::RBrace) {
              needSpace = true;
            }
          }
          if (needSpace) out << ' ';
        }
        firstToken = false;
        out << tok.lexeme;
        break;
      }
    }
  }
  out << '\n';
  return kExitOk;
}

std::string docTypeExpr(const TypeExpr& type) {
  if (const auto* named = dynamic_cast<const NamedTypeExpr*>(&type)) {
    std::string s;
    for (std::size_t i = 0; i < named->qualifiers.size(); ++i) {
      if (i) s += ".";
      s += named->qualifiers[i];
    }
    s += named->name;
    if (!named->typeArgs.empty()) {
      s += "<";
      for (std::size_t i = 0; i < named->typeArgs.size(); ++i) {
        if (i) s += ", ";
        s += docTypeExpr(*named->typeArgs[i]);
      }
      s += ">";
    }
    return s;
  }
  if (const auto* array = dynamic_cast<const ArrayTypeExpr*>(&type)) {
    return "[" + docTypeExpr(*array->element) + "]";
  }
  if (const auto* optional = dynamic_cast<const OptionalTypeExpr*>(&type)) {
    return docTypeExpr(*optional->element) + "?";
  }
  if (const auto* tuple = dynamic_cast<const TupleTypeExpr*>(&type)) {
    std::string s = "(";
    for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
      if (i) s += ", ";
      s += docTypeExpr(*tuple->elements[i]);
    }
    s += ")";
    return s;
  }
  if (const auto* fn = dynamic_cast<const FunctionTypeExpr*>(&type)) {
    std::string s = "fn(";
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      if (i) s += ", ";
      s += docTypeExpr(*fn->params[i]);
    }
    s += ") -> " + docTypeExpr(*fn->returnType);
    return s;
  }
  return "type";
}

void docStmt(const Stmt& stmt, std::ostream& out, int level) {
  const std::string indent(level * 2, ' ');
  switch (stmt.kind) {
    case StmtKind::FnDecl: {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      out << indent << "## `fn " << fn.name << "(";
      for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out << ", ";
        out << fn.params[i].name;
        if (fn.params[i].type) out << ": " << docTypeExpr(*fn.params[i].type);
      }
      out << ")";
      if (fn.returnType) out << " -> " << docTypeExpr(*fn.returnType);
      out << "`\n";
      break;
    }
    case StmtKind::StructDecl: {
      const auto& sd = static_cast<const StructDecl&>(stmt);
      out << indent << "## `struct " << sd.name << "`\n";
      for (const auto& field : sd.fields) {
        out << indent << "- `" << field.name << ": " << docTypeExpr(*field.type) << "`\n";
      }
      for (const auto& method : sd.methods) {
        out << indent << "### `fn " << method->name << "(";
        for (std::size_t i = 0; i < method->params.size(); ++i) {
          if (i) out << ", ";
          out << method->params[i].name;
          if (method->params[i].type) out << ": " << docTypeExpr(*method->params[i].type);
        }
        out << ")";
        if (method->returnType) out << " -> " << docTypeExpr(*method->returnType);
        out << "`\n";
      }
      break;
    }
    case StmtKind::EnumDecl: {
      const auto& ed = static_cast<const EnumDecl&>(stmt);
      out << indent << "## `enum " << ed.name << "`\n";
      for (const auto& variant : ed.variants) {
        out << indent << "- `" << variant.name << "`\n";
      }
      break;
    }
    default:
      break;
  }
}

int documentFile(const std::string& path, std::ostream& out, std::ostream& err) {
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
  ModuleLoader loader(std::move(options), sink);
  ModuleGraph graph = loader.load(path, buffer.str());

  sink.flush(err, false);
  if (sink.hasErrors()) return kExitCompileError;

  Sema sema(graph, sink);
  SemaResult result = sema.analyze();
  sink.flush(err, false);
  if (sink.hasErrors()) return kExitCompileError;

  out << "# Documentation for `" << path << "`\n\n";
  for (const auto& stmt : graph.modules[0].program->statements) {
    docStmt(*stmt, out, 0);
  }
  return kExitOk;
}

}  // namespace a
