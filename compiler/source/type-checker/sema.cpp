#include "type-checker/sema.hpp"

#include <algorithm>
#include <utility>

#include "name-resolution/native_modules.hpp"

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
  static const char* names[] = {"sqrt", "sin",  "cos",   "tan",   "exp",  "log",
                                "floor", "ceil", "round", "trunc", "log2", "log10"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return false;
}

bool isKnownBuiltin(const std::string& name) {
  static const char* names[] = {
      "print",    "eprint",   "input",     "len",      "str",      "abs",
      "min",      "max",      "pow",       "flush",    "read_file", "write_file",
      "append_file", "file_exists", "list_dir", "remove_file", "create_dir",
      "remove_dir", "rename", "current_dir", "now", "sleep", "panic", "assert",
      "assert_eq", "map",  "filter",   "reduce",   "any",
       "all",      "sum",      "join",      "random",   "random_int", "atan2",
       "Some",     "Ok",       "Err"};
  for (const char* n : names) {
    if (name == n) return true;
  }
  return isConversionBuiltin(name) || isMathBuiltin(name);
}

TypePtr builtinConstantType(const std::string& name) {
  if (name == "PI" || name == "E") return typeFloat(64);
  struct IntConstant {
    const char* name;
    int bits;
    bool isSigned;
  };
  static const IntConstant ints[] = {
      {"I8_MIN", 8, true},   {"I8_MAX", 8, true},   {"I16_MIN", 16, true},
      {"I16_MAX", 16, true}, {"I32_MIN", 32, true}, {"I32_MAX", 32, true},
      {"I64_MIN", 64, true}, {"I64_MAX", 64, true}, {"U8_MAX", 8, false},
      {"U16_MAX", 16, false}, {"U32_MAX", 32, false}};
  for (const auto& entry : ints) {
    if (name == entry.name) return typeInt(entry.bits, entry.isSigned);
  }
  struct FloatConstant {
    const char* name;
    int bits;
  };
  static const FloatConstant floats[] = {
      {"F32_MIN", 32}, {"F32_MAX", 32}, {"F64_MIN", 64}, {"F64_MAX", 64}};
  for (const auto& entry : floats) {
    if (name == entry.name) return typeFloat(entry.bits);
  }
  return nullptr;
}

bool isBuiltinConstant(const std::string& name) {
  return builtinConstantType(name) != nullptr;
}

}  // namespace

Sema::Sema(ModuleGraph& graph, DiagnosticSink& sink) : graph_(graph), sink_(sink) {
  moduleExports_.resize(graph_.modules.size());
}

std::string Sema::qualified(const std::string& name) const {
  return qualifiedFor(currentModule_, name);
}

std::string Sema::qualifiedFor(int moduleId, const std::string& name) const {
  return graph_.at(moduleId).name + "::" + name;
}

ModuleExports& Sema::exportsFor(int moduleId) {
  return moduleExports_[static_cast<std::size_t>(moduleId)];
}

Sema::ModuleAlias* Sema::findAlias(const std::string& name) {
  auto found = moduleAliases_.find(name);
  return found == moduleAliases_.end() ? nullptr : &found->second;
}

TypePtr Sema::moduleMemberType(const ModuleAlias& alias, const std::string& member, Span span,
                               bool report) {
  if (alias.native) {
    if (const std::string* fn = nativeFunction(alias.nativeName, member)) {
      (void)fn;
      if (report) {
        sink_.error(span,
                    "Native function '" + alias.nativeName + "." + member +
                        "' can only be called directly.",
                    "Wrap it in a lambda to pass it as a value, e.g. x => " + alias.nativeName +
                        "." + member + "(x).");
      }
      return typeError();
    }
    if (const std::string* constant = nativeConstant(alias.nativeName, member)) {
      if (TypePtr type = builtinConstantType(*constant)) return type;
    }
    if (report) {
      sink_.error(span, "Module '" + alias.nativeName + "' has no member '" + member + "'.");
    }
    return typeError();
  }

  ModuleExports& exports = exportsFor(alias.moduleId);
  if (auto fn = exports.functions.find(member); fn != exports.functions.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig != signatures_.end()) {
      return typeFunction(sig->second.paramTypes, sig->second.retType);
    }
    return typeError();
  }
  if (exports.structs.count(member)) {
    if (report) {
      sink_.error(span, "'" + exports.name + "." + member + "' is a struct type, not a value.",
                  "Construct it: " + exports.name + "." + member + "(...).");
    }
    return typeError();
  }
  if (auto en = exports.enums.find(member); en != exports.enums.end()) {
    return typeEnum(en->second);
  }
  if (auto value = exports.values.find(member); value != exports.values.end()) {
    return value->second;
  }
  if (report) {
    sink_.error(span, "Module '" + exports.name + "' has no member '" + member + "'.");
  }
  return typeError();
}

