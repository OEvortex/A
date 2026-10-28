#include "a/sema/sema.hpp"

#include <algorithm>
#include <utility>

namespace a {
namespace {

bool isConversionBuiltin(const std::string& name) {
  static const char* names[] = {"i8", "i16",  "i32", "i64", "u8",
                                "u16", "u32", "u64", "f32", "f64"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isMathBuiltin(const std::string& name) {
  static const char* names[] = {"sqrt", "sin", "cos", "tan", "exp", "log", "floor", "ceil"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isKnownBuiltin(const std::string& name) {
  if (name == "print" || name == "eprint" || name == "input" || name == "len" ||
      name == "str" || name == "abs" || name == "min" || name == "max" ||
      name == "pow") {
    return true;
  }
  return isConversionBuiltin(name) || isMathBuiltin(name);
}

}  // namespace

Sema::Sema(Program& program, DiagnosticSink& sink) : program_(program), sink_(sink) {}

// ------------------------------------------------------------------ types

TypePtr Sema::freshVar(Constraint constraint, Span span, std::string description,
                       bool allowDefault) {
  auto type = makeType(TypeKind::Var);
  type->varId = nextVarId_++;
  type->constraint = constraint;
  type->allowDefault = allowDefault;
  type->varDescription = std::move(description);
  type->varSpan = span;
  typeVariables_.push_back(VarInfo{type, span, type->varDescription});
  return type;
}

bool Sema::occursIn(TypePtr var, TypePtr type) {
  type = resolveType(type);
  if (!type) return false;
  if (type == var) return true;
  switch (type->kind) {
    case TypeKind::Array:
    case TypeKind::Range:
      return occursIn(var, type->elem);
    case TypeKind::Function: {
      for (const auto& p : type->params) {
        if (occursIn(var, p)) return true;
      }
      return occursIn(var, type->ret);
    }
    default:
      return false;
  }
}

bool Sema::bindVar(TypePtr var, TypePtr type) {
  TypePtr resolved = resolveType(type);
  if (!resolved) return true;
  if (resolved->kind == TypeKind::Var) return unify(var, resolved);
  if (resolved->kind == TypeKind::Error) return true;
  if (resolved->kind == TypeKind::Void) return false;

  switch (var->constraint) {
    case Constraint::Integer:
      if (!isIntegerType(resolved)) return false;
      break;
    case Constraint::Floating:
      if (!isFloatType(resolved)) return false;
      break;
    case Constraint::Numeric:
      if (!isNumericType(resolved)) return false;
      break;
    case Constraint::None:
      break;
  }

  if (occursIn(var, resolved)) return false;
  var->link = resolved;
  return true;
}

bool Sema::unify(TypePtr a, TypePtr b) {
  a = resolveType(a);
  b = resolveType(b);
  if (!a || !b) return true;
  if (a->kind == TypeKind::Error || b->kind == TypeKind::Error) return true;
  if (a == b) return true;

  if (a->kind == TypeKind::Var && b->kind == TypeKind::Var) {
    if (a->varId == b->varId) return true;
    Constraint merged = a->constraint;
    bool conflict = false;
    auto merge = [&](Constraint x, Constraint y) {
      if (x == Constraint::None) return y;
      if (y == Constraint::None) return x;
      if (x == y) return x;
      if (x == Constraint::Numeric) return y;
      if (y == Constraint::Numeric) return x;
      conflict = true;
      return x;
    };
    merged = merge(a->constraint, b->constraint);
    if (conflict) return false;
    b->constraint = merged;
    b->allowDefault = a->allowDefault || b->allowDefault;
    a->link = b;
    return true;
  }

  if (a->kind == TypeKind::Var) return bindVar(a, b);
  if (b->kind == TypeKind::Var) return bindVar(b, a);
  if (a->kind != b->kind) return false;

  switch (a->kind) {
    case TypeKind::Int:
      return a->bits == b->bits && a->isSigned == b->isSigned;
    case TypeKind::Float:
      return a->bits == b->bits;
    case TypeKind::Struct:
      return a->structName == b->structName;
    case TypeKind::Array:
    case TypeKind::Range:
      return unify(a->elem, b->elem);
    case TypeKind::Function: {
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return unify(a->ret, b->ret);
    }
    default:
      return true;
  }
}

bool Sema::constrainVar(TypePtr var, Constraint constraint) {
  TypePtr resolved = resolveType(var);
  if (!resolved) return false;
  if (resolved->kind == TypeKind::Var) {
    if (resolved->constraint == Constraint::None) {
      resolved->constraint = constraint;
      return true;
    }
    if (resolved->constraint == constraint) return true;
    if (resolved->constraint == Constraint::Numeric) {
      resolved->constraint = constraint;
      return true;
    }
    if (constraint == Constraint::Numeric) return true;
    return false;
  }
  switch (constraint) {
    case Constraint::Integer: return isIntegerType(resolved);
    case Constraint::Floating: return isFloatType(resolved);
    case Constraint::Numeric: return isNumericType(resolved);
    case Constraint::None: return true;
  }
  return false;
}

TypePtr Sema::resolveTypeExpr(TypeExpr& expr) {
  if (auto* array = dynamic_cast<ArrayTypeExpr*>(&expr)) {
    return typeArray(resolveTypeExpr(*array->element));
  }
  auto* named = dynamic_cast<NamedTypeExpr*>(&expr);
  if (!named) return typeError();
  if (TypePtr builtin = builtinTypeByName(named->name)) return builtin;
  if (structs_.count(named->name)) return typeStruct(named->name);
  sink_.error(named->span, "Unknown type '" + named->name + "'.",
              "Type names are 'bool', 'char', 'String', 'void', the sized numeric types, "
              "struct names, or '[T]' for arrays.");
  return typeError();
}

// ------------------------------------------------------------------ scopes

void Sema::pushScope() { scopes_.push_back({}); }

void Sema::popScope() {
  if (scopes_.size() > 1) scopes_.pop_back();
}

Sema::VarSym* Sema::lookupVar(const std::string& name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->find(name);
    if (found != it->end()) return &found->second;
  }
  return nullptr;
}

void Sema::declareVar(const std::string& name, Span span, TypePtr type, bool mutableBinding,
                      const char* what) {
  if (scopes_.empty()) return;
  auto& scope = scopes_.back();
  if (scope.count(name)) {
    sink_.error(span, std::string("Duplicate declaration of ") + what + " '" + name + "'.",
                "A name can only be declared once in the same scope.");
    return;
  }
  if (scopes_.size() == 1) {
    if (functionsByName_.count(name)) {
      sink_.error(span, "Name '" + name + "' is already declared as a function.");
      return;
    }
    if (structsByName_.count(name)) {
      sink_.error(span, "Name '" + name + "' is already declared as a struct.");
      return;
    }
  }
  scope[name] = VarSym{std::move(type), mutableBinding};
  DeclaredVarInfo info;
  info.name = name;
  info.span = span;
  info.type = scope[name].type;
  declaredVariables_.push_back(std::move(info));
}

// ------------------------------------------------------------ declarations

void Sema::registerStructLayout(StructDecl& decl) {
  StructSym& sym = structs_[decl.name];
  sym.name = decl.name;
  sym.decl = &decl;

  for (FieldDecl& field : decl.fields) {
    for (const auto& existing : sym.fields) {
      if (existing.first == field.name) {
        sink_.error(field.span, "Duplicate field '" + field.name + "' in struct '" + decl.name + "'.");
      }
    }
    TypePtr type = resolveTypeExpr(*field.type);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(field.span, "A struct field cannot have type void.");
      type = typeError();
    }
    sym.fields.emplace_back(field.name, type);
  }
}

void Sema::registerFunctionSignature(FnDecl& decl) {
  FnSig sig;
  makeSignatureTypes(decl, nullptr, sig);
  signatures_[&decl] = std::move(sig);
}

void Sema::registerMethodSignatures(StructDecl& decl) {
  StructSym& sym = structs_[decl.name];
  TypePtr selfType = typeStruct(decl.name);

  for (auto& methodPtr : decl.methods) {
    FnDecl& method = *methodPtr;
    if (method.params.empty() || method.params.front().name != "self") {
      sink_.error(method.nameSpan,
                  "Method '" + method.name + "' must declare 'self' as its first parameter.",
                  "Write: fn " + method.name + "(self, ...):");
      continue;
    }
    if (method.params.front().type) {
      sink_.error(method.params.front().span,
                  "The 'self' parameter does not take a type annotation.");
    }
    if (sym.methods.count(method.name)) {
      sink_.error(method.nameSpan, "Duplicate method '" + method.name + "' in struct '" + decl.name + "'.");
      continue;
    }
    for (const auto& field : sym.fields) {
      if (field.first == method.name) {
        sink_.error(method.nameSpan, "Method '" + method.name + "' conflicts with a field of the same name.");
      }
    }
    FnSig sig;
    makeSignatureTypes(method, selfType, sig);
    signatures_[&method] = sig;
    sym.methods[method.name] = std::move(sig);
  }
}

TypePtr Sema::makeSignatureTypes(FnDecl& decl, TypePtr selfType, FnSig& sig) {
  sig.name = decl.name;
  sig.decl = &decl;

  std::size_t index = 0;
  if (selfType) {
    sig.paramTypes.push_back(selfType);
    index = 1;
  }

  for (std::size_t i = index; i < decl.params.size(); ++i) {
    ParamDecl& param = decl.params[i];
    TypePtr type;
    if (param.type) {
      type = resolveTypeExpr(*param.type);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(param.span, "Parameter '" + param.name + "' cannot have type void.");
        type = typeError();
      }
    } else {
      type = freshVar(Constraint::None, param.span,
                      "the type of parameter '" + param.name + "' of '" + decl.name + "'");
    }
    sig.paramTypes.push_back(type);
  }

  if (decl.returnType) {
    sig.retType = resolveTypeExpr(*decl.returnType);
  } else {
    sig.retType = freshVar(Constraint::None, decl.nameSpan,
                           "the return type of '" + decl.name + "'");
  }
  return sig.retType;
}

void Sema::checkFunctionBody(FnDecl& decl, const FnSig& sig) {
  auto savedScopes = std::move(scopes_);
  scopes_.clear();
  if (!savedScopes.empty()) {
    scopes_.push_back(savedScopes.front());
  } else {
    scopes_.push_back({});
  }

  FnDecl* savedFunction = currentFunction_;
  TypePtr savedReturn = currentReturnType_;
  bool savedSawReturn = sawReturn_;
  int savedLoopDepth = loopDepth_;
  int savedDeferDepth = deferDepth_;

  currentFunction_ = &decl;
  currentReturnType_ = sig.retType;
  sawReturn_ = false;
  loopDepth_ = 0;
  deferDepth_ = 0;

  pushScope();
  for (std::size_t i = 0; i < decl.params.size() && i < sig.paramTypes.size(); ++i) {
    declareVar(decl.params[i].name, decl.params[i].span, sig.paramTypes[i], false, "parameter");
  }
  checkStatements(decl.body);

  if (!sawReturn_) {
    TypePtr ret = resolveType(sig.retType);
    if (decl.returnType) {
      if (ret->kind != TypeKind::Void) {
        sink_.error(decl.nameSpan,
                    "Function '" + decl.name + "' declares a return type of '" + typeName(ret) +
                        "' but never returns a value.",
                    "Return a value, or declare the return type as void.");
      }
    } else {
      if (!unify(sig.retType, typeVoid())) {
        sink_.error(decl.nameSpan,
                    "Function '" + decl.name + "' is used as returning '" + typeName(ret) +
                        "' but never returns a value.");
      }
    }
  }

  checkedFunctions_.push_back({&decl, sig.retType});

  currentFunction_ = savedFunction;
  currentReturnType_ = savedReturn;
  sawReturn_ = savedSawReturn;
  loopDepth_ = savedLoopDepth;
  deferDepth_ = savedDeferDepth;

  scopes_ = std::move(savedScopes);
}

void Sema::checkStructMethodBodies(StructDecl& decl) {
  for (auto& methodPtr : decl.methods) {
    auto found = signatures_.find(methodPtr.get());
    if (found == signatures_.end()) continue;
    checkFunctionBody(*methodPtr, found->second);
  }
}

// ---------------------------------------------------------------- analyze

SemaResult Sema::analyze() {
  scopes_.push_back({});

  // Pass 1: collect declaration names so duplicates and collisions are clear.
  for (auto& stmt : program_.statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      if (structsByName_.count(decl->name) || functionsByName_.count(decl->name)) {
        sink_.error(decl->nameSpan, "Duplicate declaration of '" + decl->name + "'.");
        continue;
      }
      StructSym sym;
      sym.name = decl->name;
      sym.decl = decl;
      structs_[decl->name] = std::move(sym);
      structsByName_[decl->name] = decl;
    } else if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
      if (functionsByName_.count(fn->name) || structsByName_.count(fn->name)) {
        sink_.error(fn->nameSpan, "Duplicate declaration of '" + fn->name + "'.");
        continue;
      }
      functionsByName_[fn->name] = fn;
    }
  }

