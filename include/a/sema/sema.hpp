#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a/common/diagnostics.hpp"
#include "a/frontend/ast.hpp"
#include "a/sema/types.hpp"

namespace a {

struct SemaResult {
  std::unordered_map<std::string, const FnDecl*> functions;
  std::unordered_map<std::string, const StructDecl*> structs;
};

class Sema {
 public:
  Sema(Program& program, DiagnosticSink& sink);

  SemaResult analyze();

 private:
  struct VarSym {
    TypePtr type;
    bool mutableBinding = false;
  };

  struct FnSig {
    std::string name;
    std::vector<TypePtr> paramTypes;
    TypePtr retType;
    const FnDecl* decl = nullptr;
  };

  struct StructSym {
    std::string name;
    const StructDecl* decl = nullptr;
    std::vector<std::pair<std::string, TypePtr>> fields;
    std::unordered_map<std::string, FnSig> methods;
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

  // types
  TypePtr freshVar(Constraint constraint, Span span, std::string description,
                   bool allowDefault = false);
  bool unify(TypePtr a, TypePtr b);
  bool bindVar(TypePtr var, TypePtr type);
  bool occursIn(TypePtr var, TypePtr type);
  bool constrainVar(TypePtr var, Constraint constraint);
  TypePtr resolveTypeExpr(TypeExpr& expr);
  TypePtr makeSignatureTypes(FnDecl& decl, TypePtr selfType, FnSig& sig);

  // scopes
  void pushScope();
  void popScope();
  VarSym* lookupVar(const std::string& name);
  void declareVar(const std::string& name, Span span, TypePtr type, bool mutableBinding,
                  const char* what = "variable");

  // declarations
  void registerStructLayout(StructDecl& decl);
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
  TypePtr inferStringLiteral(StringLitExpr& expr);
  TypePtr inferTuple(TupleExpr& expr);
  TypePtr inferLambda(LambdaExpr& expr);
  TypePtr inferMatch(MatchExpr& expr);
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

  Program& program_;
  DiagnosticSink& sink_;

  std::vector<std::unordered_map<std::string, VarSym>> scopes_;
  std::unordered_map<std::string, const FnDecl*> functionsByName_;
  std::unordered_map<std::string, const StructDecl*> structsByName_;
  std::unordered_map<const FnDecl*, FnSig> signatures_;
  std::unordered_map<std::string, StructSym> structs_;
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
