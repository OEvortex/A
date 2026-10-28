#include "analysis.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace a::lsp {
namespace {

namespace fs = std::filesystem;

const Span kWholeFileSpan{1, 1, INT_MAX / 2, 1, 0};

bool startsBeforeOrAt(const Span& span, int line, int col) {
  if (!span.valid()) return false;
  if (line != span.line) return line > span.line;
  return col >= span.col;
}

bool startsAfter(const Span& a, const Span& b) {
  if (a.line != b.line) return a.line > b.line;
  return a.col > b.col;
}

bool spanContains(const Span& span, int line, int col) {
  if (!span.valid()) return false;
  const bool afterStart = line > span.line || (line == span.line && col >= span.col);
  const bool beforeEnd = line < span.endLine || (line == span.endLine && col <= span.endCol);
  return afterStart && beforeEnd;
}

Span blockSpan(const std::vector<StmtPtr>& body, const Span& fallback) {
  if (body.empty()) return fallback;
  Span span = body.front()->span;
  if (body.back()->span.valid()) {
    span.endLine = body.back()->span.endLine;
    span.endCol = body.back()->span.endCol;
  }
  return span;
}

std::string readFileText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string canonicalPath(const std::string& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

std::string moduleStem(const std::string& path) {
  std::size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
  }
  if (name.empty()) name = "module";
  return name;
}

std::string importAlias(const std::string& explicitAlias, const std::string& path,
                        const std::string& bareName) {
  if (!explicitAlias.empty()) return explicitAlias;
  if (!path.empty()) return moduleStem(path);
  return bareName;
}

std::string lastSegment(const std::string& name) {
  std::size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

std::string exprTypeName(const Expr* expr) {
  if (!expr) return {};
  switch (expr->kind) {
    case ExprKind::IntLit: return "i32";
    case ExprKind::FloatLit: return "f64";
    case ExprKind::BoolLit: return "bool";
    case ExprKind::CharLit: return "char";
    case ExprKind::StringLit: return "String";
    case ExprKind::ArrayLit: return "array";
    case ExprKind::Tuple: return "tuple";
    case ExprKind::Lambda: return "fn";
    case ExprKind::Call: {
      const auto& call = static_cast<const CallExpr&>(*expr);
      if (call.callee && call.callee->kind == ExprKind::Ident) {
        const auto& ident = static_cast<const IdentExpr&>(*call.callee);
        if (!ident.name.empty() && std::isupper(static_cast<unsigned char>(ident.name[0]))) {
          return ident.name;
        }
      }
      return {};
    }
    default:
      return {};
  }
}

std::string varDetail(const std::string& name, const std::string& type, bool mutableBinding,
                      bool constant) {
  std::string detail;
  if (constant) {
    detail = "const " + name;
  } else {
    detail = (mutableBinding ? "var " : "") + name;
  }
  if (!type.empty()) detail += ": " + type;
  return detail;
}

class IndexBuilder {
 public:
  IndexBuilder(FileIndex& file, int moduleId) : file_(file), moduleId_(moduleId) {}

  void build(Program& program) {
    file_.scopes.push_back(ScopeInfo{-1, 0, kWholeFileSpan});
    walkBlock(program.statements, 0);
  }

 private:
  int addScope(Span span, int parent) {
    int depth = parent >= 0 ? file_.scopes[static_cast<std::size_t>(parent)].depth + 1 : 0;
    file_.scopes.push_back(ScopeInfo{parent, depth, span});
    return static_cast<int>(file_.scopes.size()) - 1;
  }

  int addSymbol(const std::string& name, Span span, SymbolKind kind, std::string detail,
                std::string typeName, int scope, bool hoisted) {
    if (name.empty()) return -1;
    SymbolDef symbol;
    symbol.name = name;
    symbol.span = span;
    symbol.kind = kind;
    symbol.detail = std::move(detail);
    symbol.typeName = std::move(typeName);
    symbol.scope = scope;
    symbol.hoisted = hoisted;
    symbol.topLevel = scope == 0;
    file_.symbols.push_back(std::move(symbol));
    return static_cast<int>(file_.symbols.size()) - 1;
  }

  bool visibleInScopeChain(int scope, const std::string& name) const {
    for (int current = scope; current >= 0;
         current = file_.scopes[static_cast<std::size_t>(current)].parent) {
      for (const SymbolDef& symbol : file_.symbols) {
        if (symbol.scope == current && symbol.name == name) return true;
      }
    }
    return false;
  }

  void walkBlock(const std::vector<StmtPtr>& body, int scope) {
    for (const auto& statement : body) {
      if (statement) walkStmt(*statement, scope);
    }
  }

  void walkBody(const std::vector<StmtPtr>& body, const Span& fallback, int parentScope) {
    int scope = addScope(blockSpan(body, fallback), parentScope);
    walkBlock(body, scope);
  }

  void walkStmt(Stmt& stmt, int scope) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        auto& decl = static_cast<LetStmt&>(stmt);
        std::string type =
            decl.type ? typeExprText(decl.type.get()) : exprTypeName(decl.init.get());
        addSymbol(decl.name, decl.nameSpan,
                  decl.constantBinding ? SymbolKind::Constant : SymbolKind::Variable,
                  varDetail(decl.name, type, decl.mutableBinding, decl.constantBinding), type,
                  scope, false);
        walkExpr(decl.init.get(), scope);
        return;
      }
      case StmtKind::Assign: {
        auto& assign = static_cast<AssignStmt&>(stmt);
        if (assign.target && assign.target->kind == ExprKind::Ident) {
          auto& ident = static_cast<IdentExpr&>(*assign.target);
          if (!visibleInScopeChain(scope, ident.name)) {
            std::string type = exprTypeName(assign.value.get());
            addSymbol(ident.name, ident.span, SymbolKind::Variable,
                      varDetail(ident.name, type, true, false), type, scope, false);
          }
        } else {
          walkExpr(assign.target.get(), scope);
        }
        walkExpr(assign.value.get(), scope);
        return;
      }
      case StmtKind::Destructure: {
        auto& decl = static_cast<DestructureStmt&>(stmt);
        for (std::size_t i = 0; i < decl.names.size(); ++i) {
          if (decl.names[i] == "_") continue;
          Span span = i < decl.nameSpans.size() ? decl.nameSpans[i] : decl.span;
          addSymbol(decl.names[i], span, SymbolKind::Variable,
                    varDetail(decl.names[i], {}, decl.mutableBinding, false), {}, scope, false);
        }
        walkExpr(decl.value.get(), scope);
        return;
      }
      case StmtKind::Expr:
        walkExpr(static_cast<ExprStmt&>(stmt).expr.get(), scope);
        return;
      case StmtKind::Return:
        walkExpr(static_cast<ReturnStmt&>(stmt).value.get(), scope);
        return;
      case StmtKind::Break:
      case StmtKind::Continue:
        return;
      case StmtKind::Defer: {
        auto& defer = static_cast<DeferStmt&>(stmt);
        if (defer.body) walkStmt(*defer.body, scope);
        return;
      }
      case StmtKind::If: {
        auto& ifStmt = static_cast<IfStmt&>(stmt);
        for (auto& branch : ifStmt.branches) {
          walkExpr(branch.cond.get(), scope);
          walkBody(branch.body, stmt.span, scope);
        }
        walkBody(ifStmt.elseBody, stmt.span, scope);
        return;
      }
      case StmtKind::While: {
        auto& whileStmt = static_cast<WhileStmt&>(stmt);
        walkExpr(whileStmt.cond.get(), scope);
        walkBody(whileStmt.body, stmt.span, scope);
        return;
      }
      case StmtKind::DoWhile: {
        auto& doStmt = static_cast<DoWhileStmt&>(stmt);
        walkBody(doStmt.body, stmt.span, scope);
        walkExpr(doStmt.cond.get(), scope);
        return;
      }
      case StmtKind::For: {
        auto& forStmt = static_cast<ForStmt&>(stmt);
        walkExpr(forStmt.iterable.get(), scope);
        int loopScope = addScope(stmt.span, scope);
        addSymbol(forStmt.var, forStmt.varSpan, SymbolKind::Variable,
                  varDetail(forStmt.var, {}, false, false), {}, loopScope, true);
        walkBlock(forStmt.body, loopScope);
        return;
      }
      case StmtKind::Loop: {
        auto& loop = static_cast<LoopStmt&>(stmt);
        walkBody(loop.body, stmt.span, scope);
        return;
      }
      case StmtKind::FnDecl: {
        auto& fn = static_cast<FnDecl&>(stmt);
        addSymbol(fn.name, fn.nameSpan, SymbolKind::Function, signatureText(fn), {}, scope, true);
        Span bodySpan = blockSpan(fn.body, stmt.span);
        int fnScope = addScope(bodySpan, scope);
        for (const auto& param : fn.params) {
          std::string type = param.type ? typeExprText(param.type.get()) : std::string();
          addSymbol(param.name, param.span, SymbolKind::Variable,
                    varDetail(param.name, type, false, false), type, fnScope, true);
        }
        walkBlock(fn.body, fnScope);
        return;
      }
      case StmtKind::StructDecl: {
        auto& decl = static_cast<StructDecl&>(stmt);
        const SymbolKind kind = decl.isClass ? SymbolKind::Class : SymbolKind::Struct;
        int structIndex =
            addSymbol(decl.name, decl.nameSpan, kind, (decl.isClass ? "class " : "struct ") +
                                                         decl.name,
                      decl.name, scope, true);
        int memberScope = addScope(stmt.span, scope);
        if (structIndex >= 0) file_.symbols[static_cast<std::size_t>(structIndex)].memberScope =
            memberScope;
        for (auto& field : decl.fields) {
          std::string type = field.type ? typeExprText(field.type.get()) : std::string();
          addSymbol(field.name, field.span, SymbolKind::Field,
                    varDetail(field.name, type, false, false), type, memberScope, true);
        }
        for (auto& method : decl.methods) {
          if (!method) continue;
          addSymbol(method->name, method->nameSpan, SymbolKind::Method, signatureText(*method),
                    {}, memberScope, true);
          Span bodySpan = blockSpan(method->body, method->span);
          int fnScope = addScope(bodySpan, memberScope);
          for (const auto& param : method->params) {
            std::string type = param.type ? typeExprText(param.type.get()) : std::string();
            addSymbol(param.name, param.span, SymbolKind::Variable,
                      varDetail(param.name, type, false, false), type, fnScope, true);
          }
          walkBlock(method->body, fnScope);
        }
        return;
      }
      case StmtKind::EnumDecl: {
        auto& decl = static_cast<EnumDecl&>(stmt);
        int enumIndex =
            addSymbol(decl.name, decl.nameSpan, SymbolKind::Enum, "enum " + decl.name, decl.name,
                      scope, true);
        int memberScope = addScope(stmt.span, scope);
        if (enumIndex >= 0) file_.symbols[static_cast<std::size_t>(enumIndex)].memberScope =
            memberScope;
        for (auto& variant : decl.variants) {
          addSymbol(variant.name, variant.span, SymbolKind::EnumMember,
                    decl.name + "." + variant.name, decl.name, memberScope, true);
          walkExpr(variant.value.get(), scope);
        }
        return;
      }
      case StmtKind::TypeAlias: {
        auto& alias = static_cast<TypeAliasDecl&>(stmt);
        addSymbol(alias.name, alias.nameSpan, SymbolKind::TypeParameter, "type " + alias.name, "", scope, true);
        return;
      }
      case StmtKind::Import: {
        auto& import = static_cast<ImportStmt&>(stmt);
        ImportDef def;
        def.alias = importAlias(import.alias, import.path, import.bareName);
        def.modulePath = import.path.empty() ? import.bareName : import.path;
        def.moduleId = import.moduleId;
        def.wholeModule = true;
        def.span = import.targetSpan;
        file_.imports.push_back(std::move(def));
        return;
      }
      case StmtKind::FromImport: {
        auto& import = static_cast<FromImportStmt&>(stmt);
        std::string path = import.path.empty() ? import.bareName : import.path;
        for (const auto& item : import.items) {
          ImportDef def;
          def.alias = item.alias;
          def.modulePath = path;
          def.moduleId = import.moduleId;
          def.member = item.name;
          def.wholeModule = false;
          def.span = item.span;
          file_.imports.push_back(std::move(def));
        }
        return;
      }
    }
  }

  void walkExpr(Expr* expr, int scope) {
    if (!expr) return;
    switch (expr->kind) {
      case ExprKind::Unary:
        walkExpr(static_cast<UnaryExpr&>(*expr).operand.get(), scope);
        return;
      case ExprKind::Binary: {
        auto& binary = static_cast<BinaryExpr&>(*expr);
        walkExpr(binary.lhs.get(), scope);
        walkExpr(binary.rhs.get(), scope);
        return;
      }
      case ExprKind::Call: {
        auto& call = static_cast<CallExpr&>(*expr);
        walkExpr(call.callee.get(), scope);
        for (auto& arg : call.args) walkExpr(arg.value.get(), scope);
        return;
      }
      case ExprKind::Index: {
        auto& index = static_cast<IndexExpr&>(*expr);
        walkExpr(index.object.get(), scope);
        walkExpr(index.index.get(), scope);
        return;
      }
      case ExprKind::Slice: {
        auto& slice = static_cast<SliceExpr&>(*expr);
        walkExpr(slice.object.get(), scope);
        walkExpr(slice.start.get(), scope);
        walkExpr(slice.end.get(), scope);
        walkExpr(slice.step.get(), scope);
        return;
      }
      case ExprKind::Field:
        walkExpr(static_cast<FieldExpr&>(*expr).object.get(), scope);
        return;
      case ExprKind::SafeNav:
        walkExpr(static_cast<SafeNavExpr&>(*expr).object.get(), scope);
        return;
      case ExprKind::ArrayLit: {
        for (auto& element : static_cast<ArrayLitExpr&>(*expr).elements) {
          walkExpr(element.get(), scope);
        }
        return;
      }
      case ExprKind::Range: {
        auto& range = static_cast<RangeExpr&>(*expr);
        walkExpr(range.start.get(), scope);
        walkExpr(range.end.get(), scope);
        walkExpr(range.step.get(), scope);
        return;
      }
      case ExprKind::Ternary: {
        auto& ternary = static_cast<TernaryExpr&>(*expr);
        walkExpr(ternary.cond.get(), scope);
        walkExpr(ternary.thenValue.get(), scope);
        walkExpr(ternary.elseValue.get(), scope);
        return;
      }
      case ExprKind::Tuple: {
        for (auto& element : static_cast<TupleExpr&>(*expr).elements) {
          walkExpr(element.get(), scope);
        }
        return;
      }
      case ExprKind::Lambda: {
        auto& lambda = static_cast<LambdaExpr&>(*expr);
        int lambdaScope = addScope(lambda.span, scope);
        for (const auto& param : lambda.params) {
          std::string type = param.type ? typeExprText(param.type.get()) : std::string();
          addSymbol(param.name, param.span, SymbolKind::Variable,
                    varDetail(param.name, type, false, false), type, lambdaScope, true);
        }
        walkExpr(lambda.body.get(), lambdaScope);
        return;
      }
      case ExprKind::Comprehension: {
        auto& comprehension = static_cast<ComprehensionExpr&>(*expr);
        walkExpr(comprehension.iterable.get(), scope);
        int compScope = addScope(comprehension.span, scope);
        addSymbol(comprehension.var, comprehension.varSpan, SymbolKind::Variable,
                  varDetail(comprehension.var, {}, false, false), {}, compScope, true);
        walkExpr(comprehension.element.get(), compScope);
        walkExpr(comprehension.condition.get(), compScope);
        return;
      }
      case ExprKind::Match: {
        auto& match = static_cast<MatchExpr&>(*expr);
        walkExpr(match.subject.get(), scope);
        for (auto& arm : match.arms) {
          walkExpr(arm.pattern.value.get(), scope);
          walkExpr(arm.pattern.rangeStart.get(), scope);
          walkExpr(arm.pattern.rangeEnd.get(), scope);
          walkExpr(arm.guard.get(), scope);
          walkExpr(arm.result.get(), scope);
        }
        return;
      }
      case ExprKind::StringLit: {
        for (auto& part : static_cast<StringLitExpr&>(*expr).parts) {
          walkExpr(part.expr.get(), scope);
        }
        return;
      }
      default:
        return;
    }
  }

  FileIndex& file_;
  int moduleId_ = -1;
};

