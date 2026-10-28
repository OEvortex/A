#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "analysis.hpp"
#include "json.hpp"

namespace {

namespace fs = std::filesystem;
using a::Diagnostic;
using a::Severity;
using a::Span;
using a::Tok;
using a::Token;
using a::lsp::Compilation;
using a::lsp::FileIndex;
using a::lsp::ResolvedSymbol;
using a::lsp::SymbolDef;
using a::lsp::SymbolKind;

using jsonlite::Value;

// --------------------------------------------------------------- source text

std::string canonicalPath(const std::string& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

std::string pathToUri(const std::string& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  std::string text = error ? path : absolute.string();
  std::replace(text.begin(), text.end(), '\\', '/');
  if (!text.empty() && text.front() != '/') text.insert(text.begin(), '/');

  static const char* hex = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      uri.push_back(static_cast<char>(c));
    } else {
      uri.push_back('%');
      uri.push_back(hex[(c >> 4) & 0xF]);
      uri.push_back(hex[c & 0xF]);
    }
  }
  return uri;
}

std::string uriToPath(const std::string& uri) {
  std::string rest = uri;
  const std::string prefix = "file://";
  if (rest.rfind(prefix, 0) == 0) rest = rest.substr(prefix.size());
  if (!rest.empty() && rest.front() != '/') {
    std::size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(slash);
  }

  std::string path;
  path.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit(static_cast<unsigned char>(rest[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(rest[i + 2]))) {
      auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      path.push_back(static_cast<char>(digit(rest[i + 1]) * 16 + digit(rest[i + 2])));
      i += 2;
    } else {
      path.push_back(rest[i]);
    }
  }
  return path;
}

std::string lineText(const std::string& text, int oneBasedLine) {
  if (oneBasedLine <= 0) return {};
  std::size_t line = 1;
  std::size_t start = 0;
  while (start < text.size() && line < static_cast<std::size_t>(oneBasedLine)) {
    std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) return {};
    start = newline + 1;
    ++line;
  }
  if (line != static_cast<std::size_t>(oneBasedLine)) return {};
  std::size_t end = text.find('\n', start);
  if (end == std::string::npos) end = text.size();
  if (end > start && text[end - 1] == '\r') --end;
  return text.substr(start, end - start);
}

int utf16Units(unsigned codePoint) { return codePoint > 0xFFFF ? 2 : 1; }

int byteToUtf16Column(const std::string& line, int zeroBasedByteColumn) {
  int byteColumn = std::max(0, zeroBasedByteColumn);
  int units = 0;
  std::size_t i = 0;
  while (i < line.size() && static_cast<int>(i) < byteColumn) {
    unsigned char c = static_cast<unsigned char>(line[i]);
    unsigned codePoint = 0xFFFD;
    std::size_t length = 1;
    if (c < 0x80) {
      codePoint = c;
    } else if ((c >> 5) == 0x6) {
      codePoint = c & 0x1F;
      length = 2;
    } else if ((c >> 4) == 0xE) {
      codePoint = c & 0x0F;
      length = 3;
    } else if ((c >> 3) == 0x1E) {
      codePoint = c & 0x07;
      length = 4;
    }
    for (std::size_t j = 1; j < length && i + j < line.size(); ++j) {
      codePoint = (codePoint << 6) | (static_cast<unsigned char>(line[i + j]) & 0x3F);
    }
    i += length;
    units += utf16Units(codePoint);
  }
  return units;
}

int utf16ToByteColumn(const std::string& line, int character) {
  int remaining = std::max(0, character);
  std::size_t i = 0;
  while (i < line.size() && remaining > 0) {
    unsigned char c = static_cast<unsigned char>(line[i]);
    unsigned codePoint = 0xFFFD;
    std::size_t length = 1;
    if (c < 0x80) {
      codePoint = c;
    } else if ((c >> 5) == 0x6) {
      codePoint = c & 0x1F;
      length = 2;
    } else if ((c >> 4) == 0xE) {
      codePoint = c & 0x0F;
      length = 3;
    } else if ((c >> 3) == 0x1E) {
      codePoint = c & 0x07;
      length = 4;
    }
    i += length;
    remaining -= utf16Units(codePoint);
  }
  return static_cast<int>(i);
}