  // Pass 2: resolve struct layouts and method signatures.
  for (auto& stmt : program_.statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(decl->name);
      if (found == structsByName_.end() || found->second != decl) continue;
      registerStructLayout(*decl);
      registerMethodSignatures(*decl);
    }
  }

  // Pass 3: function signatures.
  for (auto& stmt : program_.statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(decl->name);
      if (found == functionsByName_.end() || found->second != decl) continue;
      registerFunctionSignature(*decl);
    }
  }

  // Pass 4: top-level statements (global variables and executable code).
  for (auto& stmt : program_.statements) {
    if (dynamic_cast<FnDecl*>(stmt.get()) || dynamic_cast<StructDecl*>(stmt.get())) continue;
    checkStatement(*stmt);
  }

  // Pass 5: function and method bodies.
  for (auto& stmt : program_.statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(decl->name);
      if (found == structsByName_.end() || found->second != decl) continue;
      checkStructMethodBodies(*decl);
    }
  }
  for (auto& stmt : program_.statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(decl->name);
      if (found == functionsByName_.end() || found->second != decl) continue;
      auto sig = signatures_.find(decl);
      if (sig == signatures_.end()) continue;
      checkFunctionBody(*decl, sig->second);
    }
  }

  checkTypeVariables();
  checkDeclaredVariables();
  checkReturnPaths();

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  return result;
}