TypePtr Sema::moduleMemberCall(const ModuleAlias& alias, const std::string& member,
                               CallExpr& call) {
  if (alias.native) {
    if (const std::string* fn = nativeFunction(alias.nativeName, member)) {
      return checkBuiltinCall(*fn, call);
    }
    if (nativeConstant(alias.nativeName, member)) {
      sink_.error(call.callee->span,
                  "'" + alias.nativeName + "." + member + "' is a constant, not a function.");
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    sink_.error(call.callee->span,
                "Module '" + alias.nativeName + "' has no function '" + member + "'.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }

  ModuleExports& exports = exportsFor(alias.moduleId);
  if (auto fn = exports.functions.find(member); fn != exports.functions.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig == signatures_.end()) {
      for (auto& arg : call.args) inferExpr(*arg.value);
      return typeError();
    }
    return checkFunctionCall(sig->second, call);
  }
  if (exports.structs.count(member)) {
    auto sym = structs_.find(qualifiedFor(alias.moduleId, member));
    if (sym == structs_.end()) return typeError();
    return checkStructConstruction(sym->second, call);
  }
  if (exports.values.count(member)) {
    sink_.error(call.callee->span,
                "'" + exports.name + "." + member + "' is a value, not a function.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  sink_.error(call.callee->span,
              "Module '" + exports.name + "' has no function '" + member + "'.");
  for (auto& arg : call.args) inferExpr(*arg.value);
  return typeError();
}

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
    case TypeKind::Optional:
    case TypeKind::Result:
      return occursIn(var, type->elem) || occursIn(var, type->errorType);
    case TypeKind::Tuple:
    case TypeKind::Function: {
      for (const auto& p : type->params) {
        if (occursIn(var, p)) return true;
      }
      return type->kind == TypeKind::Function ? occursIn(var, type->ret) : false;
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
    b->allowUnresolved = a->allowUnresolved || b->allowUnresolved;
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
      if (a->structName != b->structName) return false;
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return true;
    case TypeKind::Array:
    case TypeKind::Range:
    case TypeKind::Optional:
      return unify(a->elem, b->elem);
    case TypeKind::Result:
      return unify(a->elem, b->elem) && unify(a->errorType, b->errorType);
    case TypeKind::Tuple: {
      if (a->params.size() != b->params.size()) return false;
      for (std::size_t i = 0; i < a->params.size(); ++i) {
        if (!unify(a->params[i], b->params[i])) return false;
      }
      return true;
    }
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
  if (auto* optional = dynamic_cast<OptionalTypeExpr*>(&expr)) {
    return typeOptional(resolveTypeExpr(*optional->element));
  }
  if (auto* tuple = dynamic_cast<TupleTypeExpr*>(&expr)) {
    std::vector<TypePtr> elements;
    elements.reserve(tuple->elements.size());
    for (auto& element : tuple->elements) {
      elements.push_back(resolveTypeExpr(*element));
    }
    return typeTuple(std::move(elements));
  }
  if (auto* fn = dynamic_cast<FunctionTypeExpr*>(&expr)) {
    std::vector<TypePtr> params;
    params.reserve(fn->params.size());
    for (auto& param : fn->params) params.push_back(resolveTypeExpr(*param));
    TypePtr ret = fn->returnType ? resolveTypeExpr(*fn->returnType) : typeVoid();
    return typeFunction(std::move(params), std::move(ret));
  }
  auto* named = dynamic_cast<NamedTypeExpr*>(&expr);
  if (!named) return typeError();
  if (named->qualifiers.empty()) {
    if (genericParams_.count(named->name)) {
      return genericParams_[named->name];
    }
    if (named->name == "Result" && !named->typeArgs.empty()) {
      if (named->typeArgs.size() != 2) {
        sink_.error(named->span, "'Result' requires two type arguments: Result<T, E>.");
        return typeError();
      }
      return typeResult(resolveTypeExpr(*named->typeArgs[0]),
                        resolveTypeExpr(*named->typeArgs[1]));
    }
    if (TypePtr builtin = builtinTypeByName(named->name)) return builtin;
    const std::string q = qualified(named->name);
    if (structs_.count(q)) {
      TypePtr t = typeStruct(q);
      if (!named->typeArgs.empty()) {
        std::vector<TypePtr> args;
        for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
        t->params = std::move(args);
      }
      return t;
    }
    if (enums_.count(q)) return typeInt(32, true);  // enums are i32-backed
    if (auto alias = structAliases_.find(named->name); alias != structAliases_.end()) {
      TypePtr t = typeStruct(alias->second);
      if (!named->typeArgs.empty()) {
        std::vector<TypePtr> args;
        for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
        t->params = std::move(args);
      }
      return t;
    }
    if (typeAliases_.count(q)) return typeAliases_.at(q);
    sink_.error(named->span, "Unknown type '" + named->name + "'.",
                "Type names are 'bool', 'char', 'String', 'void', the sized numeric types, "
                "struct names, '[T]' for arrays, '(T, U)' for tuples, 'fn(T) -> R' for "
                "functions, or 'T?' for optionals.");
    return typeError();
  }

  const std::string qualifier = named->qualifiers.back();
  ModuleAlias* alias = findAlias(qualifier);
  if (!alias) {
    sink_.error(named->span, "Unknown module alias '" + qualifier + "'.");
    return typeError();
  }
  if (alias->native) {
    sink_.error(named->span, "Native modules do not declare types.");
    return typeError();
  }
  const std::string q = qualifiedFor(alias->moduleId, named->name);
  if (structs_.count(q)) {
    TypePtr t = typeStruct(q);
    if (!named->typeArgs.empty()) {
      std::vector<TypePtr> args;
      for (auto& a : named->typeArgs) args.push_back(resolveTypeExpr(*a));
      t->params = std::move(args);
    }
    return t;
  }
  if (enums_.count(q)) return typeInt(32, true);  // enums are i32-backed
  sink_.error(named->span, "Module '" + graph_.at(alias->moduleId).name + "' has no type named '" +
                               named->name + "'.");
  return typeError();
}

TypePtr Sema::instantiateType(TypePtr type, const std::unordered_map<TypePtr, TypePtr>& mapping) {
  type = resolveType(type);
  if (!type) return type;
  if (type->kind == TypeKind::Var) {
    auto it = mapping.find(type);
    if (it != mapping.end()) return it->second;
    // create a fresh var preserving constraints
    TypePtr fresh = makeType(TypeKind::Var);
    fresh->varId = nextVarId_++;
    fresh->constraint = type->constraint;
    fresh->allowDefault = type->allowDefault;
    fresh->allowUnresolved = type->allowUnresolved;
    fresh->varDescription = type->varDescription;
    fresh->varSpan = type->varSpan;
    return fresh;
  }
  TypePtr result = makeType(type->kind);
  result->bits = type->bits;
  result->isSigned = type->isSigned;
  result->structName = type->structName;
  result->moduleId = type->moduleId;
  if (type->elem) result->elem = instantiateType(type->elem, mapping);
  if (type->errorType) result->errorType = instantiateType(type->errorType, mapping);
  if (type->ret) result->ret = instantiateType(type->ret, mapping);
  for (const auto& p : type->params) result->params.push_back(instantiateType(p, mapping));
  return result;
}

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
                      const char* what, bool constant) {
  if (scopes_.empty()) return;
  auto& scope = scopes_.back();
  if (scope.count(name)) {
    sink_.error(span, std::string("Duplicate declaration of ") + what + " '" + name + "'.",
                "A name can only be declared once in the same scope.");
    return;
  }
  if (scopes_.size() == 1) {
    if (functionsByName_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as a function.");
      return;
    }
    if (structsByName_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as a struct.");
      return;
    }
    if (enums_.count(qualified(name))) {
      sink_.error(span, "Name '" + name + "' is already declared as an enum.");
      return;
    }
  }
  scope[name] = VarSym{std::move(type), mutableBinding, constant};
  DeclaredVarInfo info;
  info.name = name;
  info.span = span;
  info.type = scope[name].type;
  declaredVariables_.push_back(std::move(info));
}

// ------------------------------------------------------------ declarations

void Sema::registerStructLayout(StructDecl& decl) {
  auto savedGenericParams = std::move(genericParams_);
  const std::string q = qualified(decl.name);
  StructSym& sym = structs_[q];
  sym.name = q;
  sym.decl = &decl;
  sym.typeParams = decl.typeParams;
  genericParams_.clear();
  for (const auto& name : decl.typeParams) {
    TypePtr var = freshVar(Constraint::None, decl.nameSpan,
                           "type parameter '" + name + "' of '" + decl.name + "'");
    var->allowUnresolved = true;
    genericParams_[name] = var;
    sym.typeParamVars.push_back(var);
  }

  // Inherit fields from baseClass if present
  if (!decl.baseClass.empty()) {
    std::string baseQ = qualified(decl.baseClass);
    auto baseIt = structs_.find(baseQ);
    if (baseIt == structs_.end()) {
      baseIt = structs_.find(decl.baseClass);
    }
    if (baseIt != structs_.end()) {
      for (const auto& f : baseIt->second.fields) {
        sym.fields.push_back(f);
      }
    }
  }

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
  genericParams_ = std::move(savedGenericParams);
}

void Sema::registerEnum(EnumDecl& decl) {
  const std::string q = qualified(decl.name);
  EnumInfo& info = enums_[q];
  info.name = q;

  std::int64_t next = 0;
  for (EnumVariant& variant : decl.variants) {
    if (info.values.count(variant.name)) {
      sink_.error(variant.span, "Duplicate variant '" + variant.name + "' in enum '" +
                                    decl.name + "'.");
      continue;
    }
    if (variant.value) {
      std::int64_t value = 0;
      bool ok = false;
      if (auto* literal = dynamic_cast<IntLitExpr*>(variant.value.get())) {
        value = literal->value;
        ok = true;
      } else if (auto* unary = dynamic_cast<UnaryExpr*>(variant.value.get());
                 unary && unary->op == "-") {
        if (auto* literal = dynamic_cast<IntLitExpr*>(unary->operand.get())) {
          value = -literal->value;
          ok = true;
        }
      }
      if (!ok) {
        sink_.error(variant.value->span,
                    "Enum variant values must be integer literals.",
                    "Write " + variant.name + " = 5.");
        continue;
      }
      info.values[variant.name] = value;
      next = value + 1;
    } else {
      info.values[variant.name] = next;
      ++next;
    }
  }
}

void Sema::registerFunctionSignature(FnDecl& decl) {
  auto saved = std::move(genericParams_);
  FnSig sig;
  sig.typeParams = decl.typeParams;
  for (const auto& name : decl.typeParams) {
    TypePtr var = freshVar(Constraint::None, decl.nameSpan,
                           "type parameter '" + name + "' of '" + decl.name + "'");
    var->allowUnresolved = true;
    genericParams_[name] = var;
    sig.typeParamVars.push_back(var);
  }
  makeSignatureTypes(decl, nullptr, sig);
  genericParams_ = std::move(saved);
  signatures_[&decl] = std::move(sig);
}

void Sema::registerMethodSignatures(StructDecl& decl) {
  const std::string q = qualified(decl.name);
  StructSym& sym = structs_[q];
  TypePtr selfType = typeStruct(q);

  // Inherit methods from baseClass if present
  if (!decl.baseClass.empty()) {
    std::string baseQ = qualified(decl.baseClass);
    auto baseIt = structs_.find(baseQ);
    if (baseIt == structs_.end()) {
      baseIt = structs_.find(decl.baseClass);
    }
    if (baseIt != structs_.end()) {
      for (const auto& m : baseIt->second.methods) {
        sym.methods.insert(m);
      }
    }
  }

  for (auto& methodPtr : decl.methods) {
    FnDecl& method = *methodPtr;
    fnModules_[&method] = currentModule_;
    method.moduleId = currentModule_;
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
    sig.typeParams = method.typeParams;
    // Inherit generic parameters from the owning struct.
    for (std::size_t i = 0; i < sym.typeParams.size() && i < sym.typeParamVars.size(); ++i) {
      sig.typeParams.push_back(sym.typeParams[i]);
      sig.typeParamVars.push_back(sym.typeParamVars[i]);
    }
    auto savedGeneric = std::move(genericParams_);
    genericParams_.clear();
    for (std::size_t i = 0; i < sym.typeParams.size() && i < sym.typeParamVars.size(); ++i) {
      genericParams_[sym.typeParams[i]] = sym.typeParamVars[i];
    }
    for (const auto& name : method.typeParams) {
      TypePtr var = freshVar(Constraint::None, method.nameSpan,
                             "type parameter '" + name + "' of '" + method.name + "'");
      var->allowUnresolved = true;
      genericParams_[name] = var;
      sig.typeParamVars.push_back(var);
    }
    makeSignatureTypes(method, selfType, sig);
    genericParams_ = std::move(savedGeneric);
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
  auto savedGenericParams = std::move(genericParams_);
  scopes_.clear();
  if (!savedScopes.empty()) {
    scopes_.push_back(savedScopes.front());
  } else {
    scopes_.push_back({});
  }
  genericParams_.clear();
  for (std::size_t i = 0; i < decl.typeParams.size() && i < sig.typeParamVars.size(); ++i) {
    genericParams_[decl.typeParams[i]] = sig.typeParamVars[i];
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
  genericParams_ = std::move(savedGenericParams);
}

void Sema::checkStructMethodBodies(StructDecl& decl) {
  for (auto& methodPtr : decl.methods) {
    auto found = signatures_.find(methodPtr.get());
    if (found == signatures_.end()) continue;
    checkFunctionBody(*methodPtr, found->second);
  }
}

// ---------------------------------------------------------------- analyze

void Sema::registerImports(Module& module) {
  auto aliasStem = [](const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
  };

  for (auto& statement : module.program->statements) {
    if (auto* import = dynamic_cast<ImportStmt*>(statement.get())) {
      std::string alias = import->alias;
      if (alias.empty()) alias = import->quoted ? aliasStem(import->path) : import->bareName;
      if (alias.empty()) continue;

      ModuleAlias entry;
      entry.native = import->nativeModule;
      entry.moduleId = import->moduleId;
      entry.nativeName = import->bareName;
      if (!entry.native && entry.moduleId < 0) continue;

      if (moduleAliases_.count(alias)) {
        sink_.error(import->span, "Duplicate import alias '" + alias + "'.");
        continue;
      }
      moduleAliases_[alias] = entry;
      continue;
    }

    if (auto* from = dynamic_cast<FromImportStmt*>(statement.get())) {
      for (auto& item : from->items) {
        FromBinding binding;
        binding.span = item.span;
        binding.moduleId = from->moduleId;

        if (from->nativeModule) {
          binding.native = true;
          if (const std::string* fn = nativeFunction(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Function;
            binding.nativeName = *fn;
          } else if (const std::string* constant = nativeConstant(from->bareName, item.name)) {
            binding.kind = FromBinding::Kind::Value;
            binding.nativeName = *constant;
            binding.type = builtinConstantType(*constant);
            if (!binding.type) {
              sink_.error(item.span, "Unsupported native constant '" + item.name + "'.");
              continue;
            }
          } else {
            sink_.error(item.span,
                        "Module '" + from->bareName + "' has no member '" + item.name + "'.");
            continue;
          }
        } else if (from->moduleId >= 0) {
          ModuleExports& exports = exportsFor(from->moduleId);
          if (auto fn = exports.functions.find(item.name); fn != exports.functions.end()) {
            binding.kind = FromBinding::Kind::Function;
            binding.fn = fn->second;
            auto sig = signatures_.find(binding.fn);
            if (sig != signatures_.end()) {
              binding.type = typeFunction(sig->second.paramTypes, sig->second.retType);
            }
          } else if (auto st = exports.structs.find(item.name); st != exports.structs.end()) {
            binding.kind = FromBinding::Kind::Struct;
            binding.decl = st->second;
            binding.structName = qualifiedFor(from->moduleId, item.name);
            binding.type = typeStruct(binding.structName);
          } else if (auto value = exports.values.find(item.name); value != exports.values.end()) {
            binding.kind = FromBinding::Kind::Value;
            binding.type = value->second;
          } else {
            sink_.error(item.span, "Module '" + exports.name + "' has no exported member '" +
                                       item.name + "'.");
            continue;
          }
        } else {
          continue;
        }

        if (fromBindings_.count(item.alias)) {
          sink_.error(item.span, "Duplicate imported name '" + item.alias + "'.");
          continue;
        }
        if (binding.kind == FromBinding::Kind::Struct) {
          structAliases_[item.alias] = binding.structName;
        }
        fromBindings_[item.alias] = std::move(binding);
      }
    }
  }
}

void Sema::buildModuleExports(Module& module) {
  ModuleExports& exports = exportsFor(module.id);
  if (!scopes_.empty()) {
    for (const auto& entry : scopes_.front()) {
      exports.values[entry.first] = entry.second.type;
    }
  }
  exports.structAliases = structAliases_;
}

SemaResult Sema::analyze() {
  for (int id : graph_.order) analyzeModule(graph_.at(id));

  SemaResult result;
  result.functions = functionsByName_;
  result.structs = structsByName_;
  result.enums = enums_;
  result.modules = moduleExports_;
  result.fnModule = fnModules_;
  result.structModule = structModules_;
  return result;
}

void Sema::analyzeModule(Module& module) {
  currentModule_ = module.id;
  scopes_.clear();
  scopes_.push_back({});
  moduleAliases_.clear();
  fromBindings_.clear();
  structAliases_.clear();
  typeAliases_.clear();
  genericParams_.clear();
  loopDepth_ = 0;
  deferDepth_ = 0;
  currentFunction_ = nullptr;
  currentReturnType_ = nullptr;
  sawReturn_ = false;
  typeVariables_.clear();
  declaredVariables_.clear();
  checkedFunctions_.clear();

  registerImports(module);

  ModuleExports& exports = exportsFor(module.id);
  exports.name = module.name;
  auto& statements = module.program->statements;

  // Pass 1: collect declaration names so duplicates and collisions are clear.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      const std::string q = qualified(decl->name);
      if (structsByName_.count(q) || functionsByName_.count(q)) {
        sink_.error(decl->nameSpan, "Duplicate declaration of '" + decl->name + "'.");
        continue;
      }
      StructSym sym;
      sym.name = q;
      sym.decl = decl;
      structs_[q] = std::move(sym);
      structsByName_[q] = decl;
      structModules_[decl] = currentModule_;
      exports.structs[decl->name] = decl;
    } else if (auto* en = dynamic_cast<EnumDecl*>(stmt.get())) {
      const std::string q = qualified(en->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q)) {
        sink_.error(en->nameSpan, "Duplicate declaration of '" + en->name + "'.");
        continue;
      }
      exports.enums[en->name] = q;
      registerEnum(*en);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      if (functionsByName_.count(q) || structsByName_.count(q) || enums_.count(q) || typeAliases_.count(q)) {
        sink_.error(ta->nameSpan, "Duplicate declaration of '" + ta->name + "'.");
        continue;
      }
    } else if (auto* fn = dynamic_cast<FnDecl*>(stmt.get())) {
      const std::string q = qualified(fn->name);
      if (functionsByName_.count(q) || structsByName_.count(q)) {
        sink_.error(fn->nameSpan, "Duplicate declaration of '" + fn->name + "'.");
        continue;
      }
      functionsByName_[q] = fn;
      fnModules_[fn] = currentModule_;
      fn->moduleId = currentModule_;
      exports.functions[fn->name] = fn;
    }
  }

  // Pass 2: resolve struct layouts and method signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      registerStructLayout(*decl);
      registerMethodSignatures(*decl);
    } else if (auto* ta = dynamic_cast<TypeAliasDecl*>(stmt.get())) {
      const std::string q = qualified(ta->name);
      typeAliases_[q] = resolveTypeExpr(*ta->type);
    }
  }

  // Pass 3: function signatures.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      registerFunctionSignature(*decl);
    }
  }

  // Pass 4: top-level statements (global variables and executable code).
  for (auto& stmt : statements) {
    if (dynamic_cast<FnDecl*>(stmt.get()) || dynamic_cast<StructDecl*>(stmt.get()) ||
        dynamic_cast<EnumDecl*>(stmt.get()) ||
        dynamic_cast<ImportStmt*>(stmt.get()) || dynamic_cast<FromImportStmt*>(stmt.get())) {
      continue;
    }
    checkStatement(*stmt);
  }

  // Pass 5: function and method bodies.
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<StructDecl*>(stmt.get())) {
      auto found = structsByName_.find(qualified(decl->name));
      if (found == structsByName_.end() || found->second != decl) continue;
      checkStructMethodBodies(*decl);
    }
  }
  for (auto& stmt : statements) {
    if (auto* decl = dynamic_cast<FnDecl*>(stmt.get())) {
      auto found = functionsByName_.find(qualified(decl->name));
      if (found == functionsByName_.end() || found->second != decl) continue;
      auto sig = signatures_.find(decl);
      if (sig == signatures_.end()) continue;
      checkFunctionBody(*decl, sig->second);
    }
  }

  checkTypeVariables();
  checkDeclaredVariables();
  checkReturnPaths();

  buildModuleExports(module);
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
  if (auto* node = dynamic_cast<DestructureStmt*>(&stmt)) {
    checkDestructure(*node);
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
  if (auto* node = dynamic_cast<DoWhileStmt*>(&stmt)) {
    checkDoWhile(*node);
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
  if (auto* node = dynamic_cast<EnumDecl*>(&stmt)) {
    sink_.error(node->nameSpan, "Enums can only be declared at the top level.");
    return;
  }
  if (auto* node = dynamic_cast<ImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
    return;
  }
  if (auto* node = dynamic_cast<FromImportStmt*>(&stmt)) {
    sink_.error(node->span, "Imports are only allowed at module top level.");
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

  declareVar(stmt.name, stmt.nameSpan, variableType, stmt.mutableBinding,
             stmt.constantBinding ? "constant" : "variable", stmt.constantBinding);
}

void Sema::checkAssign(AssignStmt& stmt) {
  if (auto* ident = dynamic_cast<IdentExpr*>(stmt.target.get())) {
    VarSym* sym = lookupVar(ident->name);
    if (!sym) {
      if (isBuiltinConstant(ident->name) || ident->name == "None") {
        sink_.error(ident->span,
                    "Cannot assign to the builtin constant '" + ident->name + "'.");
        inferExpr(*stmt.value);
        return;
      }
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
      declareVar(ident->name, ident->span, valueType, true);
      return;
    }

    if (!sym->mutableBinding) {
      if (sym->constant) {
        sink_.error(ident->span, "Cannot assign to constant '" + ident->name + "'.");
      } else {
        sink_.error(ident->span, "Cannot assign to immutable binding '" + ident->name + "'.",
                    "Declare the variable with 'var " + ident->name +
                        " = ...' to make it mutable.");
      }
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

void Sema::checkDestructure(DestructureStmt& stmt) {
  TypePtr valueType = inferExpr(*stmt.value);
  TypePtr resolved = resolveType(valueType);

  if (resolved->kind == TypeKind::Error) return;
  if (resolved->kind == TypeKind::Var) {
    sink_.error(stmt.value->span, "Cannot infer the type being destructured.",
                "Annotate the value or return a tuple from a function.");
    return;
  }
  if (resolved->kind != TypeKind::Tuple) {
    sink_.error(stmt.value->span,
                "Cannot destructure " + typePhrase(resolved) + ".",
                "Destructuring requires a tuple, e.g. (q, r) = divide(7, 2).");
    return;
  }
  if (resolved->params.size() != stmt.names.size()) {
    sink_.error(stmt.span,
                "This tuple has " + std::to_string(resolved->params.size()) +
                    " value(s), but the pattern binds " + std::to_string(stmt.names.size()) +
                    ".");
    return;
  }

  for (std::size_t i = 0; i < stmt.names.size(); ++i) {
    const std::string& name = stmt.names[i];
    if (name == "_") continue;
    for (std::size_t j = 0; j < i; ++j) {
      if (stmt.names[j] == name) {
        sink_.error(stmt.nameSpans[i],
                    "Duplicate binding '" + name + "' in destructuring.");
        break;
      }
    }
    TypePtr element = resolved->params[i];
    if (resolveType(element)->kind == TypeKind::Void) {
      sink_.error(stmt.nameSpans[i], "Cannot bind a void value.");
      element = typeError();
    }
    declareVar(name, stmt.nameSpans[i], element, stmt.mutableBinding);
  }
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

void Sema::checkDoWhile(DoWhileStmt& stmt) {
  ++loopDepth_;
  pushScope();
  checkStatements(stmt.body);
  popScope();
  --loopDepth_;
  TypePtr condType = inferExpr(*stmt.cond);
  requireBool(condType, stmt.cond->span, "The 'while' condition");
}

void Sema::checkFor(ForStmt& stmt) {  TypePtr iterableType = inferExpr(*stmt.iterable);
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
  if (auto* node = dynamic_cast<SafeNavExpr*>(&expr)) return inferSafeNav(*node);
  if (auto* node = dynamic_cast<ArrayLitExpr*>(&expr)) return inferArrayLiteral(*node);
  if (auto* node = dynamic_cast<TupleExpr*>(&expr)) return inferTuple(*node);
  if (auto* node = dynamic_cast<LambdaExpr*>(&expr)) return inferLambda(*node);
  if (auto* node = dynamic_cast<MatchExpr*>(&expr)) return inferMatch(*node);
  if (auto* node = dynamic_cast<TryExpr*>(&expr)) return inferTry(*node);
  if (auto* node = dynamic_cast<ComprehensionExpr*>(&expr)) return inferComprehension(*node);
  if (auto* node = dynamic_cast<RangeExpr*>(&expr)) {
    TypePtr start = inferExpr(*node->start);
    TypePtr end = inferExpr(*node->end);
    requireInteger(start, node->start->span, "The range start");
    requireInteger(end, node->end->span, "The range end");
    if (!unify(start, end)) {
      sink_.error(node->span, "Range endpoints must have the same integer type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    }
    if (node->step) {
      TypePtr step = inferExpr(*node->step);
      requireInteger(step, node->step->span, "The range step");
      if (!unify(step, start)) {
        reportMismatch(step, start, node->step->span, "in the range step");
      }
    }
    return typeRange(start);
  }
  if (auto* node = dynamic_cast<TernaryExpr*>(&expr)) {
    requireBool(inferExpr(*node->cond), node->cond->span, "The condition of '\?:'");
    TypePtr thenType = inferExpr(*node->thenValue);
    TypePtr elseType = inferExpr(*node->elseValue);
    if (!unify(thenType, elseType)) {
      sink_.error(node->span,
                  "Both branches of '\?:' must have the same type, found " + typeName(thenType) +
                      " and " + typeName(elseType) + ".");
      return typeError();
    }
    if (resolveType(thenType)->kind == TypeKind::Void) {
      sink_.error(node->span, "The branches of '\?:' cannot produce void.");
      return typeError();
    }
    return thenType;
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
  if (expr.name == "None") {
    TypePtr element = freshVar(Constraint::None, expr.span, "the value type of 'None'");
    element->allowUnresolved = true;
    return typeOptional(element);
  }
  if (auto imported = fromBindings_.find(expr.name); imported != fromBindings_.end()) {
    FromBinding& binding = imported->second;
    switch (binding.kind) {
      case FromBinding::Kind::Function:
        if (binding.native) {
          sink_.error(expr.span,
                      "Native function '" + expr.name + "' can only be called directly.",
                      "Wrap it in a lambda to pass it as a value, e.g. x => " + expr.name + "(x).");
          return typeError();
        }
        if (binding.type) return binding.type;
        return typeError();
      case FromBinding::Kind::Struct:
        sink_.error(expr.span, "'" + expr.name + "' is a struct type, not a value.",
                    "Construct it: " + expr.name + "(...).");
        return typeError();
      case FromBinding::Kind::Value:
        return binding.type ? binding.type : typeError();
    }
  }
  if (auto alias = moduleAliases_.find(expr.name); alias != moduleAliases_.end()) {
    return alias->second.native ? typeNativeModule(alias->second.nativeName)
                                : typeModule(alias->second.moduleId);
  }
  {
    const std::string enumQ = qualified(expr.name);
    if (enums_.count(enumQ)) return typeEnum(enumQ);
  }
  if (TypePtr constant = builtinConstantType(expr.name)) return constant;
  const std::string q = qualified(expr.name);
  if (auto fn = functionsByName_.find(q); fn != functionsByName_.end()) {
    auto sig = signatures_.find(fn->second);
    if (sig != signatures_.end()) {
      return typeFunction(sig->second.paramTypes, sig->second.retType);
    }
    return typeError();
  }
  if (structsByName_.count(q)) {
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
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == false) {
    // Prefix ++/-- : operand must be a mutable numeric variable.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        sink_.error(expr.opSpan, "Undefined variable '" + ident->name + "'.");
        return typeError();
      }
      if (!sym->mutableBinding) {
        sink_.error(ident->span, "Cannot apply '" + expr.op + "' to immutable binding '" + ident->name + "'.",
                    "Declare the variable with 'var " + ident->name + " = ...' to make it mutable.");
        return typeError();
      }
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if ((expr.op == "++" || expr.op == "--") && expr.postfix == true) {
    // Postfix i++ / i-- : same rules as prefix.
    if (auto* ident = dynamic_cast<IdentExpr*>(expr.operand.get())) {
      VarSym* sym = lookupVar(ident->name);
      if (!sym) {
        sink_.error(expr.opSpan, "Undefined variable '" + ident->name + "'.");
        return typeError();
      }
      if (!sym->mutableBinding) {
        sink_.error(ident->span, "Cannot apply '" + expr.op + "' to immutable binding '" + ident->name + "'.",
                    "Declare the variable with 'var " + ident->name + " = ...' to make it mutable.");
        return typeError();
      }
    } else {
      sink_.error(expr.operand->span, "Operand of '" + expr.op + "' must be a variable.");
      return typeError();
    }
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Int || resolved->kind == TypeKind::Float) {
      return operand;
    }
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Numeric)) {
      return operand;
    }
    sink_.error(expr.opSpan, "Operator '" + expr.op + "' requires a numeric operand, found " + typePhrase(operand) + ".");
    return typeError();
  }
  if (expr.op == "!") {
    TypePtr resolved = resolveType(operand);
    if (resolved->kind == TypeKind::Error) return typeError();
    if (resolved->kind == TypeKind::Optional) return resolved->elem;
    sink_.error(expr.opSpan,
                "'!' can only unwrap an optional value, found " + typePhrase(operand) + ".",
                "Only values of type 'T?' can be unwrapped.");
    return typeError();
  }
  if (expr.op == "not") {
    if (!unify(operand, typeBool())) {
      sink_.error(expr.opSpan,
                  "'not' requires a bool operand, found " + typePhrase(operand) + ".");
      return typeError();
    }
    return typeBool();
  }

  TypePtr resolved = resolveType(operand);
  if (expr.op == "~") {
    if (resolved->kind == TypeKind::Int) return operand;
    if (resolved->kind == TypeKind::Var && constrainVar(operand, Constraint::Integer)) {
      return operand;
    }
    sink_.error(expr.opSpan,
                "Operator '~' cannot be applied to " + typePhrase(operand) + ".",
                "'~' requires an integer operand.");
    return typeError();
  }
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

  if (op == "??") {
    TypePtr leftResolved = resolveType(lhs);
    if (leftResolved->kind == TypeKind::Error) return rhs;
    if (leftResolved->kind != TypeKind::Optional) {
      sink_.error(expr.opSpan,
                  "The left side of '\?\?' must be optional, found " + typePhrase(lhs) + ".",
                  "Use '\?\?' with a value of type 'T?', e.g. maybeName ?? \"guest\".");
      return typeError();
    }
    if (!unify(leftResolved->elem, rhs)) {
      reportMismatch(rhs, leftResolved->elem, expr.rhs->span, "in the fallback of '\?\?'");
    }
    return leftResolved->elem;
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

  if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
    requireInteger(lhs, expr.lhs->span, "The left operand of '" + op + "'");
    requireInteger(rhs, expr.rhs->span, "The right operand of '" + op + "'");
    if (!unify(lhs, rhs)) {
      sink_.error(expr.opSpan, "Operator '" + op + "' requires both operands to have the same "
                               "integer type, found " + typePhrase(lhs) + " and " +
                                   typePhrase(rhs) + ".");
      return typeError();
    }
    return lhs;
  }

  if (op == "in" || op == "not in") {
    return typeBool();
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
      case TypeKind::Optional: {
        if (op != "==" && op != "!=") {
          sink_.error(expr.opSpan, "Optionals can only be compared with '==' and '!='.");
          return typeError();
        }
        TypePtr element = resolveType(type->elem);
        bool comparable = element->kind == TypeKind::Optional || element->kind == TypeKind::Var ||
                          element->kind == TypeKind::Int || element->kind == TypeKind::Float ||
                          element->kind == TypeKind::String || element->kind == TypeKind::Char ||
                          element->kind == TypeKind::Bool;
        if (!comparable) {
          sink_.error(expr.opSpan,
                      "Values of type '" + typeName(type) + "' cannot be compared with '" + op +
                          "'. Optionals can be compared only when their element is a "
                          "primitive, String, char, bool, or another optional.");
          return typeError();
        }
        return typeBool();
      }
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
    if (VarSym* sym = lookupVar(name)) {
      return checkFunctionValueCall(name, sym->type, expr);
    }
    if (auto imported = fromBindings_.find(name); imported != fromBindings_.end()) {
      FromBinding& binding = imported->second;
      if (binding.kind == FromBinding::Kind::Struct) {
        auto sym = structs_.find(binding.structName);
        if (sym == structs_.end()) return typeError();
        return checkStructConstruction(sym->second, expr);
      }
      if (binding.kind == FromBinding::Kind::Function && !binding.native) {
        auto sigIt = signatures_.find(binding.fn);
        if (sigIt == signatures_.end()) return typeError();
        return checkFunctionCall(sigIt->second, expr);
      }
      if (binding.kind == FromBinding::Kind::Function && binding.native) {
        return checkBuiltinCall(binding.nativeName, expr);
      }
      if (!binding.type) {
        for (auto& arg : expr.args) inferExpr(*arg.value);
        return typeError();
      }
      return checkFunctionValueCall(name, binding.type, expr);
    }
    if (moduleAliases_.count(name)) {
      sink_.error(ident->span, "'" + name + "' is a module, not a function.",
                  "Call a member instead, e.g. " + name + ".function(...).");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    const std::string q = qualified(name);
    auto structIt = structs_.find(q);
    if (structIt != structs_.end()) {
      expr.calleeStruct = structIt->second.decl;
      expr.calleeResolved = true;
      return checkStructConstruction(structIt->second, expr);
    }
    auto fnIt = functionsByName_.find(q);
    if (fnIt != functionsByName_.end()) {
      // Record the declaration so the interpreter can call it directly instead
      // of re-resolving the name through the scope chain on every call.
      expr.calleeFunction = fnIt->second;
      expr.calleeResolved = true;
      auto sigIt = signatures_.find(fnIt->second);
      if (sigIt == signatures_.end()) return typeError();
      return checkFunctionCall(sigIt->second, expr);
    }
    if (isKnownBuiltin(name)) {
      expr.builtinId = builtinIdForName(name);
      return checkBuiltinCall(name, expr);
    }
    if (isBuiltinConstant(name) || name == "None") {
      sink_.error(ident->span, "'" + name + "' is a constant, not a function.");
      for (auto& arg : expr.args) inferExpr(*arg.value);
      return typeError();
    }
    sink_.error(ident->span, "Undefined function '" + name + "'.",
                "Functions must be declared before they are called.");
    return typeError();
  }

  if (auto* field = dynamic_cast<FieldExpr*>(expr.callee.get())) {
    TypePtr objectType = inferExpr(*field->object);
    TypePtr object = resolveType(objectType);
    if (object->kind == TypeKind::Module) {
      ModuleAlias alias;
      alias.native = object->moduleId < 0;
      alias.moduleId = object->moduleId;
      alias.nativeName = object->structName;
      return moduleMemberCall(alias, field->field, expr);
    }
    return checkMethodCall(objectType, *field, expr);
  }

  TypePtr calleeType = inferExpr(*expr.callee);
  TypePtr resolved = resolveType(calleeType);
  if (resolved->kind == TypeKind::Function) {
    return checkFunctionValueCall("this expression", calleeType, expr);
  }
  if (resolved->kind != TypeKind::Error) {
    sink_.error(expr.callee->span,
                "Values of type '" + typeName(resolved) + "' are not callable.",
                "Only functions and lambdas can be called.");
  }
  for (auto& arg : expr.args) inferExpr(*arg.value);
  return typeError();
}

TypePtr Sema::checkFunctionValueCall(const std::string& name, TypePtr fnType, CallExpr& call) {
  TypePtr resolved = resolveType(fnType);
  if (resolved->kind == TypeKind::Error) {
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  if (resolved->kind != TypeKind::Function) {
    sink_.error(call.callee->span,
                "'" + name + "' is a value of type '" + typeName(resolved) +
                    "', not a function.",
                "Only functions and lambdas can be called.");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are not supported for function calls.");
      return typeError();
    }
  }
  if (call.args.size() != resolved->params.size()) {
    sink_.error(call.span,
                "This function expects " + std::to_string(resolved->params.size()) +
                    " argument(s), found " + std::to_string(call.args.size()) + ".");
    for (auto& arg : call.args) inferExpr(*arg.value);
    return typeError();
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    checkArgAgainst(*call.args[i].value, resolved->params[i],
                    "in argument " + std::to_string(i + 1) + " to '" + name + "'");
  }
  return resolved->ret;
}

TypePtr Sema::checkFunctionCall(const FnSig& sig, CallExpr& call) {
  FnSig effective = sig;
  if (!sig.typeParams.empty()) {
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < sig.typeParamVars.size(); ++i) {
      TypePtr fresh = freshVar(Constraint::None,
                              sig.typeParamVars[i]->varSpan,
                              "type parameter '" + sig.typeParams[i] + "'");
      fresh->allowUnresolved = true;
      mapping[sig.typeParamVars[i]] = fresh;
    }
    std::vector<TypePtr> params;
    params.reserve(sig.paramTypes.size());
    for (const auto& p : sig.paramTypes) params.push_back(instantiateType(p, mapping));
    effective.paramTypes = std::move(params);
    effective.retType = instantiateType(sig.retType, mapping);
  }

  for (auto& arg : call.args) {
    if (!arg.name.empty()) {
      sink_.error(arg.nameSpan, "Named arguments are only supported for struct construction.");
      return typeError();
    }
  }
  if (call.args.size() != effective.paramTypes.size()) {
    sink_.error(call.span,
                "Function '" + effective.name + "' expects " + std::to_string(effective.paramTypes.size()) +
                    " argument(s), found " + std::to_string(call.args.size()) + ".");
    return typeError();
  }
  for (std::size_t i = 0; i < call.args.size(); ++i) {
    checkArgAgainst(*call.args[i].value, effective.paramTypes[i],
                   "in argument " + std::to_string(i + 1) + " to '" + effective.name + "'");
  }
  return effective.retType;
}

TypePtr Sema::checkStructConstruction(const StructSym& sym, CallExpr& call) {
  std::unordered_map<TypePtr, TypePtr> mapping;
  std::vector<TypePtr> typeArgs;
  for (std::size_t i = 0; i < sym.typeParamVars.size(); ++i) {
    TypePtr fresh = freshVar(Constraint::None, sym.typeParamVars[i]->varSpan,
                            "type parameter '" + sym.typeParams[i] + "'");
    fresh->allowUnresolved = true;
    mapping[sym.typeParamVars[i]] = fresh;
    typeArgs.push_back(fresh);
  }
  std::vector<std::pair<std::string, TypePtr>> fields;
  fields.reserve(sym.fields.size());
  for (const auto& f : sym.fields) {
    fields.emplace_back(f.first, instantiateType(f.second, mapping));
  }

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
    TypePtr result = typeStruct(sym.name);
    if (!typeArgs.empty()) result->params = typeArgs;
    return result;
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
  TypePtr result = typeStruct(sym.name);
  if (!typeArgs.empty()) result->params = typeArgs;
  return result;
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
      for (const auto& fieldEntry : structIt->second.fields) {
        if (fieldEntry.first != method) continue;
        TypePtr fieldType = resolveType(fieldEntry.second);
        if (fieldType->kind == TypeKind::Function) {
          return checkFunctionValueCall(method, fieldEntry.second, call);
        }
        sink_.error(field.fieldSpan,
                    "Field '" + method + "' of '" + object->structName +
                        "' is not callable.",
                    "Only fields with a function type can be called.");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
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
      checkArgAgainst(*call.args[i].value, sig.paramTypes[i + 1],
                      "in argument " + std::to_string(i + 1) + " to method '" + method + "'");
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
    if (method == "reverse") {
      if (rejectNamed()) return typeError();
      if (!call.args.empty()) {
        sink_.error(call.span, "Method 'reverse' takes no arguments.");
        for (auto& arg : call.args) inferExpr(*arg.value);
      }
      return typeVoid();
    }
    if (method == "contains") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'contains' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, object->elem)) {
        reportMismatch(argType, object->elem, call.args[0].value->span,
                       "in argument 1 to 'contains'");
      }
      return typeBool();
    }
    if (method == "join") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'join' expects 1 argument (separator), found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, typeString())) {
        reportMismatch(argType, typeString(), call.args[0].value->span,
                       "in separator argument to 'join'");
      }
      return typeString();
    }
    if (method == "insert") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 2) {
        sink_.error(call.span, "Method 'insert' expects 2 arguments (index, value), found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr indexType = inferExpr(*call.args[0].value);
      requireInteger(indexType, call.args[0].value->span, "The index passed to 'insert'");
      TypePtr valueType = inferExpr(*call.args[1].value);
      if (!unify(valueType, object->elem)) {
        reportMismatch(valueType, object->elem, call.args[1].value->span, "in 'insert'");
      }
      return typeVoid();
    }
    if (method == "remove_at") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'remove_at' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr indexType = inferExpr(*call.args[0].value);
      requireInteger(indexType, call.args[0].value->span, "The index passed to 'remove_at'");
      return object->elem;
    }
    if (method == "index_of") {
      if (rejectNamed()) return typeError();
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'index_of' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      if (!unify(argType, object->elem)) {
        reportMismatch(argType, object->elem, call.args[0].value->span,
                       "in argument 1 to 'index_of'");
      }
      return typeInt(64, true);
    }
    if (method == "sort") {
      if (rejectNamed()) return typeError();
      if (!call.args.empty()) {
        sink_.error(call.span, "Method 'sort' takes no arguments.");
        for (auto& arg : call.args) inferExpr(*arg.value);
      }
      TypePtr element = resolveType(object->elem);
      if (element->kind != TypeKind::Var && element->kind != TypeKind::Int &&
          element->kind != TypeKind::Float && element->kind != TypeKind::String) {
        sink_.error(field.fieldSpan,
                    "'sort' requires an array of integers, floats, or strings, found " +
                        typePhrase(object) + ".");
      }
      return typeVoid();
    }
    if (method == "first" || method == "last") {
      if (rejectNamed()) return typeError();
      if (!call.args.empty()) {
        sink_.error(call.span, "Method '" + method + "' takes no arguments.");
        for (auto& arg : call.args) inferExpr(*arg.value);
      }
      return typeOptional(object->elem);
    }
    sink_.error(field.fieldSpan,
                "Array has no method named '" + method + "'.",
                "Available array methods: push(v), pop(), insert(i, v), remove_at(i), "
                "contains(v), index_of(v), sort(), reverse(), first(), last(), join(sep).");
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
    if (method == "replace") {
      if (call.args.size() != 2) {
        sink_.error(call.span, "Method 'replace' expects 2 arguments, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      for (std::size_t i = 0; i < 2; ++i) {
        TypePtr argType = inferExpr(*call.args[i].value);
        if (!unify(argType, typeString())) {
          reportMismatch(argType, typeString(), call.args[i].value->span,
                         "in argument " + std::to_string(i + 1) + " to 'replace'");
        }
      }
      return typeString();
    }
    if (method == "starts_with" || method == "ends_with" || method == "find") {
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
      return method == "find" ? typeInt(64, true) : typeBool();
    }
    if (method == "repeat") {
      if (call.args.size() != 1) {
        sink_.error(call.span, "Method 'repeat' expects 1 argument, found " +
                                   std::to_string(call.args.size()) + ".");
        for (auto& arg : call.args) inferExpr(*arg.value);
        return typeError();
      }
      TypePtr argType = inferExpr(*call.args[0].value);
      requireInteger(argType, call.args[0].value->span, "The count passed to 'repeat'");
      return typeString();
    }
    if (method == "chars") {
      if (!expectNoArgs()) return typeError();
      return typeArray(typeChar());
    }
    if (method == "try_to_int") {
      if (!expectNoArgs()) return typeError();
      return typeOptional(typeInt(64, true));
    }
    if (method == "try_to_float") {
      if (!expectNoArgs()) return typeError();
      return typeOptional(typeFloat(64));
    }
    sink_.error(field.fieldSpan,
                "String has no method named '" + method + "'.",
                "Available string methods: to_int(), to_float(), try_to_int(), "
                "try_to_float(), upper(), lower(), trim(), contains(sub), split(sep), "
                "replace(sub, with), starts_with(prefix), ends_with(suffix), find(sub), "
                "repeat(n), chars().");
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

  if (name == "flush") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeVoid();
  }

  if (name == "read_file") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeString();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span, "in the path passed to 'read_file'");
    }
    return typeString();
  }

  if (name == "write_file" || name == "append_file") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    for (std::size_t i = 0; i < 2; ++i) {
      TypePtr type = inferExpr(*call.args[i].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[i].value->span,
                       "in argument " + std::to_string(i + 1) + " to '" + name + "'");
      }
    }
    return typeVoid();
  }

  if (name == "file_exists") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeBool();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to 'file_exists'");
    }
    return typeBool();
  }

  if (name == "list_dir") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeArray(typeString());
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to 'list_dir'");
    }
    return typeArray(typeString());
  }

  if (name == "remove_file" || name == "create_dir" || name == "remove_dir") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the path passed to '" + name + "'");
    }
    return typeVoid();
  }

  if (name == "rename") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    for (std::size_t i = 0; i < 2; ++i) {
      TypePtr type = inferExpr(*call.args[i].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[i].value->span,
                       "in argument " + std::to_string(i + 1) + " to 'rename'");
      }
    }
    return typeVoid();
  }

  if (name == "current_dir") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeString();
  }

  if (name == "now") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeInt(64, true);
  }

  if (name == "sleep") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The duration passed to 'sleep'");
    return typeVoid();
  }

  if (name == "panic") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    TypePtr type = inferExpr(*call.args[0].value);
    if (!unify(type, typeString())) {
      reportMismatch(type, typeString(), call.args[0].value->span,
                     "in the message passed to 'panic'");
    }
    return typeVoid();
  }

  if (name == "assert") {
    if (call.args.empty() || call.args.size() > 2) {
      arityError(1);
      inferArgs();
      return typeVoid();
    }
    requireBool(inferExpr(*call.args[0].value), call.args[0].value->span,
                "The condition of 'assert'");
    if (call.args.size() == 2) {
      TypePtr type = inferExpr(*call.args[1].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[1].value->span,
                       "in the message passed to 'assert'");
      }
    }
    return typeVoid();
  }

  if (name == "assert_eq") {
    if (call.args.size() < 2 || call.args.size() > 3) {
      arityError(2);
      inferArgs();
      return typeVoid();
    }
    TypePtr left = inferExpr(*call.args[0].value);
    TypePtr right = inferExpr(*call.args[1].value);
    if (!unify(left, right)) {
      reportMismatch(right, left, call.args[1].value->span, "in the second argument of 'assert_eq'");
    }
    if (call.args.size() == 3) {
      TypePtr type = inferExpr(*call.args[2].value);
      if (!unify(type, typeString())) {
        reportMismatch(type, typeString(), call.args[2].value->span,
                       "in the message passed to 'assert_eq'");
      }
    }
    return typeVoid();
  }

  if (name == "Some") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeOptional(typeError());
    }
    TypePtr value = inferExpr(*call.args[0].value);
    if (resolveType(value)->kind == TypeKind::Void) {
      sink_.error(call.args[0].value->span, "Cannot wrap a void value in 'Some'.");
    }
    return typeOptional(value);
  }

  if (name == "Ok") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
      e->allowUnresolved = true;
      return typeResult(typeError(), e);
    }
    TypePtr value = inferExpr(*call.args[0].value);
    TypePtr e = freshVar(Constraint::None, call.span, "the error type of 'Result'");
    e->allowUnresolved = true;
    return typeResult(value, e);
  }

  if (name == "Err") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
      t->allowUnresolved = true;
      return typeResult(t, typeError());
    }
    TypePtr error = inferExpr(*call.args[0].value);
    TypePtr t = freshVar(Constraint::None, call.span, "the value type of 'Result'");
    t->allowUnresolved = true;
    return typeResult(t, error);
  }

  if (name == "atan2") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeFloat(64);
    }
    requireNumeric(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The y argument of 'atan2'");
    requireNumeric(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The x argument of 'atan2'");
    return typeFloat(64);
  }

  if (name == "random") {
    if (!call.args.empty()) {
      arityError(0);
      inferArgs();
    }
    return typeFloat(64);
  }

  if (name == "random_int") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeInt(64, true);
    }
    requireInteger(inferExpr(*call.args[0].value), call.args[0].value->span,
                   "The low bound of 'random_int'");
    requireInteger(inferExpr(*call.args[1].value), call.args[1].value->span,
                   "The high bound of 'random_int'");
    return typeInt(64, true);
  }

  if (name == "map" || name == "filter" || name == "any" || name == "all") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'" + name + "' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
      inferExpr(*call.args[1].value);
      return typeError();
    }
    if (name == "map") {
      TypePtr mapped = freshVar(Constraint::None, call.span,
                                "the element type produced by 'map'");
      checkArgAgainst(*call.args[1].value, typeFunction({array->elem}, mapped),
                      "in the function passed to 'map'");
      return typeArray(mapped);
    }
    checkArgAgainst(*call.args[1].value, typeFunction({array->elem}, typeBool()),
                    "in the function passed to '" + name + "'");
    return name == "filter" ? typeArray(array->elem) : typeBool();
  }

  if (name == "reduce") {
    if (call.args.size() != 3) {
      arityError(3);
      inferArgs();
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    TypePtr accumulator = inferExpr(*call.args[1].value);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'reduce' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
      inferExpr(*call.args[2].value);
      return typeError();
    }
    checkArgAgainst(*call.args[2].value, typeFunction({accumulator, array->elem}, accumulator),
                    "in the function passed to 'reduce'");
    return accumulator;
  }

  if (name == "sum") {
    if (call.args.size() != 1) {
      arityError(1);
      inferArgs();
      return typeError();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'sum' expects an array, found " + typePhrase(array) + ".");
      return typeError();
    }
    return requireNumeric(array->elem, call.args[0].value->span, "The element type of 'sum'");
  }

  if (name == "join") {
    if (call.args.size() != 2) {
      arityError(2);
      inferArgs();
      return typeString();
    }
    TypePtr arrayType = inferExpr(*call.args[0].value);
    TypePtr array = resolveType(arrayType);
    if (array->kind != TypeKind::Array) {
      sink_.error(call.args[0].value->span,
                  "'join' expects an array as its first argument, found " +
                      typePhrase(array) + ".");
    }
    TypePtr separator = inferExpr(*call.args[1].value);
    if (!unify(separator, typeString())) {
      reportMismatch(separator, typeString(), call.args[1].value->span,
                     "in the separator passed to 'join'");
    }
    return typeString();
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
  if (object->kind == TypeKind::String) return typeChar();
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
  if (expr.step) {
    TypePtr stepType = inferExpr(*expr.step);
    requireInteger(stepType, expr.step->span, "The slice step");
  }

  TypePtr object = resolveType(objectType);
  if (object->kind == TypeKind::Array) return object;
  if (object->kind == TypeKind::String) return typeString();
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

  if (object->kind == TypeKind::Module) {
    ModuleAlias alias;
    alias.native = object->moduleId < 0;
    alias.moduleId = object->moduleId;
    alias.nativeName = object->structName;
    return moduleMemberType(alias, expr.field, expr.fieldSpan, true);
  }

  if (object->kind == TypeKind::Enum) {
    auto enumIt = enums_.find(object->structName);
    if (enumIt == enums_.end()) return typeError();
    if (enumIt->second.values.count(expr.field)) return typeInt(32, true);
    sink_.error(expr.fieldSpan,
                "Enum '" + shortName(object->structName) + "' has no variant named '" +
                    expr.field + "'.");
    return typeError();
  }

  if (object->kind == TypeKind::Struct) {
    auto structIt = structs_.find(object->structName);
    if (structIt == structs_.end()) return typeError();
    std::unordered_map<TypePtr, TypePtr> mapping;
    for (std::size_t i = 0; i < structIt->second.typeParamVars.size() && i < object->params.size(); ++i) {
      mapping[structIt->second.typeParamVars[i]] = object->params[i];
    }
    for (const auto& field : structIt->second.fields) {
      if (field.first == expr.field) {
        return instantiateType(field.second, mapping);
      }
    }
    sink_.error(expr.fieldSpan,
                "Struct '" + object->structName + "' has no field named '" + expr.field + "'.");
    return typeError();
  }

  if (object->kind == TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "Cannot access field '" + expr.field + "' on " + typePhrase(object) + ".",
                "Use '?.' to access fields of an optional value.");
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

TypePtr Sema::inferTuple(TupleExpr& expr) {
  std::vector<TypePtr> elements;
  elements.reserve(expr.elements.size());
  for (auto& element : expr.elements) {
    TypePtr type = inferExpr(*element);
    if (resolveType(type)->kind == TypeKind::Void) {
      sink_.error(element->span, "A tuple element cannot be a void value.");
      type = typeError();
    }
    elements.push_back(type);
  }
  return typeTuple(std::move(elements));
}

TypePtr Sema::inferLambda(LambdaExpr& expr) {
  std::vector<TypePtr> params;
  params.reserve(expr.params.size());
  for (auto& param : expr.params) {
    TypePtr type;
    if (param.type) {
      type = resolveTypeExpr(*param.type);
      if (resolveType(type)->kind == TypeKind::Void) {
        sink_.error(param.span, "Lambda parameter '" + param.name + "' cannot have type void.");
        type = typeError();
      }
    } else {
      type = freshVar(Constraint::None, param.span,
                      "the type of lambda parameter '" + param.name + "'");
    }
    params.push_back(type);
  }

  TypePtr ret = expr.returnType
                    ? resolveTypeExpr(*expr.returnType)
                    : freshVar(Constraint::None, expr.span, "the return type of a lambda");

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    declareVar(expr.params[i].name, expr.params[i].span, params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, ret)) {
    reportMismatch(bodyType, ret, expr.body->span, "in the lambda result");
  }
  popScope();

  return typeFunction(std::move(params), std::move(ret));
}