Value positionValue(const std::string& text, Span span) {
  Value position = Value::object();
  int line = span.line > 0 ? span.line : 1;
  std::string textLine = lineText(text, line);
  position["line"] = line - 1;
  position["character"] = byteToUtf16Column(textLine, std::max(0, span.col - 1));
  return position;
}

Value rangeValue(const std::string& text, Span span) {
  Value range = Value::object();
  int startLine = span.line > 0 ? span.line : 1;
  int endLine = span.endLine > 0 ? span.endLine : startLine;
  int startCol = span.col > 0 ? span.col : 1;
  int endCol = span.endCol > 0 ? span.endCol : startCol + 1;

  range["start"] = positionValue(text, Span{startLine, startCol, startLine, startCol, span.fileId});
  range["end"] = positionValue(text, Span{endLine, endCol, endLine, endCol, span.fileId});
  return range;
}

Value locationValue(const Compilation& compilation, const std::string& path, Span span) {
  Value location = Value::object();
  location["uri"] = pathToUri(path);
  const std::string* text = compilation.textForPath(path);
  static const std::string empty;
  location["range"] = rangeValue(text ? *text : empty, span);
  return location;
}

std::vector<std::string> readStringArray(const Value* value) {
  std::vector<std::string> result;
  if (!value || !value->isArray()) return result;
  for (const Value& item : value->asArray()) {
    if (item.isString()) result.push_back(item.asString());
  }
  return result;
}

// ------------------------------------------------------------------ builtins

struct BuiltinInfo {
  const char* name;
  const char* signature;
  const char* documentation;
};

const BuiltinInfo kBuiltins[] = {
    {"print", "print(value)", "Write a value to standard output, followed by a newline."},
    {"eprint", "eprint(value)", "Write a value to standard error, followed by a newline."},
    {"len", "len(value) -> i32", "Length of a string, array, tuple, or range."},
    {"str", "str(value) -> String", "Convert a value to its string representation."},
    {"sqrt", "sqrt(x: f64) -> f64", "Square root."},
    {"f64", "f64(value) -> f64", "Convert a number to f64."},
    {"i64", "i64(value) -> i64", "Convert a number to i64 (truncating)."},
    {"abs", "abs(value) -> number", "Absolute value."},
    {"round", "round(x) -> number", "Round to the nearest integer."},
    {"floor", "floor(x) -> number", "Round down."},
    {"ceil", "ceil(x) -> number", "Round up."},
    {"pow", "pow(base, exponent) -> number", "Raise a number to a power."},
    {"join", "join(values, separator) -> String", "Join string values with a separator."},
    {"map", "map(values, fn) -> array", "Apply a function to every element."},
    {"filter", "filter(values, fn) -> array", "Keep elements for which the predicate is true."},
    {"sum", "sum(values) -> number", "Sum an array of numbers."},
    {"reduce", "reduce(values, fn, initial) -> value", "Fold an array from left to right."},
    {"any", "any(values, fn) -> bool", "True when the predicate holds for any element."},
    {"all", "all(values, fn) -> bool", "True when the predicate holds for every element."},
    {"Some", "Some(value) -> T?", "Wrap a value in an optional."},
    {"panic", "panic(message)", "Abort execution with a runtime error."},
    {"assert", "assert(condition)", "Abort when the condition is false."},
    {"assert_eq", "assert_eq(actual, expected)", "Abort when the two values differ."},
};

const BuiltinInfo* findBuiltin(const std::string& name) {
  for (const BuiltinInfo& builtin : kBuiltins) {
    if (name == builtin.name) return &builtin;
  }
  return nullptr;
}

const char* const kKeywords[] = {
    "var", "const", "fn", "if", "elif", "else", "while", "do", "for", "in", "loop",
    "break", "continue", "return", "defer", "struct", "class", "metaclass", "enum",
    "self", "true", "false", "and", "or", "not", "import", "from", "as", "match",
    "None", "pass",
};