// ------------------------------------------------------------- statement checks

void Sema::checkStatements(std::vector<StmtPtr>& statements) {
  for (auto& stmt : statements) checkStatement(*stmt);
}

void Sema::checkStatement(Stmt& stmt) {
  if (auto* node = dynamic_cast<LetStmt*>(&stmt)) {
    checkLet(*node);
    return;
  }
  if (auto* node = dynamic_cast<AssignStmt*>(&stmt)) {
    checkAssign(*node);
    return;
  }
  if (auto* node = dynamic_cast<ExprStmt*>(&stmt)) {
    inferExpr(*node->expr);
    return;
  }
  if (auto* node = dynamic_cast<IfStmt*>(&stmt)) {
    checkIf(*node);
    return;
  }
  if (auto* node = dynamic_cast<WhileStmt*>(&stmt)) {
    checkWhile(*node);
    return;
  }
  if (auto* node = dynamic_cast<ForStmt*>(&stmt)) {
    checkFor(*node);
    return;
  }
  if (auto* node = dynamic_cast<LoopStmt*>(&stmt)) {
    checkLoop(*node);
    return;
  }
  if (auto* node = dynamic_cast<ReturnStmt*>(&stmt)) {
    checkReturn(*node);
    return;
  }
  if (auto* node = dynamic_cast<DeferStmt*>(&stmt)) {
    checkDefer(*node);
    return;
  }
  if (auto* node = dynamic_cast<BreakStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'break' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'break' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<ContinueStmt*>(&stmt)) {
    if (loopDepth_ == 0) {
      sink_.error(node->span, "'continue' used outside of a loop.");
    }
    if (deferDepth_ > 0) {
      sink_.error(node->span, "'continue' cannot be used inside a defer statement.");
    }
    return;
  }
  if (auto* node = dynamic_cast<FnDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Functions can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<StructDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Structs can only be declared at the top level.");
    return;
  }
}

void Sema::checkLet(LetStmt& stmt) {
  TypePtr initType = inferExpr(*stmt.init);

  TypePtr variableType = initType;
  if (stmt.type) {
    TypePtr declared = resolveTypeExpr(*stmt.type);
    if (resolveType(declared)->kind == TypeKind::Void) {
      sink_.error(stmt.span, "A variable cannot have type void.");
      declared = typeError();
    }
    if (!unify(initType, declared)) {
      reportMismatch(initType, declared, stmt.init->span,
                     "in the initializer for '" + stmt.name + "'");
    }
    variableType = declared;
  } else if (resolveType(initType)->kind == TypeKind::Void) {
    sink_.error(stmt.init->span,
                "Cannot initialize '" + stmt.name + "' with a void value.");
    variableType = typeError();
  }

  declareVar(stmt.name, stmt.nameSpan, variableType, stmt.mutableBinding);
}

