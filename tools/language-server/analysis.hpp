#pragma once

// Semantic index for the A language server. The compiler already provides the
// lexer, parser, module loader, and type checker; this layer keeps the parsed
// program around and records the declaration/scope information that an editor
// needs for go-to-definition, references, hover, and completion.

#include <memory>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "diagnostics/diagnostics.hpp"
#include "lexer/lexer.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/types.hpp"

namespace a::lsp {

enum class SymbolKind : int {
  File = 1,
  Module = 2,
  Namespace = 3,
  Package = 4,
  Class = 5,
  Method = 6,
  Property = 7,
  Field = 8,
  Constructor = 9,
  Enum = 10,
  Interface = 11,
  Function = 12,
  Variable = 13,
  Constant = 14,
  String = 15,
  Number = 16,
  Boolean = 17,
  Array = 18,
  Object = 19,
  Key = 20,
  Null = 21,
  EnumMember = 22,
  Struct = 23,
  Event = 24,
  Operator = 25,
  TypeParameter = 26,
};

enum class CompletionKind : int {
  Text = 1,
  Method = 2,
  Function = 3,
  Constructor = 4,
  Field = 5,
  Variable = 6,
  Class = 7,
  Interface = 8,
  Module = 9,
  Property = 10,
  Unit = 11,
  Value = 12,
  Enum = 13,
  Keyword = 14,
  Snippet = 15,
  Color = 16,
  File = 17,
  Reference = 18,
  Folder = 19,
  EnumMember = 20,
  Constant = 21,
  Struct = 22,
  Event = 23,
  Operator = 24,
  TypeParameter = 25,
};

struct ScopeInfo {
  int parent = -1;
  int depth = 0;
  Span span;
};

struct SymbolDef {
  std::string name;
  Span span;  // the name token
  SymbolKind kind = SymbolKind::Variable;
  std::string detail;
  std::string typeName;  // struct/enum type for variables/fields
  int scope = 0;
  bool hoisted = false;
  bool topLevel = false;
  int memberScope = -1;  // struct fields, methods, and enum variants
};

// `alias` refers either to a whole module or to one name imported with
// `from ... import ...`.
struct ImportDef {
  std::string alias;
  std::string modulePath;
  int moduleId = -1;
  std::string member;
  bool wholeModule = true;
  Span span;
};

struct FileIndex {
  std::string path;
  std::vector<ScopeInfo> scopes;
  std::vector<SymbolDef> symbols;
  std::vector<ImportDef> imports;
};

struct Compilation {
  std::string path;
  std::string text;
  DiagnosticSink sink;
  ModuleGraph graph;
  SemaResult sema;
  std::vector<FileIndex> indexes;                  // by module id
  std::vector<std::vector<Token>> tokensByModule;  // by module id
  std::vector<std::vector<Comment>> commentsByModule;  // by module id
  std::vector<std::string> textsByModule;          // by module id
  bool analyzed = false;                           // Sema ran without errors

  const FileIndex* entryIndex() const;
  const FileIndex* indexForModule(int moduleId) const;
  const std::vector<Token>* entryTokens() const;
  const std::vector<Comment>* commentsForModule(int moduleId) const;
  const std::string* textForModule(int moduleId) const;
  const std::string* textForPath(const std::string& path) const;
};

struct ResolvedSymbol {
  const SymbolDef* symbol = nullptr;
  const FileIndex* file = nullptr;
  bool valid() const { return symbol != nullptr && file != nullptr; }
};

std::unique_ptr<Compilation> analyzeDocument(const std::string& path, const std::string& text,
                                             const std::vector<std::string>& modulePaths,
                                             const std::string& stdlibRoot);

// Resolves the identifier at `line`/`byteColumn` (both 1-based, byte columns).
ResolvedSymbol resolveAt(const Compilation& compilation, const FileIndex& file,
                         const std::vector<Token>& tokens, int line, int byteColumn);

// Resolves a specific identifier token. Used by reference search.
ResolvedSymbol resolveToken(const Compilation& compilation, const FileIndex& file,
                            const std::vector<Token>& tokens, int tokenIndex);

const SymbolDef* findTopLevel(const FileIndex& file, const std::string& name);
const SymbolDef* findStructMember(const Compilation& compilation, const FileIndex& file,
                                  const std::string& typeName, const std::string& member);
std::vector<const SymbolDef*> structMembers(const Compilation& compilation,
                                            const FileIndex& file, const std::string& typeName);

// All names visible at a position (scope chain plus imports and builtins).
std::vector<const SymbolDef*> visibleSymbols(const Compilation& compilation,
                                             const FileIndex& file, int line, int byteColumn);

std::string typeExprText(const TypeExpr* type);
std::string signatureText(const FnDecl& decl);
std::string describeSymbol(const SymbolDef& symbol);

}  // namespace a::lsp