int scopeAt(const FileIndex& file, int line, int col) {
  int best = 0;
  int bestDepth = -1;
  for (std::size_t i = 0; i < file.scopes.size(); ++i) {
    const ScopeInfo& scope = file.scopes[i];
    if (!spanContains(scope.span, line, col)) continue;
    if (scope.depth > bestDepth) {
      bestDepth = scope.depth;
      best = static_cast<int>(i);
    }
  }
  return best;
}

const SymbolDef* findGlobalTopLevel(const Compilation& compilation, const std::string& name) {
  for (const FileIndex& file : compilation.indexes) {
    if (const SymbolDef* symbol = findTopLevel(file, name)) return symbol;
  }
  return nullptr;
}

ResolvedSymbol findModuleMember(const Compilation& compilation, int moduleId,
                                const std::string& name) {
  const FileIndex* file = compilation.indexForModule(moduleId);
  if (!file) return {};
  const SymbolDef* symbol = findTopLevel(*file, name);
  if (!symbol) return {};
  return ResolvedSymbol{symbol, file};
}

ResolvedSymbol resolveName(const Compilation& compilation, const FileIndex& file, int scope,
                           const std::string& name, int line, int col) {
  for (int current = scope; current >= 0;
       current = file.scopes[static_cast<std::size_t>(current)].parent) {
    const SymbolDef* best = nullptr;
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.scope != current || symbol.name != name) continue;
      if (!symbol.hoisted && !startsBeforeOrAt(symbol.span, line, col)) continue;
      if (!best || startsAfter(symbol.span, best->span)) best = &symbol;
    }
    if (best) return ResolvedSymbol{best, &file};
  }

  for (const ImportDef& import : file.imports) {
    if (import.wholeModule || import.alias != name || import.moduleId < 0) continue;
    if (ResolvedSymbol resolved = findModuleMember(compilation, import.moduleId, import.member);
        resolved.valid()) {
      return resolved;
    }
  }

  if (const SymbolDef* symbol = findGlobalTopLevel(compilation, name)) {
    for (const FileIndex& candidate : compilation.indexes) {
      for (const SymbolDef& local : candidate.symbols) {
        if (&local == symbol) return ResolvedSymbol{&local, &candidate};
      }
    }
  }
  return {};
}