void Sema::checkAssign(AssignStmt& stmt) {
  if (auto* ident = dynamic_cast<IdentExpr*>(stmt.target.get())) {
    VarSym* sym = lookupVar(ident->name);
    if (!sym) {
      if (stmt.op != "=") {
        sink_.error(stmt.target->span,
                    "Undefined variable '" + ident->name + "' in compound assignment.");
        return;
      }
      TypePtr valueType = inferExpr(*stmt.value);
      if (resolveType(valueType)->kind == TypeKind::Void) {
        sink_.error(stmt.value->span,
                    "Cannot initialize '" + ident->name + "' with a void value.");
        valueType = typeError();
      }
      declareVar(ident->name, ident->span, valueType, false);
      return;
    }

    if (!sym->mutableBinding) {
      sink_.error(ident->span, "Cannot assign to immutable binding '" + ident->name + "'.",
                  "Declare the variable with 'var " + ident->name +
                      " = ...' to make it mutable.");
    }

    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, sym->type, stmt.opSpan)) return;
    }
    if (!unify(valueType, sym->type)) {
      reportMismatch(valueType, sym->type, stmt.value->span,
                     "in assignment to '" + ident->name + "'");
    }
    return;
  }

  if (dynamic_cast<FieldExpr*>(stmt.target.get()) ||
      dynamic_cast<IndexExpr*>(stmt.target.get())) {
    TypePtr targetType = inferExpr(*stmt.target);
    TypePtr valueType = inferExpr(*stmt.value);
    if (stmt.op != "=") {
      std::string baseOp = stmt.op.substr(0, stmt.op.size() - 1);
      if (!checkCompoundOp(baseOp, targetType, stmt.opSpan)) return;
    }
    if (!unify(valueType, targetType)) {
      reportMismatch(valueType, targetType, stmt.value->span, "in assignment");
    }
    return;
  }

  sink_.error(stmt.target->span,
              "Invalid assignment target.",
              "Only variables, struct fields, and array elements can be assigned to.");
}

void Sema::checkIf(IfStmt& stmt) {
  for (auto& branch : stmt.branches) {
    TypePtr condType = inferExpr(*branch.cond);
    requireBool(condType, branch.cond->span, "The 'if' condition");
    pushScope();
    checkStatements(branch.body);
    popScope();
  }
  if (!stmt.elseBody.empty()) {
    pushScope();
    checkStatements(stmt.elseBody);
    popScope();
  }
}

void Sema::checkWhile(WhileStmt& stmt) {
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkFor(ForStmt& stmt) {
  TypePtr iterableType = inferExpr(*stmt.iterable);
  TypePtr resolved = resolveType(iterableType);
  TypePtr elementType;

  if (resolved->kind == TypeKind::Var) {
    sink_.error(stmt.iterable->span,
                "Cannot infer the type of the value being iterated over.",
                "Annotate the variable, or iterate over an array, string, or range.");
    elementType = typeError();
  } else if (resolved->kind == TypeKind::Array || resolved->kind == TypeKind::Range) {
    elementType = resolved->elem;
  } else if (resolved->kind == TypeKind::String) {
    elementType = typeChar();
  } else {
    sink_.error(stmt.iterable->span,
                "Cannot iterate over " + typePhrase(resolved) + ".",
                "Loops iterate over arrays, strings, and integer ranges ('0..10').");
    elementType = typeError();
  }

  ++loopDepth_;
  pushScope();
  declareVar(stmt.var, stmt.varSpan, elementType, false, "loop variable");
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkLoop(LoopStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
}

void Sema::checkReturn(ReturnStmt& stmt) {
  if (!currentFunction_) {
    sink_.error(stmt.span, "'return' used outside of a function.");
    return;
  }
  if (deferDepth_ > 0) {
    sink_.error(stmt.span, "'return' cannot be used inside a defer statement.");
  }
  sawReturn_ = true;

  TypePtr expected = currentReturnType_;
  TypePtr resolvedExpected = resolveType(expected);

  if (stmt.value) {
    TypePtr valueType = inferExpr(*stmt.value);
    if (resolvedExpected->kind == TypeKind::Void) {
      sink_.error(stmt.value->span,
                  "Function '" + currentFunction_->name +
                      "' has return type void and cannot return a value.");
      return;
    }
    if (!unify(valueType, expected)) {
      reportMismatch(valueType, expected, stmt.value->span,
                     "in the return value of '" + currentFunction_->name + "'");
    }
  } else {
    if (resolvedExpected->kind != TypeKind::Var &&
        resolvedExpected->kind != TypeKind::Void) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name + "' must return a value of type '" +
                      typeName(resolvedExpected) + "'.");
      return;
    }
    if (!unify(expected, typeVoid())) {
      sink_.error(stmt.span,
                  "Function '" + currentFunction_->name +
                      "' is expected to return a value, but this 'return' has none.");
    }
  }
}

void Sema::checkDefer(DeferStmt& stmt) {
  ++deferDepth_;
  pushScope();
  checkStatement(*stmt.body);
  popScope();
  --deferDepth_;
}

// ------------------------------------------------------------- expressions

TypePtr Sema::inferExpr(Expr& expr) {
  if (auto* node = dynamic_cast<IntLitExpr*>(&expr)) {
    (void)node;
    return freshVar(Constraint::Integer, expr.span, "an integer literal", /*allowDefault=*/true);
  }
  if (auto* node = dynamic_cast<FloatLitExpr*>(&expr)) {
    (void)node;
    return freshVar(Constraint::Floating, expr.span, "a float literal", /*allowDefault=*/true);
  }
  if (dynamic_cast<BoolLitExpr*>(&expr)) return typeBool();
  if (dynamic_cast<CharLitExpr*>(&expr)) return typeChar();
  if (auto* node = dynamic_cast<StringLitExpr*>(&expr)) return inferStringLiteral(*node);
  if (auto* node = dynamic_cast<IdentExpr*>(&expr)) return inferIdent(*node);
  if (auto* node = dynamic_cast<UnaryExpr*>(&expr)) return inferUnary(*node);
  if (auto* node = dynamic_cast<BinaryExpr*>(&expr)) return inferBinary(*node);
  if (auto* node = dynamic_cast<CallExpr*>(&expr)) return inferCall(*node);
  if (auto* node = dynamic_cast<IndexExpr*>(&expr)) return inferIndex(*node);
  if (auto* node = dynamic_cast<SliceExpr*>(&expr)) return inferSlice(*node);
  if (auto* node = dynamic_cast<FieldExpr*>(&expr)) return inferField(*node);
  if (auto* node = dynamic_cast<ArrayLitExpr*>(&expr)) return inferArrayLiteral(*node);
  if (auto* node = dynamic_cast<RangeExpr*>(&expr)) {
    TypePtr start = inferExpr(*node->start);
    TypePtr end = inferExpr(*node->end);
    requireInteger(start, node->start->span, "The range start");
    requireInteger(end, node->end->span, "The range end");
    if (!unify(start, end)) {
      sink_.error(node->span, "Range endpoints must have the same integer type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    }
    return typeRange(start);
  }
  return typeError();
}

TypePtr Sema::inferStringLiteral(StringLitExpr& expr) {
  for (auto& part : expr.parts) {
    if (!part.isExpr || !part.expr) continue;
    TypePtr type = inferExpr(*part.expr);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(part.expr->span, "Cannot interpolate a void value into a string.");
    }
  }
  return typeString();
}

TypePtr Sema::inferIdent(IdentExpr& expr) {
  if (VarSym* sym = lookupVar(expr.name)) return sym->type;
  if (functionsByName_.count(expr.name)) {
    sink_.error(expr.span, "'" + expr.name + "' is a function, not a value.",
                "Call it: " + expr.name + "(...).");
    return typeError();
  }
  if (structsByName_.count(expr.name)) {
    sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                "Construct it: " + expr.name + "(...).");
    return typeError();
  }
  sink_.error(expr.span, "Undefined name '" + expr.name + "'.",
              "Declare it by assigning a value, e.g. " + expr.name + " = 0.");
  return typeError();
}

