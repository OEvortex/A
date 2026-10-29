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
#include "name-resolution/builtin_docs.hpp"
#include "name-resolution/native_modules.hpp"
#include "tooling/formatter.hpp"
#include "type-checker/method_signatures.hpp"

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

// --------------------------------------------------------------- shared docs
// Hover, completion, and signature help all read the tables that the type
// checker uses: builtinDocs() for interpreter builtins and the primitive
// method tables in method_signatures.hpp.

constexpr const char* kServerVersion = "0.0.1";

const std::vector<a::MethodSig>* methodTableForType(const std::string& typeName) {
  if (typeName == "array") return &a::arrayMethods();
  if (typeName == "map") return &a::mapMethods();
  if (typeName == "set") return &a::setMethods();
  if (typeName == "String") return &a::stringMethods();
  return nullptr;
}

const a::MethodSig* findMethodSig(const std::vector<a::MethodSig>& table,
                                  const std::string& name) {
  for (const a::MethodSig& method : table) {
    if (name == method.name) return &method;
  }
  return nullptr;
}

// Maps an import alias (`json`, or `import json as j` → `j`) to the native
// module it refers to; empty when the alias is not a native module.
std::string nativeModuleForAlias(const a::lsp::FileIndex& file, const std::string& alias) {
  for (const a::lsp::ImportDef& import : file.imports) {
    if (import.alias == alias && a::isNativeModule(import.modulePath)) return import.modulePath;
  }
  return {};
}

Value hoverMarkdown(const std::string& signature, const std::string& documentation) {
  std::string markdown = "```a\n" + signature + "\n```";
  if (!documentation.empty()) markdown += "\n\n" + documentation;
  Value contents = Value::object();
  contents["kind"] = "markdown";
  contents["value"] = std::move(markdown);
  Value result = Value::object();
  result["contents"] = std::move(contents);
  return result;
}