ResolvedSymbol resolveMember(const Compilation& compilation, const FileIndex& file,
                             const std::vector<Token>& tokens, int memberTokenIndex) {
  if (memberTokenIndex < 2) return {};
  const Token& baseToken = tokens[static_cast<std::size_t>(memberTokenIndex - 2)];
  const Token& memberToken = tokens[static_cast<std::size_t>(memberTokenIndex)];
  if (baseToken.kind != Tok::Ident) return {};

  for (const ImportDef& import : file.imports) {
    if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
    if (ResolvedSymbol resolved =
            findModuleMember(compilation, import.moduleId, memberToken.lexeme);
        resolved.valid()) {
      return resolved;
    }
  }

  ResolvedSymbol base = resolveToken(compilation, file, tokens, memberTokenIndex - 2);
  if (base.valid() && !base.symbol->typeName.empty()) {
    if (const SymbolDef* member =
            findStructMember(compilation, *base.file, base.symbol->typeName, memberToken.lexeme)) {
      return ResolvedSymbol{member, base.file};
    }
  }

  for (const FileIndex& candidate : compilation.indexes) {
    for (const SymbolDef& symbol : candidate.symbols) {
      if (symbol.memberScope < 0) continue;
      if (symbol.kind != SymbolKind::Struct && symbol.kind != SymbolKind::Class &&
          symbol.kind != SymbolKind::Enum) {
        continue;
      }
      if (const SymbolDef* member =
              findStructMember(compilation, candidate, symbol.name, memberToken.lexeme)) {
        return ResolvedSymbol{member, &candidate};
      }
    }
  }
  return {};
}

}  // namespace