TypePtr Sema::inferUnary(UnaryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  if (expr.op == "not") {
    if (!unify(operand, typeBool())) {
      sink_.error(expr.opSpan,
                  "'not' requires a bool operand, found " + typePhrase(operand) + ".");
      return typeError();
    }
    return typeBool();
  }

  TypePtr resolved = resolveType(operand);
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
    return operand;
  }
  if (resolved->kind == TypeKind::Var) {
    if (constrainVar(operand, Constraint::Numeric)) return operand;
  }
  sink_.error(expr.opSpan,
              "Operator '-' cannot be applied to " + typePhrase(operand) + ".");
  return typeError();
}

TypePtr Sema::numericBinaryResult(const std::string& op, TypePtr lhs, TypePtr rhs, Span span) {
  if (!unify(lhs, rhs)) {
    sink_.error(span, "Operator '" + op + "' requires both operands to have the same type, "
                      "found " + typePhrase(lhs) + " and " + typePhrase(rhs) + ".");
    return typeError();
  }
  TypePtr type = resolveType(lhs);
  if (type->kind == TypeKind::Int) return type;
  if (type->kind == TypeKind::Float) {
    if (op == "%") {
      sink_.error(span, "Operator '%' requires integer operands, found " + typePhrase(type) + ".");
      return typeError();
    }
    return type;
  }
  if (type->kind == TypeKind::Var) {
    Constraint constraint = (op == "%") ? Constraint::Integer : Constraint::Numeric;
    if (constrainVar(type, constraint)) return type;
  }
  sink_.error(span,
              "Operator '" + op + "' cannot be applied to " + typePhrase(type) + ".");
  return typeError();
}

TypePtr Sema::inferBinary(BinaryExpr& expr) {
  const std::string& op = expr.op;

  TypePtr lhs = inferExpr(*expr.lhs);
  TypePtr rhs = inferExpr(*expr.rhs);

  if (op == "and" || op == "or") {
    requireBool(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireBool(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    return typeBool();
  }

  if (op == "..") {
    requireInteger(lhs, expr.lhs->span, "The range start");
    requireInteger(rhs, expr.rhs->span, "The range end");
    if (!unify(lhs, rhs)) {
      sink_.error(expr.opSpan, "Range endpoints must have the same integer type, found " +
                                   typeName(lhs) + " and " + typeName(rhs) + ".");
      return typeError();
    }
    return typeRange(lhs);
  }

  const bool comparison = op == "==" || op == "!=" || op == "<" || op == ">" ||
                          op == "<=" || op == ">=";

  if (comparison) {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    if (!unify(lhs, rhs)) {
      sink_.error(expr.opSpan,
                  "Cannot compare " + typePhrase(lhsResolved) + " and " +
                      typePhrase(rhsResolved) + ".");
      return typeError();
    }
    TypePtr type = resolveType(lhs);
    switch (type->kind) {
      case TypeKind::Int:
      case TypeKind::Float:
      case TypeKind::String:
      case TypeKind::Char:
      case TypeKind::Var:
        return typeBool();
      case TypeKind::Bool:
        if (op == "==" || op == "!=") return typeBool();
        sink_.error(expr.opSpan, "Only '==' and '!=' can compare bool values.");
        return typeError();
      default:
        sink_.error(expr.opSpan,
                    "Values of type '" + typeName(type) + "' cannot be compared with '" + op +
                        "'.");
        return typeError();
    }
  }

  if (op == "+") {
    TypePtr lhsResolved = resolveType(lhs);
    TypePtr rhsResolved = resolveType(rhs);
    bool lhsString = lhsResolved->kind == TypeKind::String;
    bool rhsString = rhsResolved->kind == TypeKind::String;
    if (lhsString || rhsString) {
      if (lhsString && rhsString) return typeString();
      TypePtr other = lhsString ? rhs : lhs;
      if (resolveType(other)->kind == TypeKind::Var && unify(other, typeString())) {
        return typeString();
      }
      sink_.error(expr.opSpan,
                  "Cannot add a String and " + typePhrase(other) + ".",
                  "Use string interpolation, e.g. \"Price: {100}\".");
      return typeError();
    }
  }

  return numericBinaryResult(op, lhs, rhs, expr.opSpan);
}

TypePtr Sema::inferCall(CallExpr& expr) {
  if (auto* ident = dynamic_cast<IdentExpr*>(expr.callee.get())) {
    const std::string& name = ident->name;
    if (lookupVar(name)) {
      sink_.error(ident->span, "Variable '" + name + "' is not callable.",
                  "Functions are called by name; values cannot be called in this version.");
      return typeError();
    }
    auto structIt = structs_.find(name);
    if (structIt != structs_.end()) {
      return checkStructConstruction(structIt->second, expr);
    }
    auto fnIt = functionsByName_.find(name);
    if (fnIt != functionsByName_.end()) {
      auto sigIt = signatures_.find(fnIt->second);
      if (sigIt == signatures_.end()) return typeError();
      return checkFunctionCall(sigIt->second, expr);
    }
    if (isKnownBuiltin(name)) return checkBuiltinCall(name, expr);
    sink_.error(ident->span, "Undefined function '" + name + "'.",
                "Functions must be declared before they are called.");
    return typeError();
  }

  if (auto* field = dynamic_cast<FieldExpr*>(expr.callee.get())) {
    TypePtr objectType = inferExpr(*field->object);
    return checkMethodCall(objectType, *field, expr);
  }

  sink_.error(expr.callee->span, "This expression is not callable.",
              "Only named functions, builtins, methods, and struct constructors can be called.");
  return typeError();
}

TypePtr Sema::checkFunctionCall(const FnSig& sig, CallExpr& call) {
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are only supported for struct construction.");
      return typeError();
    }
  }
  if (call.args.size() != sig.paramTypes.size()) {
    sink_.error(call.span,
                "Function '" + sig.name + "' expects " + std::to_string(sig.paramTypes.size()) +
                    " argument(s), found " + std::to_string(call.args.size()) + ".");
    return typeError();
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    TypePtr argType = inferExpr(*call.args[i].value);
    if (!unify(argType, sig.paramTypes[i])) {
      reportMismatch(argType, sig.paramTypes[i], call.args[i].value->span,
                     "in argument " + std::to_string(i + 1) + " to '" + sig.name + "'");
    }
  }
  return sig.retType;
}