void Sema::checkLambdaAgainst(LambdaExpr& expr, TypePtr expected) {
  TypePtr fn = resolveType(expected);
  if (fn->kind != TypeKind::Function) {
    inferLambda(expr);
    return;
  }
  if (expr.params.size() != fn->params.size()) {
    sink_.error(expr.span,
                "This lambda declares " + std::to_string(expr.params.size()) +
                    " parameter(s), but the expected function type takes " +
                    std::to_string(fn->params.size()) + ".");
    inferLambda(expr);
    return;
  }

  pushScope();
  for (std::size_t i = 0; i < expr.params.size(); ++i) {
    if (expr.params[i].type) {
      TypePtr declared = resolveTypeExpr(*expr.params[i].type);
      if (!unify(declared, fn->params[i])) {
        reportMismatch(declared, fn->params[i], expr.params[i].span,
                       "in parameter '" + expr.params[i].name + "' of the lambda");
      }
    }
    declareVar(expr.params[i].name, expr.params[i].span, fn->params[i], false, "parameter");
  }
  TypePtr bodyType = inferExpr(*expr.body);
  if (resolveType(bodyType)->kind == TypeKind::Void) {
    sink_.error(expr.body->span, "A lambda body must produce a value.");
  } else if (!unify(bodyType, fn->ret)) {
    reportMismatch(bodyType, fn->ret, expr.body->span, "in the lambda result");
  }
  popScope();
}