const FileIndex* Compilation::entryIndex() const {
  const int entry = graph.entry >= 0 ? graph.entry : 0;
  return indexForModule(entry);
}

const FileIndex* Compilation::indexForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= indexes.size()) return nullptr;
  return &indexes[static_cast<std::size_t>(moduleId)];
}

const std::vector<Token>* Compilation::entryTokens() const {
  const int entry = graph.entry >= 0 ? graph.entry : 0;
  if (entry < 0 || static_cast<std::size_t>(entry) >= tokensByModule.size()) return nullptr;
  return &tokensByModule[static_cast<std::size_t>(entry)];
}

const std::vector<Comment>* Compilation::commentsForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= commentsByModule.size()) {
    return nullptr;
  }
  return &commentsByModule[static_cast<std::size_t>(moduleId)];
}

const std::string* Compilation::textForModule(int moduleId) const {
  if (moduleId < 0 || static_cast<std::size_t>(moduleId) >= textsByModule.size()) return nullptr;
  return &textsByModule[static_cast<std::size_t>(moduleId)];
}

const std::string* Compilation::textForPath(const std::string& path) const {
  const std::string canonical = canonicalPath(path);
  for (std::size_t i = 0; i < graph.modules.size() && i < textsByModule.size(); ++i) {
    if (canonicalPath(graph.modules[i].path) == canonical) return &textsByModule[i];
  }
  return nullptr;
}