TypePtr Sema::checkStructConstruction(const StructSym& sym, CallExpr& call) {
  const auto& fields = sym.fields;

  bool anyNamed = false;
  bool allNamed = true;
  for (auto& arg : call.args) {
    if (!arg.name.empty()) anyNamed = true;
    else allNamed = false;
  }

  if (anyNamed && !allNamed) {
    sink_.error(call.span, "Cannot mix named and positional arguments in a struct constructor.");
    return typeStruct(sym.name);
  }

  if (!anyNamed) {
    if (call.args.size() != fields.size()) {
      sink_.error(call.span,
                  "Struct '" + sym.name + "' expects " + std::to_string(fields.size()) +
                      " field(s), found " + std::to_string(call.args.size()) + ".");
      return typeStruct(sym.name);
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      TypePtr argType = inferExpr(*call.args[i].value);
      if (!unify(argType, fields[i].second)) {
        reportMismatch(argType, fields[i].second, call.args[i].value->span,
                       "in field '" + fields[i].first + "' of '" + sym.name + "'");
      }
    }
    return typeStruct(sym.name);
  }

  std::vector<bool> provided(fields.size(), false);
  for (auto& arg : call.args) {
    std::size_t index = fields.size();
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].first == arg.name) {
        index = i;
        break;
      }
    }
    if (index == fields.size()) {
      sink_.error(arg.nameSpan, "'" + sym.name + "' has no field named '" + arg.name + "'.");
      inferExpr(*arg.value);
      continue;
    }
    if (provided[index]) {
      sink_.error(arg.nameSpan, "Field '" + arg.name + "' is provided more than once.");
      inferExpr(*arg.value);
      continue;
    }
    provided[index] = true;
    TypePtr argType = inferExpr(*arg.value);
    if (!unify(argType, fields[index].second)) {
      reportMismatch(argType, fields[index].second, arg.value->span,
                     "in field '" + arg.name + "' of '" + sym.name + "'");
    }
  }
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (!provided[i]) {
      sink_.error(call.span, "Missing field '" + fields[i].first + "' in '" + sym.name +
                                 "' constructor.");
    }
  }
  return typeStruct(sym.name);
}

TypePtr Sema::checkMethodCall(const TypePtr& objectType, FieldExpr& field, CallExpr& call) {
  TypePtr object = resolveType(objectType);
  const std::string& method = field.field;

  auto rejectNamed = [&]() {
    for (auto& arg : call.args) {
      if (!arg.name.empty()) {
        sink_.error(arg.nameSpan, "Named arguments are not supported in method calls.");
        return true;
      }
    }
    return false;
  };

  if (object->kind == TypeKind::Var) {
    sink_.error(field.fieldSpan,
                "Cannot infer the receiver type for method '" + method + "'.",
                "Add a type annotation to the receiver.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    auto methodIt = structIt->second.methods.find(method);
    if (methodIt == structIt->second.methods.end()) {
      sink_.error(field.fieldSpan,
                  "Struct '" + object->structName + "' has no method named '" + method + "'.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    if (rejectNamed()) return typeError();
    const FnSig& sig = methodIt->second;
    std::size_t expected = sig.paramTypes.size() - 1;  // exclude self
    if (call.args.size() != expected) {
      sink_.error(call.span,
                  "Method '" + method + "' expects " + std::to_string(expected) +
                      " argument(s), found " + std::to_string(call.args.size()) + ".");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      TypePtr argType = inferExpr(*call.args[i].value);
      if (!unify(argType, sig.paramTypes[i + 1])) {
        reportMismatch(argType, sig.paramTypes[i + 1], call.args[i].value->span,
                       "in argument " + std::to_string(i + 1) + " to method '" + method + "'");
      }
    }
    return sig.retType;
  }

  if (object->kind == TypeKind::Array) {
    if (method == "push") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'push' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, object->elem)) {
        reportMismatch(argType, object->elem, call.args[0].value->span, "in 'push'");
      }
      return typeVoid();
    }
    if (method == "pop") {
      if (rejectNamed()) return typeError();
      if (!call.args.empty()) {
        sink_.error(call.span, "Method 'pop' takes no arguments.");
        for (auto& arg : call.args) inferExpr(*arg.value);
      }
      return object->elem;
    }
    sink_.error(field.fieldSpan,
                "Array has no method named '" + method + "'.",
                "Available array methods: push(value), pop().");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  if (object->kind == TypeKind::String) {
    if (rejectNamed()) return typeError();
    auto expectNoArgs = [&]() -> bool {
      if (!call.args.empty()) {
        sink_.error(call.span, "Method '" + method + "' takes no arguments.");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return false;
      }
      return true;
    };
    if (method == "to_int") {
      if (!expectNoArgs()) return typeError();
      return typeInt(64, true);
    }
    if (method == "to_float") {
      if (!expectNoArgs()) return typeError();
      return typeFloat(64);
    }
    if (method == "upper" || method == "lower" || method == "trim") {
      if (!expectNoArgs()) return typeError();
      return typeString();
    }
    if (method == "contains" || method == "split") {
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method '" + method + "' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, typeString())) {
        reportMismatch(argType, typeString(), call.args[0].value->span,
                       "in argument 1 to '" + method + "'");
      }
      return method == "contains" ? typeBool() : typeArray(typeString());
    }
    sink_.error(field.fieldSpan,
                "String has no method named '" + method + "'.",
                "Available string methods: to_int(), to_float(), upper(), lower(), trim(), "
                "contains(sub), split(sep).");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  sink_.error(field.fieldSpan,
              "Values of type '" + typeName(object) + "' have no methods.",
              "Methods are defined on structs and on arrays and strings.");
  for (auto& arg : call.args) inferExpr(*arg.value);
  return typeError();
}