int completionKind(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::Function: return 3;
    case SymbolKind::Method: return 2;
    case SymbolKind::Struct: return 22;
    case SymbolKind::Class: return 7;
    case SymbolKind::Enum: return 13;
    case SymbolKind::EnumMember: return 20;
    case SymbolKind::Field: return 5;
    case SymbolKind::Constant: return 21;
    case SymbolKind::Module: return 9;
    case SymbolKind::Interface: return 8;
    case SymbolKind::TypeParameter: return 25;
    default: return 6;
  }
}

Value symbolRange(const std::string& text, Span span) { return rangeValue(text, span); }

// ------------------------------------------------------------ language server

struct Document {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
  std::unique_ptr<Compilation> compilation;
};

class LanguageServer {
 public:
  LanguageServer() = default;

  int run() {
    std::string body;
    while (readMessage(std::cin, body)) {
      std::string error;
      Value message = jsonlite::parse(body, &error);
      if (!message.isObject()) {
        sendError(Value(), -32700, "parse error: " + error);
        continue;
      }
      if (!handle(message)) break;
    }
    return shutdownReceived_ ? 0 : 1;
  }

 private:
  bool logging() const {
    static const bool enabled = std::getenv("A_LSP_LOG") != nullptr;
    return enabled;
  }

  void log(const std::string& message) {
    if (logging()) std::cerr << "[a-lsp] " << message << "\n";
  }