std::string typeExprText(const TypeExpr* type) {
  if (!type) return {};
  if (const auto* named = dynamic_cast<const NamedTypeExpr*>(type)) {
    std::string text;
    for (const std::string& qualifier : named->qualifiers) {
      if (!text.empty()) text += ".";
      text += qualifier;
    }
    if (!text.empty()) text += ".";
    return text + named->name;
  }
  if (const auto* array = dynamic_cast<const ArrayTypeExpr*>(type)) {
    return "[" + typeExprText(array->element.get()) + "]";
  }
  if (const auto* optional = dynamic_cast<const OptionalTypeExpr*>(type)) {
    return typeExprText(optional->element.get()) + "?";
  }
  if (const auto* tuple = dynamic_cast<const TupleTypeExpr*>(type)) {
    std::string text = "(";
    for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
      if (i) text += ", ";
      text += typeExprText(tuple->elements[i].get());
    }
    return text + ")";
  }
  if (const auto* fn = dynamic_cast<const FunctionTypeExpr*>(type)) {
    std::string text = "fn(";
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      if (i) text += ", ";
      text += typeExprText(fn->params[i].get());
    }
    text += ")";
    if (fn->returnType) text += " -> " + typeExprText(fn->returnType.get());
    return text;
  }
  return "?";
}

