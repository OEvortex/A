#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "diagnostics/diagnostics.hpp"
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/types.hpp"

namespace a {

// Per-module exported symbols, keyed by their short names.
struct ModuleExports {
  std::string name;
  StringMap<const FnDecl*> functions;
  StringMap<const StructDecl*> structs;
  StringMap<TypePtr> values;
  StringMap<std::string> structAliases;  // local alias -> qualified
  StringMap<std::string> enums;  // name -> qualified enum name
};

struct EnumInfo {
  std::string name;  // qualified
  StringMap<std::int64_t> values;
};

struct SemaResult {
  StringMap<const FnDecl*> functions;  // qualified names
  StringMap<const StructDecl*> structs;  // qualified names
  StringMap<EnumInfo> enums;  // qualified names
  std::vector<ModuleExports> modules;
  std::unordered_map<const FnDecl*, int> fnModule;
  std::unordered_map<const StructDecl*, int> structModule;
};

class Sema {
 public:
  Sema(ModuleGraph& graph, DiagnosticSink& sink);

  SemaResult analyze();

 private:
  struct VarSym {
    TypePtr type;
    bool mutableBinding = false;
    bool constant = false;
  };

  struct FnSig {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<TypePtr> typeParamVars;
    std::vector<TypePtr> paramTypes;
    TypePtr retType;
    const FnDecl* decl = nullptr;
  };

  struct StructSym {
    std::string name;
    const StructDecl* decl = nullptr;
    std::vector<std::string> typeParams;
    std::vector<TypePtr> typeParamVars;
    std::vector<std::pair<std::string, TypePtr>> fields;
    StringMap<FnSig> methods;
  };

  struct VarInfo {
    TypePtr var;
    Span span;
    std::string description;
  };

  struct DeclaredVarInfo {
    std::string name;
    Span span;
    TypePtr type;
  };

  struct ModuleAlias {
    bool native = false;
    int moduleId = -1;
    std::string nativeName;
  };

  struct FromBinding {
    enum class Kind { Value, Function, Struct };
    Kind kind = Kind::Value;
    bool native = false;
    std::string nativeName;  // builtin implementing a native member
    const FnDecl* fn = nullptr;
    const StructDecl* decl = nullptr;
    std::string structName;  // qualified struct name
    TypePtr type;
    int moduleId = -1;
    Span span;
  };

  // program-level
  void analyzeModule(Module& module);
  void registerImports(Module& module);
  void buildModuleExports(Module& module);
  std::string qualified(const std::string& name) const;
  std::string qualifiedFor(int moduleId, const std::string& name) const;
  ModuleExports& exportsFor(int moduleId);
  TypePtr moduleMemberType(const ModuleAlias& alias, const std::string& member, Span span,
                           bool report);
  TypePtr moduleMemberCall(const ModuleAlias& alias, const std::string& member, CallExpr& call);
  ModuleAlias* findAlias(const std::string& name);

  // types
  TypePtr freshVar(Constraint constraint, Span span, std::string description,
                   bool allowDefault = false);
  bool unify(TypePtr a, TypePtr b);
  bool bindVar(TypePtr var, TypePtr type);
  bool occursIn(TypePtr var, TypePtr type);
  bool constrainVar(TypePtr var, Constraint constraint);
  TypePtr resolveTypeExpr(TypeExpr& expr);
  TypePtr instantiateType(TypePtr type, const std::unordered_map<TypePtr, TypePtr>& mapping);
  TypePtr makeSignatureTypes(FnDecl& decl, TypePtr selfType, FnSig& sig);

  // scopes
  void pushScope();
  void popScope();
  VarSym* lookupVar(const std::string& name);
  void declareVar(const std::string& name, Span span, TypePtr type, bool mutableBinding,
                  const char* what = "variable", bool constant = false);

  // declarations
  void registerStructLayout(StructDecl& decl);
  void registerEnum(EnumDecl& decl);
  void registerFunctionSignature(FnDecl& decl);
  void registerMethodSignatures(StructDecl& decl);
  void checkFunctionBody(FnDecl& decl, const FnSig& sig);
  void checkStructMethodBodies(StructDecl& decl);