  void reply(const Value& id, Value result) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    writeMessage(response);
  }

  void replyNull(const Value& id) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = Value();
    writeMessage(response);
  }

  void sendError(const Value& id, int code, const std::string& message) {
    Value response = Value::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    Value error = Value::object();
    error["code"] = code;
    error["message"] = message;
    response["error"] = std::move(error);
    writeMessage(response);
  }

  void notify(const std::string& method, Value params) {
    Value message = Value::object();
    message["jsonrpc"] = "2.0";
    message["method"] = method;
    message["params"] = std::move(params);
    writeMessage(message);
  }

  static bool readMessage(std::istream& input, std::string& body) {
    std::size_t contentLength = 0;
    bool sawHeader = false;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) break;
      sawHeader = true;
      std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == "content-length") {
        try {
          contentLength = static_cast<std::size_t>(std::stoul(value));
        } catch (...) {
          return false;
        }
      }
    }
    if (!sawHeader && input.eof()) return false;
    if (contentLength == 0) return !input.eof();
    body.resize(contentLength);
    input.read(body.data(), static_cast<std::streamsize>(contentLength));
    return static_cast<std::size_t>(input.gcount()) == contentLength;
  }

  static void writeMessage(const Value& message) {
    std::string body = message.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
    std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
    std::cout.flush();
  }

  bool handle(const Value& message) {
    const Value* methodValue = message.find("method");
    const Value* idValue = message.find("id");
    const Value* params = message.find("params");
    const std::string method = methodValue && methodValue->isString() ? methodValue->asString() : "";
    const bool isRequest = idValue != nullptr;

    if (method == "initialize") {
      handleInitialize(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "initialized") return true;
    if (method == "shutdown") {
      shutdownReceived_ = true;
      replyNull(idValue ? *idValue : Value());
      return true;
    }
    if (method == "exit") return false;
    if (method == "textDocument/didOpen") {
      didOpen(params);
      return true;
    }
    if (method == "textDocument/didChange") {
      didChange(params);
      return true;
    }
    if (method == "textDocument/didSave") {
      didSave(params);
      return true;
    }
    if (method == "textDocument/didClose") {
      didClose(params);
      return true;
    }
    if (method == "workspace/didChangeConfiguration") {
      handleConfiguration(params);
      return true;
    }
    if (method == "$/setTrace" || method == "$/cancelRequest" || method == "workspace/didChangeWatchedFiles") {
      return true;
    }

    if (method == "textDocument/definition") {
      definition(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/hover") {
      hover(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/completion") {
      completion(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/references") {
      references(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/documentSymbol") {
      documentSymbols(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "workspace/symbol") {
      workspaceSymbols(idValue ? *idValue : Value(), params);
      return true;
    }

    if (isRequest) {
      sendError(*idValue, -32601, "method not found: " + method);
    }
    return true;
  }

  void handleInitialize(const Value& id, const Value* params) {
    modulePaths_.clear();
    std::string root;
    if (params) {
      if (const Value* options = params->find("initializationOptions")) {
        for (const std::string& path : readStringArray(options->find("modulePaths"))) {
          modulePaths_.push_back(path);
        }
        if (const Value* stdlib = options->find("stdlibRoot"); stdlib && stdlib->isString()) {
          stdlibRoot_ = stdlib->asString();
        }
      }
      if (const Value* folders = params->find("workspaceFolders"); folders && folders->isArray()) {
        for (const Value& folder : folders->asArray()) {
          if (const Value* uri = folder.find("uri"); uri && uri->isString()) {
            root = uriToPath(uri->asString());
            break;
          }
        }
      }
      if (root.empty()) {
        if (const Value* rootUri = params->find("rootUri"); rootUri && rootUri->isString()) {
          root = uriToPath(rootUri->asString());
        } else if (const Value* rootPath = params->find("rootPath");
                   rootPath && rootPath->isString()) {
          root = rootPath->asString();
        }
      }
    }
    if (!root.empty()) {
      modulePaths_.insert(modulePaths_.begin(), root);
      std::error_code error;
      fs::path build = fs::path(root) / "build";
      if (fs::is_directory(build, error)) modulePaths_.push_back(build.string());
    }

    Value capabilities = Value::object();
    Value sync = Value::object();
    sync["openClose"] = true;
    sync["change"] = 1;  // full document sync
    Value save = Value::object();
    save["includeText"] = true;
    sync["save"] = std::move(save);
    capabilities["textDocumentSync"] = std::move(sync);
    capabilities["definitionProvider"] = true;
    capabilities["referencesProvider"] = true;
    capabilities["hoverProvider"] = true;
    capabilities["documentSymbolProvider"] = true;
    capabilities["workspaceSymbolProvider"] = true;
    Value completion = Value::object();
    Value triggers = Value::array();
    triggers.push(".");
    triggers.push("\"");
    completion["triggerCharacters"] = std::move(triggers);
    completion["resolveProvider"] = false;
    capabilities["completionProvider"] = std::move(completion);

    Value result = Value::object();
    result["capabilities"] = std::move(capabilities);
    Value info = Value::object();
    info["name"] = "a-language-server";
    info["version"] = "0.3.0";
    result["serverInfo"] = std::move(info);
    reply(id, std::move(result));
  }

  void handleConfiguration(const Value* params) {
    if (!params) return;
    const Value* settings = params->find("settings");
    if (!settings) return;
    const Value* aSettings = settings->find("a");
    if (!aSettings) return;
    if (const Value* paths = aSettings->find("modulePaths")) {
      modulePaths_ = readStringArray(paths);
    }
    if (const Value* stdlib = aSettings->find("stdlibRoot"); stdlib && stdlib->isString()) {
      stdlibRoot_ = stdlib->asString();
    }
    for (auto& entry : documents_) recompile(entry.second);
  }

  Document* documentFor(const std::string& uri) {
    auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
  }

  std::optional<std::pair<int, int>> positionFrom(const Value* params, int* lineOut,
                                                  int* characterOut) {
    if (!params) return std::nullopt;
    const Value* position = params->find("position");
    if (!position) return std::nullopt;
    const Value* line = position->find("line");
    const Value* character = position->find("character");
    if (!line || !character) return std::nullopt;
    *lineOut = static_cast<int>(line->asInt());
    *characterOut = static_cast<int>(character->asInt());
    return {{*lineOut, *characterOut}};
  }

  Document* documentFrom(const Value* params) {
    if (!params) return nullptr;
    const Value* textDocument = params->find("textDocument");
    if (!textDocument) return nullptr;
    const Value* uri = textDocument->find("uri");
    if (!uri || !uri->isString()) return nullptr;
    return documentFor(uri->asString());
  }

  void didOpen(const Value* params) {
    if (!params) return;
    const Value* textDocument = params->find("textDocument");
    if (!textDocument) return;
    const Value* uri = textDocument->find("uri");
    const Value* text = textDocument->find("text");
    if (!uri || !uri->isString() || !text || !text->isString()) return;

    Document document;
    document.uri = uri->asString();
    document.path = uriToPath(document.uri);
    document.text = text->asString();
    if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
      document.version = static_cast<int>(version->asInt());
    }
    log("open " + document.path);
    Document& stored = documents_[document.uri];
    stored = std::move(document);
    recompile(stored);
  }

  void didChange(const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !params) return;
    const Value* textDocument = params->find("textDocument");
    if (textDocument) {
      if (const Value* version = textDocument->find("version"); version && version->isNumber()) {
        document->version = static_cast<int>(version->asInt());
      }
    }
    const Value* changes = params->find("contentChanges");
    if (!changes || !changes->isArray() || changes->asArray().empty()) return;
    const Value& last = changes->asArray().back();
    const Value* text = last.find("text");
    if (!text || !text->isString()) return;
    document->text = text->asString();
    recompile(*document);
  }

  void didSave(const Value* params) {
    Document* document = documentFrom(params);
    if (!document) return;
    if (params) {
      const Value* text = params->find("text");
      if (text && text->isString()) document->text = text->asString();
    }
    recompile(*document);
  }

  void didClose(const Value* params) {
    Document* document = documentFrom(params);
    if (!document) return;
    publishDiagnostics(*document, {});
    documents_.erase(document->uri);
  }

  void recompile(Document& document) {
    document.compilation = a::lsp::analyzeDocument(document.path, document.text, modulePaths_,
                                                    stdlibRoot_);
    log("compiled " + document.path + " (errors: " +
        std::to_string(document.compilation ? document.compilation->sink.errorCount() : 0) + ")");
    publishDiagnosticsFor(document);
  }

  void publishDiagnosticsFor(const Document& document) {
    if (!document.compilation) return;
    const Compilation& compilation = *document.compilation;

    std::map<std::string, std::vector<const Diagnostic*>> byPath;
    for (const Diagnostic& diagnostic : compilation.sink.diagnostics()) {
      if (!diagnostic.span.valid()) continue;
      const a::SourceFile* source = compilation.sink.file(diagnostic.span.fileId);
      if (!source) continue;
      byPath[canonicalPath(source->path())].push_back(&diagnostic);
    }

    for (auto& entry : documents_) {
      Document& open = entry.second;
      Value params = Value::object();
      params["uri"] = open.uri;
      if (open.version > 0) params["version"] = open.version;
      Value diagnostics = Value::array();

      auto found = byPath.find(canonicalPath(open.path));
      if (found != byPath.end()) {
        for (const Diagnostic* diagnostic : found->second) {
          const a::SourceFile* source = compilation.sink.file(diagnostic->span.fileId);
          const std::string& text = source ? source->text() : open.text;
          Value item = Value::object();
          item["range"] = rangeValue(text, diagnostic->span);
          item["severity"] = diagnostic->severity == Severity::Error ? 1 : 2;
          std::string message = diagnostic->message;
          if (!diagnostic->help.empty()) message += "\nhelp: " + diagnostic->help;
          item["message"] = message;
          item["source"] = "a";
          diagnostics.push(std::move(item));
        }
      }
      params["diagnostics"] = std::move(diagnostics);
      notify("textDocument/publishDiagnostics", std::move(params));
    }
  }

  void publishDiagnostics(const Document& document, const std::vector<Value>& diagnostics) {
    Value params = Value::object();
    params["uri"] = document.uri;
    if (document.version > 0) params["version"] = document.version;
    Value array = Value::array();
    for (const Value& item : diagnostics) array.push(item);
    params["diagnostics"] = std::move(array);
    notify("textDocument/publishDiagnostics", std::move(params));
  }

  const Token* tokenAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
    for (const Token& token : tokens) {
      if (token.kind == Tok::Eof) continue;
      if (token.span.line != line) continue;
      if (byteColumn < token.span.col || byteColumn > token.span.endCol) continue;
      return &token;
    }
    return nullptr;
  }

  int tokenIndexAt(const std::vector<Token>& tokens, int line, int byteColumn) const {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const Token& token = tokens[i];
      if (token.kind == Tok::Eof) continue;
      if (token.span.line != line) continue;
      if (byteColumn < token.span.col || byteColumn > token.span.endCol) continue;
      return static_cast<int>(i);
    }
    return -1;
  }

  int tokenIndexBefore(const std::vector<Token>& tokens, int line, int byteColumn) const {
    int best = -1;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const Token& token = tokens[i];
      if (token.kind == Tok::Eof || token.kind == Tok::Newline || token.kind == Tok::Indent ||
          token.kind == Tok::Dedent) {
        continue;
      }
      if (token.span.line > line) break;
      if (token.span.line == line && token.span.col > byteColumn) break;
      best = static_cast<int>(i);
    }
    return best;
  }

  void definition(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol resolved = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!resolved.valid()) return replyNull(id);
    reply(id, locationValue(compilation, resolved.file->path, resolved.symbol->span));
  }

  void hover(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol resolved = a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (resolved.valid()) {
      std::string description = a::lsp::describeSymbol(*resolved.symbol);
      std::string markdown = "```a\n" + description + "\n```";
      const std::string* text = compilation.textForPath(resolved.file->path);
      Value result = Value::object();
      Value contents = Value::object();
      contents["kind"] = "markdown";
      contents["value"] = markdown;
      result["contents"] = std::move(contents);
      if (text) result["range"] = rangeValue(*text, resolved.symbol->span);
      reply(id, std::move(result));
      return;
    }

    const Token* token = tokenAt(*tokens, line + 1, byteColumn);
    if (token && token->kind == Tok::Ident) {
      if (const BuiltinInfo* builtin = findBuiltin(token->lexeme)) {
        std::string markdown = "```a\n";
        markdown += builtin->signature;
        markdown += "\n```\n\n";
        markdown += builtin->documentation;
        Value result = Value::object();
        Value contents = Value::object();
        contents["kind"] = "markdown";
        contents["value"] = markdown;
        result["contents"] = std::move(contents);
        reply(id, std::move(result));
        return;
      }
    }
    replyNull(id);
  }

  void references(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* entry = compilation.entryIndex();
    const std::vector<Token>* entryTokens = compilation.entryTokens();
    if (!entry || !entryTokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    ResolvedSymbol target = a::lsp::resolveAt(compilation, *entry, *entryTokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    Value result = Value::array();
    for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
      const FileIndex& file = compilation.indexes[moduleId];
      if (moduleId >= compilation.tokensByModule.size()) continue;
      const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
      for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& token = tokens[static_cast<std::size_t>(i)];
        if (token.kind != Tok::Ident) continue;
        ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
        if (!resolved.valid()) continue;
        if (resolved.symbol->span.line != target.symbol->span.line) continue;
        if (resolved.symbol->span.col != target.symbol->span.col) continue;
        if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
        result.push(locationValue(compilation, resolved.file->path, token.span));
      }
    }
    reply(id, std::move(result));
  }

  Value documentSymbolFromStatement(const Compilation& compilation, const std::string& text,
                                    const a::Stmt& statement) {
    Value symbol = Value::object();
    switch (statement.kind) {
      case a::StmtKind::FnDecl: {
        const auto& fn = static_cast<const a::FnDecl&>(statement);
        symbol["name"] = fn.name;
        symbol["detail"] = a::lsp::signatureText(fn);
        symbol["kind"] = static_cast<int>(SymbolKind::Function);
        symbol["range"] = symbolRange(text, fn.span);
        symbol["selectionRange"] = symbolRange(text, fn.nameSpan);
        return symbol;
      }
      case a::StmtKind::StructDecl: {
        const auto& decl = static_cast<const a::StructDecl&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = decl.isClass ? "class" : "struct";
        symbol["kind"] = static_cast<int>(decl.isClass ? SymbolKind::Class : SymbolKind::Struct);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        Value children = Value::array();
        for (const auto& field : decl.fields) {
          Value child = Value::object();
          child["name"] = field.name;
          child["detail"] = a::lsp::typeExprText(field.type.get());
          child["kind"] = static_cast<int>(SymbolKind::Field);
          child["range"] = symbolRange(text, field.span);
          child["selectionRange"] = symbolRange(text, field.span);
          children.push(std::move(child));
        }
        for (const auto& method : decl.methods) {
          if (!method) continue;
          Value child = Value::object();
          child["name"] = method->name;
          child["detail"] = a::lsp::signatureText(*method);
          child["kind"] = static_cast<int>(SymbolKind::Method);
          child["range"] = symbolRange(text, method->span);
          child["selectionRange"] = symbolRange(text, method->nameSpan);
          children.push(std::move(child));
        }
        symbol["children"] = std::move(children);
        return symbol;
      }
      case a::StmtKind::EnumDecl: {
        const auto& decl = static_cast<const a::EnumDecl&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = "enum";
        symbol["kind"] = static_cast<int>(SymbolKind::Enum);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        Value children = Value::array();
        for (const auto& variant : decl.variants) {
          Value child = Value::object();
          child["name"] = variant.name;
          child["detail"] = decl.name + "." + variant.name;
          child["kind"] = static_cast<int>(SymbolKind::EnumMember);
          child["range"] = symbolRange(text, variant.span);
          child["selectionRange"] = symbolRange(text, variant.span);
          children.push(std::move(child));
        }
        symbol["children"] = std::move(children);
        return symbol;
      }
      case a::StmtKind::Let: {
        const auto& decl = static_cast<const a::LetStmt&>(statement);
        symbol["name"] = decl.name;
        symbol["detail"] = a::lsp::typeExprText(decl.type.get());
        symbol["kind"] = static_cast<int>(decl.constantBinding ? SymbolKind::Constant
                                                               : SymbolKind::Variable);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] = symbolRange(text, decl.nameSpan);
        return symbol;
      }
      case a::StmtKind::Assign: {
        const auto& assign = static_cast<const a::AssignStmt&>(statement);
        if (!assign.target || assign.target->kind != a::ExprKind::Ident) return Value();
        const auto& ident = static_cast<const a::IdentExpr&>(*assign.target);
        symbol["name"] = ident.name;
        symbol["kind"] = static_cast<int>(SymbolKind::Variable);
        symbol["range"] = symbolRange(text, assign.span);
        symbol["selectionRange"] = symbolRange(text, ident.span);
        return symbol;
      }
      case a::StmtKind::Destructure: {
        const auto& decl = static_cast<const a::DestructureStmt&>(statement);
        if (decl.names.size() != 1) return Value();
        symbol["name"] = decl.names.front();
        symbol["kind"] = static_cast<int>(SymbolKind::Variable);
        symbol["range"] = symbolRange(text, decl.span);
        symbol["selectionRange"] =
            symbolRange(text, decl.nameSpans.empty() ? decl.span : decl.nameSpans.front());
        return symbol;
      }
      default:
        return Value();
    }
    (void)compilation;
  }

  void documentSymbols(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const int entry = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
    if (entry < 0 || static_cast<std::size_t>(entry) >= compilation.graph.modules.size()) {
      return replyNull(id);
    }
    const a::Module& module = compilation.graph.modules[static_cast<std::size_t>(entry)];
    if (!module.program) return replyNull(id);

    Value result = Value::array();
    for (const auto& statement : module.program->statements) {
      if (!statement) continue;
      Value symbol = documentSymbolFromStatement(compilation, document->text, *statement);
      if (!symbol.isNull()) result.push(std::move(symbol));
    }
    reply(id, std::move(result));
  }

  void workspaceSymbols(const Value& id, const Value* params) {
    std::string query;
    if (params) {
      const Value* queryValue = params->find("query");
      if (queryValue && queryValue->isString()) query = queryValue->asString();
    }
    std::string lowered = query;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    Value result = Value::array();
    std::unordered_set<std::string> seen;
    for (const auto& entry : documents_) {
      const Document& document = entry.second;
      if (!document.compilation) continue;
      const FileIndex* file = document.compilation->entryIndex();
      if (!file) continue;
      for (const SymbolDef& symbol : file->symbols) {
        if (!symbol.topLevel) continue;
        std::string nameLower = symbol.name;
        for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!lowered.empty() && nameLower.find(lowered) == std::string::npos) continue;
        std::string key = file->path + ":" + std::to_string(symbol.span.line) + ":" +
                          std::to_string(symbol.span.col);
        if (!seen.insert(key).second) continue;
        Value item = Value::object();
        item["name"] = symbol.name;
        item["kind"] = static_cast<int>(symbol.kind);
        item["location"] =
            locationValue(*document.compilation, file->path, symbol.span);
        result.push(std::move(item));
      }
    }
    reply(id, std::move(result));
  }

  void completion(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;

    std::unordered_set<std::string> seen;
    Value items = Value::array();

    auto pushItem = [&](const std::string& name, int kind, const std::string& detail) {
      if (!seen.insert(name).second) return;
      Value item = Value::object();
      item["label"] = name;
      item["kind"] = kind;
      item["insertText"] = name;
      if (!detail.empty()) item["detail"] = detail;
      items.push(std::move(item));
    };

    // Member completion after "." or "?.".
    int previous = tokenIndexBefore(*tokens, line + 1, byteColumn - 1);
    int base = -1;
    if (previous >= 0) {
      const Token& token = (*tokens)[static_cast<std::size_t>(previous)];
      if (token.kind == Tok::Dot || token.kind == Tok::QuestionDot) {
        if (previous >= 1) base = previous - 1;
      } else if (previous >= 1) {
        const Token& before =
            (*tokens)[static_cast<std::size_t>(previous - 1)];
        if ((before.kind == Tok::Dot || before.kind == Tok::QuestionDot) && previous >= 2) {
          base = previous - 2;
        }
      }
    }

    if (base >= 0 && (*tokens)[static_cast<std::size_t>(base)].kind == Tok::Ident) {
      const Token& baseToken = (*tokens)[static_cast<std::size_t>(base)];
      for (const a::lsp::ImportDef& import : file->imports) {
        if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
        const FileIndex* target = compilation.indexForModule(import.moduleId);
        if (!target) continue;
        for (const SymbolDef& symbol : target->symbols) {
          if (symbol.topLevel) {
            pushItem(symbol.name, completionKind(symbol.kind), symbol.detail);
          }
        }
      }
      ResolvedSymbol resolved =
          a::lsp::resolveToken(compilation, *file, *tokens, base);
      if (resolved.valid()) {
        std::string typeName = resolved.symbol->typeName;
        if (typeName.empty() &&
            (resolved.symbol->kind == SymbolKind::Struct ||
             resolved.symbol->kind == SymbolKind::Class ||
             resolved.symbol->kind == SymbolKind::Enum)) {
          typeName = resolved.symbol->name;
        }
        if (!typeName.empty()) {
          for (const SymbolDef* member :
               a::lsp::structMembers(compilation, *resolved.file, typeName)) {
            pushItem(member->name, completionKind(member->kind), member->detail);
          }
        }
      }
    } else {
      for (const char* keyword : kKeywords) pushItem(keyword, 14, "keyword");
      for (const BuiltinInfo& builtin : kBuiltins) {
        pushItem(builtin.name, 3, builtin.signature);
      }
      for (const SymbolDef* symbol :
           a::lsp::visibleSymbols(compilation, *file, line + 1, byteColumn)) {
        pushItem(symbol->name, completionKind(symbol->kind), symbol->detail);
      }
      for (const a::lsp::ImportDef& import : file->imports) {
        pushItem(import.alias, import.wholeModule ? 9 : 18, import.modulePath);
      }
    }

    Value result = Value::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    reply(id, std::move(result));
  }

  std::map<std::string, Document> documents_;
  std::vector<std::string> modulePaths_;
  std::string stdlibRoot_;
  bool shutdownReceived_ = false;
};

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--version" || argument == "-V") {
      std::cout << "a-language-server 0.3.0\n";
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: a-lsp [--stdio]\\n"
                   "\\n"
                   "A language server for the A programming language. It speaks the\\n"
                   "Language Server Protocol over stdin/stdout. Editors normally start it\\n"
                   "automatically; pass --stdio when they require an explicit transport.\\n";
      return 0;
    }
  }

  std::ios::sync_with_stdio(false);
  LanguageServer server;
  return server.run();
}