std::string signatureText(const FnDecl& decl) {
  std::string text = "fn " + decl.name + "(";
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    if (i) text += ", ";
    const ParamDecl& param = decl.params[i];
    text += param.name;
    if (param.type) text += ": " + typeExprText(param.type.get());
  }
  text += ")";
  if (decl.returnType) text += " -> " + typeExprText(decl.returnType.get());
  return text;
}

std::string describeSymbol(const SymbolDef& symbol) {
  if (!symbol.detail.empty()) return symbol.detail;
  return symbol.name;
}

const SymbolDef* findTopLevel(const FileIndex& file, const std::string& name) {
  for (const SymbolDef& symbol : file.symbols) {
    if (symbol.topLevel && symbol.name == name) return &symbol;
  }
  return nullptr;
}

const SymbolDef* findStructMember(const Compilation& compilation, const FileIndex& file,
                                  const std::string& typeName, const std::string& member) {
  auto lookup = [&](const FileIndex& index) -> const SymbolDef* {
    const SymbolDef* typeSymbol = findTopLevel(index, lastSegment(typeName));
    if (!typeSymbol || typeSymbol->memberScope < 0) return nullptr;
    for (const SymbolDef& symbol : index.symbols) {
      if (symbol.scope == typeSymbol->memberScope && symbol.name == member) return &symbol;
    }
    return nullptr;
  };

  if (const SymbolDef* symbol = lookup(file)) return symbol;
  for (const FileIndex& candidate : compilation.indexes) {
    if (const SymbolDef* symbol = lookup(candidate)) return symbol;
  }
  return nullptr;
}

std::vector<const SymbolDef*> structMembers(const Compilation& compilation,
                                            const FileIndex& file, const std::string& typeName) {
  auto collect = [&](const FileIndex& index, std::vector<const SymbolDef*>& out) {
    const SymbolDef* typeSymbol = findTopLevel(index, lastSegment(typeName));
    if (!typeSymbol || typeSymbol->memberScope < 0) return;
    for (const SymbolDef& symbol : index.symbols) {
      if (symbol.scope == typeSymbol->memberScope) out.push_back(&symbol);
    }
  };

  std::vector<const SymbolDef*> members;
  collect(file, members);
  if (!members.empty()) return members;
  for (const FileIndex& candidate : compilation.indexes) {
    collect(candidate, members);
    if (!members.empty()) return members;
  }
  return members;
}