TypePtr Sema::checkBuiltinCall(const std::string& name, CallExpr& call) {
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Builtin functions do not accept named arguments.");
      return typeError();
    }
  }

  auto inferArgs = [&]() {
    std::vector<TypePtr> types;
    types.reserve(call.args.size());
    for (auto& arg : call.args) types.push_back(inferExpr(*arg.value));
    return types;
  };

  auto arityError = [&](std::size_t expected) {
    sink_.error(call.span, "'" + name + "' expects " + std::to_string(expected) +
                               " argument(s), found " + std::to_string(call.args.size()) + ".");
  };

  if (name == "print" || name == "eprint") {
    for (auto& arg : call.args) {
      TypePtr type = inferExpr(*arg.value);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(arg.value->span, "Cannot print a void value.");
      }
    }
    return typeVoid();
  }

  if (name == "input") {
    if (call.args.size() > 1) {
      arityError(0);
      inferArgs();
      return typeString();
    }
    if (call.args.size() == 1) {
      TypePtr type = inferExpr(*call.args[0].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[0].value->span, "in the input prompt");
      }
    }
    return typeString();
  }

  if (name == "len") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeInt(64, true);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    TypePtr resolved = resolveType(type);
    if (resolved->kind == TypeKind::String || resolved->kind == TypeKind::Array) {
      return typeInt(64, true);
    }
    if (resolved->kind == TypeKind::Var) {
      sink_.error(call.args[0].value->span, "Cannot infer the argument type for 'len'.",
                  "Add a type annotation to the argument.");
    } else {
      sink_.error(call.args[0].value->span,
                  "'len' cannot be applied to " + typePhrase(resolved) + ".",
                  "'len' works on strings and arrays.");
    }
    return typeInt(64, true);
  }

  if (name == "str") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeString();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot convert a void value to a string.");
    }
    return typeString();
  }

  if (name == "abs") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    return requireNumeric(type, call.args[0].value->span, "The argument of 'abs'");
  }

  if (name == "min" || name == "max") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeError();
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    if (!unify(a, b)) {
      sink_.error(call.span, "'" + name + "' requires arguments of the same numeric type.");
      return typeError();
    }
    return requireNumeric(a, call.span, "The arguments of '" + name + "'");
  }

  if (name == "pow") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeFloat(64);
    }
    TypePtr a = inferExpr(*call.args[0].value);
    TypePtr b = inferExpr(*call.args[1].value);
    requireNumeric(a, call.args[0].value->span, "The base of 'pow'");
    requireNumeric(b, call.args[1].value->span, "The exponent of 'pow'");
    return typeFloat(64);
  }

  if (isMathBuiltin(name)) {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeFloat(64);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    return typeFloat(64);
  }

  if (isConversionBuiltin(name)) {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return builtinTypeByName(name);
    }
    TypePtr type = inferExpr(*call.args[0].value);
    requireNumeric(type, call.args[0].value->span, "The argument of '" + name + "'");
    return builtinTypeByName(name);
  }

  return typeError();
}

TypePtr Sema::inferIndex(IndexExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr indexType = inferExpr(*expr.index);
  requireInteger(indexType, expr.index->span, "The array index");

  TypePtr object = resolveType(objectType);
  if (object->kind == TypeKind::Array) return object->elem;
  if (object->kind == TypeKind::String) {
    sink_.error(expr.bracketSpan,
                "Strings cannot be indexed by number.",
                "Strings are UTF-8; iterate with 'for c in text:' to read characters.");
    return typeChar();
  }
  if (object->kind == TypeKind::Var) {
    sink_.error(expr.object->span, "Cannot infer the type being indexed.",
                "Add a type annotation to the value.");
    return typeError();
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be indexed.");
  return typeError();
}

TypePtr Sema::inferSlice(SliceExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  if (expr.start) {
    TypePtr startType = inferExpr(*expr.start);
    requireInteger(startType, expr.start->span, "The slice start");
  }
  if (expr.end) {
    TypePtr endType = inferExpr(*expr.end);
    requireInteger(endType, expr.end->span, "The slice end");
  }

  TypePtr object = resolveType(objectType);
  if (object->kind == TypeKind::Array) return object;
  if (object->kind == TypeKind::String) {
    sink_.error(expr.bracketSpan,
                "Slicing strings is not supported yet.",
                "Iterate with 'for c in text:' or use string methods.");
    return typeString();
  }
  if (object->kind == TypeKind::Var) {
    sink_.error(expr.object->span, "Cannot infer the type being sliced.",
                "Add a type annotation to the value.");
    return typeError();
  }
  sink_.error(expr.object->span,
              "Values of type '" + typeName(object) + "' cannot be sliced.");
  return typeError();
}

TypePtr Sema::inferField(FieldExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    for (const auto& field : structIt->second.fields) {
      if (field.first == expr.field) return field.second;
    }
    sink_.error(expr.fieldSpan,
                "Struct '" + object->structName + "' has no field named '" + expr.field + "'.");
    return typeError();
  }

  if (object->kind == TypeKind::Var) {
    sink_.error(expr.fieldSpan,
                "Cannot infer the type of the value before '." + expr.field + "'.",
                "Add a type annotation to the value.");
    return typeError();
  }

  if (object->kind == TypeKind::Array || object->kind == TypeKind::String) {
    std::string help = expr.field == "len" ? "Use len(value) instead." : "";
    sink_.error(expr.fieldSpan,
                "Values of type '" + typeName(object) + "' have no fields.", help);
    return typeError();
  }

  sink_.error(expr.fieldSpan,
              "Values of type '" + typeName(object) + "' have no field '" + expr.field + "'.");
  return typeError();
}