void Sema::checkArgAgainst(Expr& expr, TypePtr expected, const std::string& context) {
  if (auto* lambda = dynamic_cast<LambdaExpr*>(&expr)) {
    checkLambdaAgainst(*lambda, expected);
    return;
  }
  TypePtr actual = inferExpr(expr);
  if (!unify(actual, expected)) {
    reportMismatch(actual, expected, expr.span, context);
  }
}

TypePtr Sema::inferSafeNav(SafeNavExpr& expr) {
  TypePtr objectType = inferExpr(*expr.object);
  TypePtr object = resolveType(objectType);

  if (object->kind == TypeKind::Error) return typeError();
  if (object->kind == TypeKind::Var) {
    sink_.error(expr.fieldSpan,
                "Cannot infer the type of the value before '?." + expr.field + "'.",
                "Add a type annotation to the value.");
    return typeError();
  }
  if (object->kind != TypeKind::Optional) {
    sink_.error(expr.fieldSpan,
                "The left side of '?.' must be optional, found " + typePhrase(object) + ".",
                "Use '.' to access fields of a non-optional value.");
    return typeError();
  }
  TypePtr inner = resolveType(object->elem);
  if (inner->kind == TypeKind::Var) {
    sink_.error(expr.fieldSpan,
                "Cannot infer the optional's element type before '?." + expr.field + "'.",
                "Add a type annotation to the value.");
    return typeError();
  }
  if (inner->kind != TypeKind::Struct) {
    sink_.error(expr.fieldSpan,
                "'?.' can only access struct fields, but the optional holds " +
                    typePhrase(inner) + ".");
    return typeError();
  }
  auto structIt = structs_.find(inner->structName);
  if (structIt == structs_.end()) return typeError();
  for (const auto& field : structIt->second.fields) {
    if (field.first != expr.field) continue;
    if (resolveType(field.second)->kind == TypeKind::Optional) return field.second;
    return typeOptional(field.second);
  }
  sink_.error(expr.fieldSpan,
              "Struct '" + inner->structName + "' has no field named '" + expr.field + "'.");
  return typeError();
}