ResolvedSymbol resolveToken(const Compilation& compilation, const FileIndex& file,
                            const std::vector<Token>& tokens, int tokenIndex) {
  if (tokenIndex < 0 || static_cast<std::size_t>(tokenIndex) >= tokens.size()) return {};
  const Token& token = tokens[static_cast<std::size_t>(tokenIndex)];
  if (token.kind != Tok::Ident) return {};

  if (tokenIndex >= 2) {
    const Tok previous = tokens[static_cast<std::size_t>(tokenIndex - 1)].kind;
    if (previous == Tok::Dot || previous == Tok::QuestionDot) {
      if (ResolvedSymbol member = resolveMember(compilation, file, tokens, tokenIndex);
          member.valid()) {
        return member;
      }
    }
  }

  int scope = scopeAt(file, token.span.line, token.span.col);
  return resolveName(compilation, file, scope, token.lexeme, token.span.line, token.span.col);
}

ResolvedSymbol resolveAt(const Compilation& compilation, const FileIndex& file,
                         const std::vector<Token>& tokens, int line, int byteColumn) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind != Tok::Ident) continue;
    if (token.span.line != line) continue;
    if (byteColumn < token.span.col || byteColumn > token.span.endCol) continue;
    return resolveToken(compilation, file, tokens, static_cast<int>(i));
  }
  return {};
}

std::vector<const SymbolDef*> visibleSymbols(const Compilation& compilation,
                                             const FileIndex& file, int line, int byteColumn) {
  std::vector<const SymbolDef*> result;
  std::unordered_set<std::string> seen;
  int scope = scopeAt(file, line, byteColumn);
  for (int current = scope; current >= 0;
       current = file.scopes[static_cast<std::size_t>(current)].parent) {
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.scope != current) continue;
      if (!symbol.hoisted && !startsBeforeOrAt(symbol.span, line, byteColumn)) continue;
      if (!seen.insert(symbol.name).second) continue;
      result.push_back(&symbol);
    }
  }
  for (const ImportDef& import : file.imports) {
    if (!import.wholeModule || !seen.insert(import.alias).second) continue;
  }
  (void)compilation;
  return result;
}

std::unique_ptr<Compilation> analyzeDocument(const std::string& path, const std::string& text,
                                             const std::vector<std::string>& modulePaths,
                                             const std::string& stdlibRoot) {
  auto compilation = std::make_unique<Compilation>();
  compilation->path = path;
  compilation->text = text;

  ModuleLoaderOptions options;
  options.modulePaths = modulePaths;
  options.stdlibRoot = stdlibRoot.empty() ? defaultStdlibRoot() : stdlibRoot;

  ModuleLoader loader(std::move(options), compilation->sink);
  compilation->graph = loader.load(path, text);

  const std::string canonicalEntry = canonicalPath(path);
  compilation->indexes.resize(compilation->graph.modules.size());
  compilation->tokensByModule.resize(compilation->graph.modules.size());
  compilation->commentsByModule.resize(compilation->graph.modules.size());
  compilation->textsByModule.resize(compilation->graph.modules.size());

  for (std::size_t i = 0; i < compilation->graph.modules.size(); ++i) {
    Module& module = compilation->graph.modules[i];
    std::string source =
        canonicalPath(module.path) == canonicalEntry ? text : readFileText(module.path);
    compilation->textsByModule[i] = source;

    DiagnosticSink scratch;
    Lexer lexer(source, scratch, static_cast<int>(i));
    compilation->tokensByModule[i] = lexer.tokenize();
    compilation->commentsByModule[i] = lexer.comments();

    FileIndex& index = compilation->indexes[i];
    index.path = module.path;
    IndexBuilder builder(index, module.id);
    if (module.program) builder.build(*module.program);
  }

  if (!compilation->sink.hasErrors()) {
    Sema sema(compilation->graph, compilation->sink);
    compilation->sema = sema.analyze();
    compilation->analyzed = !compilation->sink.hasErrors();
  }

  return compilation;
}

}  // namespace a::lsp