TypePtr Sema::inferArrayLiteral(ArrayLitExpr& expr) {
  if (expr.elements.empty()) {
    TypePtr element = freshVar(Constraint::None, expr.span, "the element type of an empty array");
    return typeArray(element);
  }

  TypePtr elementType = inferExpr(*expr.elements[0]);
  for (std::size_t i = 1; i < expr.elements.size(); ++i) {
    TypePtr nextType = inferExpr(*expr.elements[i]);
    if (!unify(elementType, nextType)) {
      sink_.error(expr.elements[i]->span,
                  "All array elements must have the same type, found " + typeName(elementType) +
                      " and " + typeName(nextType) + ".");
      return typeError();
    }
  }
  return typeArray(elementType);
}

// ---------------------------------------------------------------- helpers

void Sema::requireBool(TypePtr type, Span span, const std::string& what) {
  if (!unify(type, typeBool())) {
    sink_.error(span, what + " must be bool, found " + typePhrase(type) + ".");
  }
}

void Sema::requireInteger(TypePtr type, Span span, const std::string& what) {
  TypePtr resolved = resolveType(type);
  if (resolved->kind == TypeKind::Int) return;
  if (resolved->kind == TypeKind::Var && constrainVar(type, Constraint::Integer)) return;
  sink_.error(span, what + " must be an integer, found " + typePhrase(type) + ".");
}

TypePtr Sema::requireNumeric(TypePtr type, Span span, const std::string& what) {
  TypePtr resolved = resolveType(type);
  if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) return type;
  if (resolved->kind == TypeKind::Var && constrainVar(type, Constraint::Numeric)) return type;
  sink_.error(span, what + " must be numeric, found " + typePhrase(type) + ".");
  return typeError();
}

bool Sema::checkCompoundOp(const std::string& op, TypePtr targetType, Span span) {
  TypePtr type = resolveType(targetType);
  if (type->kind == TypeKind::Var) {
    if (constrainVar(targetType, Constraint::Numeric)) return true;
    sink_.error(span, "Operator '" + op + "=' is not defined for " + typePhrase(type) + ".");
    return false;
  }
  if (type->kind == TypeKind::Int) return true;
  if (type->kind == TypeKind::Float) {
    if (op == "%") {
      sink_.error(span, "Operator '%=' requires an integer target.");
      return false;
    }
    return true;
  }
  if (type->kind == TypeKind::String && op == "+") return true;
  sink_.error(span, "Operator '" + op + "=' is not defined for " + typePhrase(type) + ".");
  return false;
}

void Sema::reportMismatch(TypePtr actual, TypePtr expected, Span span,
                          const std::string& context) {
  sink_.error(span, "Type mismatch " + context + ": expected '" + typeName(expected) +
                        "', found '" + typeName(actual) + "'.");
}

std::string Sema::typePhrase(const TypePtr& type) const {
  TypePtr resolved = resolveType(type);
  if (!resolved) return "a value";
  if (resolved->kind == TypeKind::Var) {
    switch (resolved->constraint) {
      case Constraint::Integer: return "an integer";
      case Constraint::Floating: return "a float";
      case Constraint::Numeric: return "a number";
      case Constraint::None: return "a value of unknown type";
    }
  }
  std::string name = typeName(resolved);
  char first = name.empty() ? 'x' : name[0];
  bool vowelSound = first == 'a' || first == 'e' || first == 'i' || first == 'o';
  return (vowelSound ? "an " : "a ") + name;
}

void Sema::checkTypeVariables() {
  for (auto& info : typeVariables_) {
    TypePtr resolved = resolveType(info.var);
    if (!resolved || resolved->kind != TypeKind::Var) continue;
    if (resolved->allowDefault) {
      if (resolved->constraint == Constraint::Integer) {
        resolved->link = typeInt(32, true);
        continue;
      }
      if (resolved->constraint == Constraint::Floating) {
        resolved->link = typeFloat(64);
        continue;
      }
    }
    Span span = resolved->varSpan.valid() ? resolved->varSpan : info.span;
    sink_.error(span, "Cannot infer " + info.description + ".",
                "Add an explicit type annotation.");
  }
}

void Sema::checkDeclaredVariables() {
  for (auto& info : declaredVariables_) {
    TypePtr resolved = resolveType(info.type);
    if (resolved && resolved->kind == TypeKind::Void) {
      sink_.error(info.span, "Variable '" + info.name + "' cannot have type void.");
    }
  }
}

void Sema::checkReturnPaths() {
  for (auto& entry : checkedFunctions_) {
    const FnDecl* decl = entry.first;
    TypePtr ret = resolveType(entry.second);
    if (!ret || ret->kind == TypeKind::Void || ret->kind == TypeKind::Var) continue;
    if (!blockReturns(decl->body)) {
      sink_.error(decl->nameSpan,
                  "Not all paths in function '" + decl->name + "' return a value of type '" +
                      typeName(ret) + "'.",
                  "Add a 'return' at the end, or use if/else so that every branch returns.");
    }
  }
}

bool Sema::blockReturns(const std::vector<StmtPtr>& body) {
  if (body.empty()) return false;
  return statementReturns(*body.back());
}

bool Sema::statementReturns(const Stmt& stmt) {
  if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
  if (auto* node = dynamic_cast<const IfStmt*>(&stmt)) {
    if (node->elseBody.empty()) return false;
    for (const auto& branch : node->branches) {
      if (!blockReturns(branch.body)) return false;
    }
    return blockReturns(node->elseBody);
  }
  if (dynamic_cast<const LoopStmt*>(&stmt)) return true;
  return false;
}

}  // namespace a