void Sema::checkPattern(Pattern& pattern, TypePtr subjectType) {
  if (pattern.kind == Pattern::Kind::Wildcard) return;

  if (pattern.kind == Pattern::Kind::Literal) {
    TypePtr valueType = inferExpr(*pattern.value);
    if (!unify(valueType, subjectType)) {
      sink_.error(pattern.span,
                  "This pattern has type " + typeName(valueType) + ", but the match subject has "
                  "type " + typeName(subjectType) + ".");
    }
    return;
  }

  TypePtr start = inferExpr(*pattern.rangeStart);
  TypePtr end = inferExpr(*pattern.rangeEnd);
  requireInteger(start, pattern.rangeStart->span, "The range pattern start");
  requireInteger(end, pattern.rangeEnd->span, "The range pattern end");
  if (!unify(start, end)) {
    sink_.error(pattern.span, "Range pattern endpoints must have the same type, found " +
                                  typeName(start) + " and " + typeName(end) + ".");
    return;
  }
  if (!unify(start, subjectType)) {
    sink_.error(pattern.span,
                "This range pattern has type " + typeName(start) + ", but the match subject has "
                "type " + typeName(subjectType) + ".");
    return;
  }
}

TypePtr Sema::inferMatch(MatchExpr& expr) {
  TypePtr subject = inferExpr(*expr.subject);
  TypePtr result;
  bool sawResult = false;

  for (auto& arm : expr.arms) {
    checkPattern(arm.pattern, subject);
    if (arm.guard) {
      requireBool(inferExpr(*arm.guard), arm.guard->span, "A match guard");
    }
    TypePtr armType = inferExpr(*arm.result);
    if (resolveType(armType)->kind == TypeKind::Void) {
      sink_.error(arm.result->span, "A match arm must produce a value.");
      armType = typeError();
    }
    if (!sawResult) {
      result = armType;
      sawResult = true;
    } else if (!unify(result, armType)) {
      sink_.error(arm.result->span,
                  "All match arms must produce the same type, found " + typeName(result) +
                      " and " + typeName(armType) + ".");
    }
  }

  const MatchArm& last = expr.arms.back();
  if (last.pattern.kind != Pattern::Kind::Wildcard || last.guard) {
    sink_.error(expr.span,
                "A match expression must end with an unguarded '_' arm.",
                "Add '_ => ...' so every value is handled.");
  }

  return sawResult ? result : typeError();
}