const char* const kKeywords[] = {
    "var", "const", "fn", "if", "elif", "else", "while", "do", "for", "in", "loop",
    "break", "continue", "return", "defer", "struct", "class", "metaclass", "enum",
    "self", "true", "false", "and", "or", "not", "import", "from", "as", "match",
    "None", "pass", "type", "pvt",
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
    if (method == "completionItem/resolve") {
      completionResolve(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/signatureHelp") {
      signatureHelp(idValue ? *idValue : Value(), params);
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
    if (method == "textDocument/formatting") {
      formatting(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/rename") {
      rename(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/documentHighlight") {
      documentHighlights(idValue ? *idValue : Value(), params);
      return true;
    }
    if (method == "textDocument/foldingRange") {
      foldingRanges(idValue ? *idValue : Value(), params);
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
    capabilities["documentFormattingProvider"] = true;
    capabilities["documentHighlightProvider"] = true;
    capabilities["foldingRangeProvider"] = true;
    Value rename = Value::object();
    rename["prepareProvider"] = false;
    capabilities["renameProvider"] = std::move(rename);
    Value completion = Value::object();
    Value triggers = Value::array();
    triggers.push(".");
    triggers.push("\"");
    completion["triggerCharacters"] = std::move(triggers);
    completion["resolveProvider"] = true;
    capabilities["completionProvider"] = std::move(completion);
    Value signatureHelp = Value::object();
    Value signatureTriggers = Value::array();
    signatureTriggers.push("(");
    signatureTriggers.push(",");
    signatureHelp["triggerCharacters"] = std::move(signatureTriggers);
    capabilities["signatureHelpProvider"] = std::move(signatureHelp);

    Value result = Value::object();
    result["capabilities"] = std::move(capabilities);
    Value info = Value::object();
    info["name"] = "a-language-server";
    info["version"] = kServerVersion;
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

  std::string docCommentText(const Compilation& compilation, const FileIndex& file,
                             const Span& span) const {
    if (!span.valid()) return {};
    int moduleId = -1;
    const std::string canonical = canonicalPath(file.path);
    for (std::size_t i = 0; i < compilation.graph.modules.size(); ++i) {
      if (canonicalPath(compilation.graph.modules[i].path) == canonical) {
        moduleId = static_cast<int>(i);
        break;
      }
    }
    const std::vector<a::Comment>* comments = compilation.commentsForModule(moduleId);
    if (!comments) return {};

    std::vector<std::string> lines;
    int expected = span.line - 1;
    for (std::size_t index = comments->size(); index > 0; --index) {
      const a::Comment& comment = (*comments)[index - 1];
      if (!comment.doc || comment.trailing || comment.span.line != expected) break;
      std::string line = comment.text;
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (line.rfind("##", 0) == 0) line.erase(0, 2);
      if (!line.empty() && line.front() == ' ') line.erase(0, 1);
      lines.push_back(std::move(line));
      --expected;
    }
    std::string text;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      if (!text.empty()) text += '\n';
      text += *it;
    }
    return text;
  }

  static bool validIdentifier(const std::string& name) {
    if (name.empty()) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    for (char c : name) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    for (const char* keyword : kKeywords) {
      if (name == keyword) return false;
    }
    return true;
  }

  std::vector<std::pair<std::string, Span>> collectReferences(
      const Compilation& compilation, const ResolvedSymbol& target) const {
    std::vector<std::pair<std::string, Span>> references;
    if (!target.valid()) return references;
    const std::string targetPath = canonicalPath(target.file->path);
    for (std::size_t moduleId = 0; moduleId < compilation.indexes.size(); ++moduleId) {
      if (moduleId >= compilation.tokensByModule.size()) continue;
      const FileIndex& file = compilation.indexes[moduleId];
      const std::vector<Token>& tokens = compilation.tokensByModule[moduleId];
      for (int i = 0; i < static_cast<int>(tokens.size()); ++i) {
        const Token& token = tokens[static_cast<std::size_t>(i)];
        if (token.kind != Tok::Ident) continue;
        ResolvedSymbol resolved = a::lsp::resolveToken(compilation, file, tokens, i);
        if (!resolved.valid()) continue;
        if (resolved.symbol->span.line != target.symbol->span.line) continue;
        if (resolved.symbol->span.col != target.symbol->span.col) continue;
        if (canonicalPath(resolved.file->path) != targetPath) continue;
        references.emplace_back(resolved.file->path, token.span);
      }
    }
    return references;
  }

  Value fullDocumentRange(const std::string& text) const {
    int lines = 0;
    for (char c : text) {
      if (c == '\n') ++lines;
    }
    std::string lastLine = lineText(text, lines + 1);
    Value range = Value::object();
    Value start = Value::object();
    start["line"] = 0;
    start["character"] = 0;
    Value end = Value::object();
    end["line"] = lines;
    end["character"] = byteToUtf16Column(lastLine, static_cast<int>(lastLine.size()));
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    return range;
  }

  void formatting(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);

    a::tooling::FormatOptions options;
    if (params) {
      if (const Value* formatOptions = params->find("options")) {
        if (const Value* tabSize = formatOptions->find("tabSize"); tabSize && tabSize->isNumber()) {
          const int size = static_cast<int>(tabSize->asInt());
          if (size >= 1 && size <= 16) options.indentWidth = size;
        }
      }
    }

    a::DiagnosticSink sink;
    sink.addFile(std::make_shared<a::SourceFile>(document->path, document->text));
    a::tooling::FormatResult result =
        a::tooling::formatSource(document->text, sink, options);

    Value edits = Value::array();
    if (!result.ok || sink.hasErrors() || result.text == document->text) {
      reply(id, std::move(edits));
      return;
    }

    Value edit = Value::object();
    edit["range"] = fullDocumentRange(document->text);
    edit["newText"] = result.text;
    edits.push(std::move(edit));
    reply(id, std::move(edits));
  }

  void rename(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation || !params) return replyNull(id);
    const Value* newNameValue = params->find("newName");
    if (!newNameValue || !newNameValue->isString()) return replyNull(id);
    const std::string newName = newNameValue->asString();
    if (!validIdentifier(newName)) {
      sendError(id, -32602, "invalid identifier: " + newName);
      return;
    }

    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    int byteColumn = utf16ToByteColumn(lineText(document->text, line + 1), character) + 1;
    ResolvedSymbol target =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    const auto references = collectReferences(compilation, target);
    if (references.empty()) return replyNull(id);

    Value changes = Value::object();
    for (const auto& [path, span] : references) {
      const std::string uri = pathToUri(path);
      if (!changes.contains(uri)) changes[uri] = Value::array();
      const std::string* text = compilation.textForPath(path);
      static const std::string empty;
      Value edit = Value::object();
      edit["range"] = rangeValue(text ? *text : empty, span);
      edit["newText"] = newName;
      changes[uri].push(std::move(edit));
    }

    Value workspaceEdit = Value::object();
    workspaceEdit["changes"] = std::move(changes);
    reply(id, std::move(workspaceEdit));
  }

  void documentHighlights(const Value& id, const Value* params) {
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
    ResolvedSymbol target =
        a::lsp::resolveAt(compilation, *file, *tokens, line + 1, byteColumn);
    if (!target.valid()) return replyNull(id);

    Value highlights = Value::array();
    for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind != Tok::Ident) continue;
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, i);
      if (!resolved.valid()) continue;
      if (resolved.symbol->span.line != target.symbol->span.line) continue;
      if (resolved.symbol->span.col != target.symbol->span.col) continue;
      if (canonicalPath(resolved.file->path) != canonicalPath(target.file->path)) continue;
      Value highlight = Value::object();
      highlight["range"] = rangeValue(document->text, token.span);
      highlight["kind"] = 1;  // Text
      highlights.push(std::move(highlight));
    }
    reply(id, std::move(highlights));
  }

  void foldingRanges(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const std::vector<Token>* tokens = document->compilation->entryTokens();
    if (!tokens) return replyNull(id);

    Value result = Value::array();
    std::vector<int> starts;
    int lastCodeLine = 0;
    auto addRange = [&](int startLine, int endLine) {
      if (startLine <= 0 || endLine <= startLine) return;
      Value range = Value::object();
      range["startLine"] = startLine - 1;
      range["endLine"] = endLine - 1;
      range["kind"] = "region";
      result.push(std::move(range));
    };

    for (const Token& token : *tokens) {
      switch (token.kind) {
        case Tok::Indent:
          starts.push_back(lastCodeLine > 0 ? lastCodeLine : token.span.line);
          break;
        case Tok::Dedent:
          if (!starts.empty()) {
            const int startLine = starts.back();
            starts.pop_back();
            addRange(startLine, lastCodeLine);
          }
          break;
        case Tok::Newline:
        case Tok::Eof:
          break;
        default:
          if (token.span.endLine > 0) lastCodeLine = token.span.endLine;
          break;
      }
    }
    while (!starts.empty()) {
      addRange(starts.back(), lastCodeLine);
      starts.pop_back();
    }
    reply(id, std::move(result));
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
      const std::string documentation =
          docCommentText(compilation, *resolved.file, resolved.symbol->span);
      if (!documentation.empty()) markdown += "\n\n" + documentation;
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
      int index = tokenIndexAt(*tokens, line + 1, byteColumn);

      // `module.member` and primitive `receiver.method` forms.
      if (index >= 2) {
        const Token& dot = (*tokens)[static_cast<std::size_t>(index - 1)];
        const Token& baseToken = (*tokens)[static_cast<std::size_t>(index - 2)];
        if ((dot.kind == Tok::Dot || dot.kind == Tok::QuestionDot) &&
            baseToken.kind == Tok::Ident) {
          const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
          if (!moduleName.empty()) {
            const std::string* builtinName = a::nativeFunction(moduleName, token->lexeme);
            if (!builtinName) builtinName = a::nativeConstant(moduleName, token->lexeme);
            if (builtinName) {
              if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) {
                reply(id, hoverMarkdown(doc->signature, doc->documentation));
                return;
              }
            }
          }
          ResolvedSymbol base =
              a::lsp::resolveToken(compilation, *file, *tokens, index - 2);
          if (base.valid()) {
            if (const std::vector<a::MethodSig>* table =
                    methodTableForType(base.symbol->typeName)) {
              if (const a::MethodSig* method = findMethodSig(*table, token->lexeme)) {
                reply(id, hoverMarkdown(method->signature, method->documentation));
                return;
              }
            }
          }
        }
      }

      if (!nativeModuleForAlias(*file, token->lexeme).empty()) {
        const std::string moduleName = nativeModuleForAlias(*file, token->lexeme);
        reply(id, hoverMarkdown("import " + moduleName,
                                "Native standard-library module."));
        return;
      }
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(token->lexeme)) {
        reply(id, hoverMarkdown(doc->signature, doc->documentation));
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

    auto pushItem = [&](const std::string& name, int kind, const std::string& detail,
                        const std::string& documentation = {}) {
      if (!seen.insert(name).second) return;
      Value item = Value::object();
      item["label"] = name;
      item["kind"] = kind;
      item["insertText"] = name;
      if (!detail.empty()) item["detail"] = detail;
      if (!documentation.empty()) {
        Value data = Value::object();
        data["documentation"] = documentation;
        item["data"] = std::move(data);
      }
      items.push(std::move(item));
    };

    auto pushMethods = [&](const std::vector<a::MethodSig>& table) {
      for (const a::MethodSig& method : table) {
        pushItem(method.name, 2, method.signature, method.documentation);
      }
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

    if (base >= 0) {
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

      if (baseToken.kind == Tok::Ident) {
        const std::string moduleName = nativeModuleForAlias(*file, baseToken.lexeme);
        if (!moduleName.empty()) {
          for (const a::lsp::MemberInfo& member : a::lsp::nativeModuleMembers(moduleName)) {
            pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                     member.documentation);
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
          if (const std::vector<a::MethodSig>* table = methodTableForType(typeName)) {
            pushMethods(*table);
          } else if (!typeName.empty()) {
            for (const SymbolDef* member :
                 a::lsp::structMembers(compilation, *resolved.file, typeName)) {
              pushItem(member->name, completionKind(member->kind), member->detail);
            }
          }
        }
      } else if (baseToken.kind == Tok::String) {
        pushMethods(a::stringMethods());
      } else if (baseToken.kind == Tok::RBracket) {
        pushMethods(a::arrayMethods());
      } else if (baseToken.kind == Tok::RBrace) {
        // Distinguish `{key: value}` maps from `{element}` sets by looking
        // for a colon at the literal's top level.
        bool isMap = false;
        int depth = 0;
        for (int i = base - 1; i >= 0; --i) {
          const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
          if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
              inner.kind == Tok::RParen) {
            ++depth;
          } else if (inner.kind == Tok::LBrace) {
            if (depth == 0) break;
            --depth;
          } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
            --depth;
          } else if (inner.kind == Tok::Colon && depth == 0) {
            isMap = true;
            break;
          }
        }
        pushMethods(isMap ? a::mapMethods() : a::setMethods());
      }

      // Never fall back to the global list after a dot; empty is correct
      // for receivers with no members.
      Value result = Value::object();
      result["isIncomplete"] = false;
      result["items"] = std::move(items);
      reply(id, std::move(result));
      return;
    }

    for (const char* keyword : kKeywords) pushItem(keyword, 14, "keyword");
    for (const auto& entry : a::builtinDocs()) {
      const std::string& name = entry.first;
      if (a::isStdlibBuiltin(name) || name == "PI" || name == "E") continue;
      pushItem(name, 3, entry.second.signature);
    }
    for (const SymbolDef* symbol :
         a::lsp::visibleSymbols(compilation, *file, line + 1, byteColumn)) {
      pushItem(symbol->name, completionKind(symbol->kind), symbol->detail);
    }
    for (const a::lsp::ImportDef& import : file->imports) {
      pushItem(import.alias, import.wholeModule ? 9 : 18, import.modulePath);
    }

    // `import <TAB>`, `from <TAB>`, and `from module import <TAB>`.
    std::string currentLine = lineText(document->text, line + 1);
    std::string prefix = currentLine.substr(
        0, std::min<std::size_t>(byteColumn - 1, currentLine.size()));
    const bool insideWord =
        !prefix.empty() && !std::isspace(static_cast<unsigned char>(prefix.back()));
    std::vector<std::string> words;
    {
      std::istringstream stream(prefix);
      std::string word;
      while (stream >> word) words.push_back(word);
    }
    std::string previousWord;
    if (!words.empty()) {
      previousWord =
          insideWord && words.size() >= 2 ? words[words.size() - 2] : words.back();
    }
    if (previousWord == "import" || previousWord == "from") {
      if (previousWord == "from" && words.size() >= 2) {
        const std::size_t needed = insideWord ? 3 : 2;
        if (words.size() >= needed) {
          const std::size_t moduleIndex = insideWord ? words.size() - 3 : words.size() - 2;
          if (a::isNativeModule(words[moduleIndex])) {
            for (const a::lsp::MemberInfo& member :
                 a::lsp::nativeModuleMembers(words[moduleIndex])) {
              pushItem(member.name, member.isConstant ? 21 : 3, member.signature,
                       member.documentation);
            }
          }
        }
      } else {
        for (const auto& module : a::nativeModules()) {
          pushItem(module.first, 9, "native module");
        }
      }
    }

    Value result = Value::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    reply(id, std::move(result));
  }

  void completionResolve(const Value& id, const Value* params) {
    if (!params || !params->isObject()) return replyNull(id);
    const Value* item = params;
    if (const Value* nested = params->find("item"); nested && nested->isObject()) item = nested;
    Value result = *item;
    const Value* data = item->find("data");
    const Value* documentation = data && data->isObject() ? data->find("documentation") : nullptr;
    if (documentation && documentation->isString()) {
      Value contents = Value::object();
      contents["kind"] = "markdown";
      contents["value"] = documentation->asString();
      result["documentation"] = std::move(contents);
    }
    reply(id, std::move(result));
  }

  void signatureHelp(const Value& id, const Value* params) {
    Document* document = documentFrom(params);
    if (!document || !document->compilation) return replyNull(id);
    const Compilation& compilation = *document->compilation;
    const FileIndex* file = compilation.entryIndex();
    const std::vector<Token>* tokens = compilation.entryTokens();
    if (!file || !tokens) return replyNull(id);

    int line = 0;
    int character = 0;
    if (!positionFrom(params, &line, &character)) return replyNull(id);
    const int cursorLine = line + 1;
    const int byteColumn = utf16ToByteColumn(lineText(document->text, cursorLine), character) + 1;

    // Innermost unmatched bracket whose opening is before the cursor.
    std::vector<int> stack;
    for (int i = 0; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind == Tok::Eof) break;
      if (token.span.line > cursorLine ||
          (token.span.line == cursorLine && token.span.col >= byteColumn)) {
        break;
      }
      if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
        stack.push_back(i);
      } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
                 token.kind == Tok::RBrace) {
        if (!stack.empty()) stack.pop_back();
      }
    }
    if (stack.empty() || (*tokens)[static_cast<std::size_t>(stack.back())].kind != Tok::LParen) {
      return replyNull(id);
    }
    const int openIndex = stack.back();
    if (openIndex == 0) return replyNull(id);

    // Callee: `name(`, `module.member(`, or `receiver.method(`.
    std::string label;
    const Token& callee = (*tokens)[static_cast<std::size_t>(openIndex - 1)];
    if (callee.kind != Tok::Ident) return replyNull(id);
    const bool qualified = openIndex >= 3 &&
                           ((*tokens)[static_cast<std::size_t>(openIndex - 2)].kind == Tok::Dot ||
                            (*tokens)[static_cast<std::size_t>(openIndex - 2)].kind ==
                                Tok::QuestionDot);
    const Token& baseToken =
        qualified ? (*tokens)[static_cast<std::size_t>(openIndex - 3)] : callee;
    const bool qualifiedOnDot =
        qualified && (baseToken.kind == Tok::Ident || baseToken.kind == Tok::String ||
                      baseToken.kind == Tok::RBracket || baseToken.kind == Tok::RBrace);
    if (!qualified && baseToken.kind != Tok::Ident) return replyNull(id);

    const std::string moduleAlias =
        qualifiedOnDot && baseToken.kind == Tok::Ident ? nativeModuleForAlias(*file, baseToken.lexeme)
                                                       : std::string();
    if (!moduleAlias.empty()) {
      const std::string* builtinName = a::nativeFunction(moduleAlias, callee.lexeme);
      if (!builtinName) builtinName = a::nativeConstant(moduleAlias, callee.lexeme);
      if (builtinName) {
        if (const a::BuiltinDoc* doc = a::findBuiltinDoc(*builtinName)) label = doc->signature;
      }
    }
    if (label.empty()) {
      const bool primitiveBase = baseToken.kind != Tok::Ident &&
                                 baseToken.kind != Tok::Eof &&
                                 (baseToken.kind == Tok::String ||
                                  baseToken.kind == Tok::RBracket ||
                                  baseToken.kind == Tok::RBrace);
      std::string typeName;
      if (primitiveBase) {
        typeName = baseToken.kind == Tok::String
                       ? "String"
                       : (baseToken.kind == Tok::RBracket ? "array" : "map");
        if (baseToken.kind == Tok::RBrace) {
          int depth = 0;
          bool isMap = false;
          for (int i = openIndex - 4; i >= 0; --i) {
            const Token& inner = (*tokens)[static_cast<std::size_t>(i)];
            if (inner.kind == Tok::RBrace || inner.kind == Tok::RBracket ||
                inner.kind == Tok::RParen) {
              ++depth;
            } else if (inner.kind == Tok::LBrace) {
              if (depth == 0) break;
              --depth;
            } else if (inner.kind == Tok::LBracket || inner.kind == Tok::LParen) {
              --depth;
            } else if (inner.kind == Tok::Colon && depth == 0) {
              isMap = true;
              break;
            }
          }
          typeName = isMap ? "map" : "set";
        }
      } else if (baseToken.kind == Tok::Ident) {
        ResolvedSymbol resolved =
            a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 3);
        if (resolved.valid()) typeName = resolved.symbol->typeName;
      }
      const std::vector<a::MethodSig>* table = methodTableForType(typeName);
      if (table) {
        if (const a::MethodSig* method = findMethodSig(*table, callee.lexeme)) {
          label = method->signature;
        }
      }
    }
    if (label.empty() && baseToken.kind == Tok::Ident) {
      ResolvedSymbol resolved = a::lsp::resolveToken(compilation, *file, *tokens, openIndex - 1);
      if (resolved.valid() && !resolved.symbol->detail.empty()) {
        label = resolved.symbol->detail;
      }
    }
    if (label.empty() && qualifiedOnDot && baseToken.kind == Tok::Ident) {
      auto found = compilation.sema.functions.find(baseToken.lexeme + "." + callee.lexeme);
      if (found == compilation.sema.functions.end()) {
        found = compilation.sema.functions.find(callee.lexeme);
      }
      if (found != compilation.sema.functions.end() && found->second) {
        label = a::lsp::signatureText(*found->second);
      }
    }
    if (label.empty() && !qualifiedOnDot) {
      if (const a::BuiltinDoc* doc = a::findBuiltinDoc(callee.lexeme)) label = doc->signature;
    }
    if (label.empty()) return replyNull(id);

    // Split the parameter list of the label into display segments.
    std::vector<std::string> parameters;
    std::size_t start = label.find('(');
    if (start != std::string::npos) {
      int depth = 0;
      std::size_t segmentStart = start + 1;
      for (std::size_t i = start; i < label.size(); ++i) {
        char c = label[i];
        if (c == '(' || c == '[' || c == '{') {
          ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
          --depth;
          if (depth == 0) {
            std::string last = label.substr(segmentStart, i - segmentStart);
            if (!last.empty()) parameters.push_back(last);
            break;
          }
        } else if (c == ',' && depth == 1) {
          parameters.push_back(label.substr(segmentStart, i - segmentStart));
          segmentStart = i + 1;
        }
      }
    }

    // Active parameter: top-level commas between the open paren and cursor.
    int activeParameter = 0;
    int depth = 0;
    for (int i = openIndex + 1; i < static_cast<int>(tokens->size()); ++i) {
      const Token& token = (*tokens)[static_cast<std::size_t>(i)];
      if (token.kind == Tok::Eof) break;
      if (token.span.line > cursorLine ||
          (token.span.line == cursorLine && token.span.col >= byteColumn)) {
        break;
      }
      if (token.kind == Tok::LParen || token.kind == Tok::LBracket || token.kind == Tok::LBrace) {
        ++depth;
      } else if (token.kind == Tok::RParen || token.kind == Tok::RBracket ||
                 token.kind == Tok::RBrace) {
        if (depth == 0) break;
        --depth;
      } else if (token.kind == Tok::Comma && depth == 0) {
        ++activeParameter;
      }
    }
    if (activeParameter > 0 && parameters.empty()) activeParameter = 0;
    if (static_cast<std::size_t>(activeParameter) >= parameters.size() && !parameters.empty()) {
      activeParameter = static_cast<int>(parameters.size()) - 1;
    }

    Value signature = Value::object();
    signature["label"] = label;
    Value parameterValues = Value::array();
    for (const std::string& parameter : parameters) {
      Value entry = Value::object();
      std::string text = parameter;
      while (!text.empty() && text.front() == ' ') text.erase(text.begin());
      entry["label"] = text;
      parameterValues.push(std::move(entry));
    }
    if (!parameters.empty()) signature["parameters"] = std::move(parameterValues);
    Value signatures = Value::array();
    signatures.push(std::move(signature));

    Value result = Value::object();
    result["signatures"] = std::move(signatures);
    result["activeSignature"] = 0;
    result["activeParameter"] = activeParameter;
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
      std::cout << "a-language-server " << kServerVersion << "\n";
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: a-lsp [--stdio]\n"
                   "\n"
                   "A language server for the A programming language. It speaks the\n"
                   "Language Server Protocol over stdin/stdout. Editors normally start it\n"
                   "automatically; pass --stdio when they require an explicit transport.\n";
      return 0;
    }
  }

  std::ios::sync_with_stdio(false);
  LanguageServer server;
  return server.run();
}