  // statements
  void checkStatements(std::vector<StmtPtr>& statements);
  void checkStatement(Stmt& stmt);
  void checkLet(LetStmt& stmt);
  void checkAssign(AssignStmt& stmt);
  void checkDestructure(DestructureStmt& stmt);
  void checkIf(IfStmt& stmt);
  void checkWhile(WhileStmt& stmt);
  void checkDoWhile(DoWhileStmt& stmt);
  void checkFor(ForStmt& stmt);
  void checkLoop(LoopStmt& stmt);
  void checkReturn(ReturnStmt& stmt);
  void checkDefer(DeferStmt& stmt);

  // expressions
  TypePtr inferExpr(Expr& expr);
  TypePtr inferIdent(IdentExpr& expr);
  TypePtr inferUnary(UnaryExpr& expr);
  TypePtr inferBinary(BinaryExpr& expr);
  TypePtr inferCall(CallExpr& expr);
  TypePtr inferIndex(IndexExpr& expr);
  TypePtr inferSlice(SliceExpr& expr);
  TypePtr inferField(FieldExpr& expr);
  TypePtr inferSafeNav(SafeNavExpr& expr);
  TypePtr inferArrayLiteral(ArrayLitExpr& expr);
  TypePtr inferMapLiteral(MapLitExpr& expr);
  TypePtr inferSetLiteral(SetLitExpr& expr);
  TypePtr inferStringLiteral(StringLitExpr& expr);
  TypePtr inferTuple(TupleExpr& expr);
  TypePtr inferLambda(LambdaExpr& expr);
  TypePtr inferMatch(MatchExpr& expr);
  TypePtr inferTry(TryExpr& expr);
  TypePtr inferComprehension(ComprehensionExpr& expr);
  void checkPattern(Pattern& pattern, TypePtr subjectType);
  void checkLambdaAgainst(LambdaExpr& expr, TypePtr expected);
  void checkArgAgainst(Expr& expr, TypePtr expected, const std::string& context);

  TypePtr checkFunctionValueCall(const std::string& name, TypePtr fnType, CallExpr& call);

  TypePtr checkStructConstruction(const StructSym& sym, CallExpr& call);
  TypePtr checkFunctionCall(const FnSig& sig, CallExpr& call);
  TypePtr checkBuiltinCall(const std::string& name, CallExpr& call);
  TypePtr checkMethodCall(const TypePtr& objectType, FieldExpr& field, CallExpr& call);

  // helpers
  void requireBool(TypePtr type, Span span, const std::string& what);
  void requireInteger(TypePtr type, Span span, const std::string& what);
  TypePtr requireNumeric(TypePtr type, Span span, const std::string& what);
  TypePtr numericBinaryResult(const std::string& op, TypePtr lhs, TypePtr rhs, Span span);
  bool checkCompoundOp(const std::string& op, TypePtr targetType, Span span);
  void reportMismatch(TypePtr actual, TypePtr expected, Span span, const std::string& context);
  std::string typePhrase(const TypePtr& type) const;
  void checkTypeVariables();
  void checkDeclaredVariables();
  void checkReturnPaths();
  bool blockReturns(const std::vector<StmtPtr>& body);
  bool statementReturns(const Stmt& stmt);

  ModuleGraph& graph_;
  DiagnosticSink& sink_;
  std::vector<ModuleExports> moduleExports_;
  int currentModule_ = -1;

  StringMap<ModuleAlias> moduleAliases_;
  StringMap<FromBinding> fromBindings_;
  StringMap<std::string> structAliases_;  // local alias -> qualified name
  StringMap<TypePtr> genericParams_;  // current generic type parameters

  std::vector<StringMap<VarSym>> scopes_;
  StringMap<const FnDecl*> functionsByName_;
  StringMap<const StructDecl*> structsByName_;
  StringMap<EnumInfo> enums_;
  std::unordered_map<const FnDecl*, FnSig> signatures_;
  StringMap<StructSym> structs_;
  StringMap<TypePtr> typeAliases_;
  std::unordered_map<const FnDecl*, int> fnModules_;
  std::unordered_map<const StructDecl*, int> structModules_;
  std::vector<VarInfo> typeVariables_;
  std::vector<DeclaredVarInfo> declaredVariables_;
  std::vector<std::pair<const FnDecl*, TypePtr>> checkedFunctions_;

  int nextVarId_ = 1;
  int loopDepth_ = 0;
  int deferDepth_ = 0;
  FnDecl* currentFunction_ = nullptr;
  TypePtr currentReturnType_;
  bool sawReturn_ = false;
};

}  // namespace a