TypePtr Sema::inferTry(TryExpr& expr) {
  TypePtr operand = inferExpr(*expr.operand);
  TypePtr resolved = resolveType(operand);
  if (resolved->kind != TypeKind::Result) {
    if (resolved->kind != TypeKind::Error) {
      sink_.error(expr.operand->span,
                  "The '?' operator requires a Result value, found '" + typeName(resolved) + "'.");
    }
    return typeError();
  }
  // The enclosing function must return a compatible Result.
  if (currentReturnType_) {
    TypePtr ret = resolveType(currentReturnType_);
    if (ret->kind == TypeKind::Result) {
      if (!unify(ret->errorType, resolved->errorType)) {
        sink_.error(expr.span, "The error type of '?' does not match the function's return type.");
      }
    }
  }
  return resolved->elem;
}

TypePtr Sema::inferComprehension(ComprehensionExpr& expr) {
  TypePtr iterableType = inferExpr(*expr.iterable);
  TypePtr iterable = resolveType(iterableType);
  TypePtr elementType;

  if (iterable->kind == TypeKind::Var) {
    sink_.error(expr.iterable->span,
                "Cannot infer the type of the value being iterated over.",
                "Annotate the variable, or iterate over an array, string, or range.");
    elementType = typeError();
  } else if (iterable->kind == TypeKind::Array || iterable->kind == TypeKind::Range) {
    elementType = iterable->elem;
  } else if (iterable->kind == TypeKind::String) {
    elementType = typeChar();
  } else {
    sink_.error(expr.iterable->span,
                "Cannot iterate over " + typePhrase(iterable) + ".",
                "Comprehensions iterate over arrays, strings, and integer ranges ('0..10').");
    elementType = typeError();
  }

  pushScope();
  declareVar(expr.var, expr.varSpan, elementType, false, "loop variable");
  if (expr.condition) {
    requireBool(inferExpr(*expr.condition), expr.condition->span, "The comprehension condition");
  }
  TypePtr result = inferExpr(*expr.element);
  if (resolveType(result)->kind == TypeKind::Void) {
    sink_.error(expr.element->span, "A comprehension element must produce a value.");
    result = typeError();
  }
  popScope();
  return typeArray(result);
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
  if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
    if (type->kind == TypeKind::Int) return true;
    if (type->kind == TypeKind::Var && constrainVar(targetType, Constraint::Integer)) return true;
    sink_.error(span, "Operator '" + op + "=' requires an integer target, found " +
                          typePhrase(type) + ".");
    return false;
  }
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
    if (resolved->allowUnresolved) continue;
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
