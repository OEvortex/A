#include "ownership/ownership.hpp"

#include <string>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "collections/transparent_map.hpp"

namespace a {
namespace {

// Methods that can change an array's storage. Element writes keep the array's
// length, so they are allowed while a view exists; these are not.
bool isStructuralMutator(const std::string& method) {
  static const char* names[] = {"append", "extend", "insert", "pop",  "remove",
                                "clear",  "remove_at"};
  for (const char* name : names) {
    if (method == name) return true;
  }
  return false;
}

struct OwnershipState {
  bool moved = false;
  Span movedAt;
};

struct Scope {
  StringMap<OwnershipState> vars;
  std::vector<std::string> views;   // views declared in this scope
  std::vector<std::string> lenders;  // their lenders (parallel to `views`)
};

class Analyzer {
 public:
  explicit Analyzer(DiagnosticSink& sink) : sink_(sink) {}

  void analyzeModule(const Module& module) {
    reset();
    scopes_.emplace_back();
    for (const auto& stmt : module.program->statements) {
      if (dynamic_cast<const FnDecl*>(stmt.get()) ||
          dynamic_cast<const StructDecl*>(stmt.get()) ||
          dynamic_cast<const EnumDecl*>(stmt.get()) ||
          dynamic_cast<const TypeAliasDecl*>(stmt.get()) ||
          dynamic_cast<const ImportStmt*>(stmt.get()) ||
          dynamic_cast<const FromImportStmt*>(stmt.get())) {
        continue;
      }
      analyzeStmt(*stmt);
    }
    scopes_.pop_back();
  }

  void analyzeFunction(const FnDecl& decl) {
    reset();
    scopes_.emplace_back();
    for (const auto& param : decl.params) declare(param.name);
    analyzeStatements(decl.body);
    scopes_.pop_back();
  }

 private:
  void reset() {
    scopes_.clear();
    lenderOf_.clear();
    borrows_.clear();
  }

  // -------------------------------------------------------------- state

  OwnershipState* findState(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->vars.find(name);
      if (found != it->vars.end()) return &found->second;
    }
    return nullptr;
  }

  void declare(const std::string& name) {
    releaseBorrow(name);
    scopes_.back().vars[name] = OwnershipState{};
  }

  void rebind(const std::string& name) {
    releaseBorrow(name);
    if (OwnershipState* state = findState(name)) {
      *state = OwnershipState{};
      return;
    }
    declare(name);
  }

  void releaseBorrow(const std::string& view) {
    auto lender = lenderOf_.find(view);
    if (lender == lenderOf_.end()) return;
    auto list = borrows_.find(lender->second);
    if (list != borrows_.end()) {
      auto& views = list->second;
      for (std::size_t i = 0; i < views.size(); ++i) {
        if (views[i] != view) continue;
        views.erase(views.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
      if (views.empty()) borrows_.erase(list);
    }
    lenderOf_.erase(lender);
  }

  void addBorrow(const std::string& view, const std::string& lender) {
    releaseBorrow(view);
    lenderOf_[view] = lender;
    borrows_[lender].push_back(view);
  }

  std::string lenderChain(const std::string& name) const {
    std::string current = name;
    for (int guard = 0; guard < 64; ++guard) {
      auto found = lenderOf_.find(current);
      if (found == lenderOf_.end()) break;
      current = found->second;
    }
    return current;
  }

  const std::vector<std::string>* liveViews(const std::string& name) const {
    auto found = borrows_.find(name);
    return found == borrows_.end() ? nullptr : &found->second;
  }

  void useVariable(const IdentExpr& ident) {
    OwnershipState* state = findState(ident.name);
    if (state && state->moved) {
      sink_.error(ident.span, "Use of moved value '" + ident.name + "'.",
                  "Assign a new value to '" + ident.name +
                      "' before using it, or remove the earlier move().");
    }
  }

  void requireNotBorrowed(const std::string& name, const std::string& action, Span span) {
    const std::vector<std::string>* views = liveViews(name);
    if (!views || views->empty()) return;
    sink_.error(span,
                "Cannot " + action + " '" + name + "' while view '" + views->front() +
                    "' borrows it.",
                "Let '" + views->front() +
                    "' go out of scope (or reassign it) before modifying the array's length.");
  }

  // --------------------------------------------------------- statements

  void analyzeStatements(const std::vector<StmtPtr>& statements) {
    scopes_.emplace_back();
    for (const auto& stmt : statements) analyzeStmt(*stmt);
    popScope();
  }

  void popScope() {
    for (std::size_t i = 0; i < scopes_.back().views.size(); ++i) {
      releaseBorrow(scopes_.back().views[i]);
    }
    scopes_.pop_back();
  }

  void analyzeStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        if (let.init) useExpr(*let.init);
        releaseBorrow(let.name);
        if (let.init) detectView(let.name, *let.init);
        scopes_.back().vars[let.name] = OwnershipState{};
        return;
      }
      case StmtKind::Assign: {
        analyzeAssign(static_cast<const AssignStmt&>(stmt));
        return;
      }
      case StmtKind::Expr: {
        useExpr(*static_cast<const ExprStmt&>(stmt).expr);
        return;
      }
      case StmtKind::Destructure: {
        const auto& destructure = static_cast<const DestructureStmt&>(stmt);
        useExpr(*destructure.value);
        for (const auto& name : destructure.names) {
          if (name != "_") scopes_.back().vars[name] = OwnershipState{};
        }
        return;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) useExpr(*ret.value);
        return;
      }
      case StmtKind::Break:
      case StmtKind::Continue:
        return;
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(stmt);
        Snapshot saved = snapshot();
        if (defer.body) analyzeStmt(*defer.body);
        restore(saved);
        return;
      }
      case StmtKind::If: {
        analyzeIf(static_cast<const IfStmt&>(stmt));
        return;
      }
      case StmtKind::While: {
        analyzeLoop(static_cast<const WhileStmt&>(stmt).cond.get(),
                    static_cast<const WhileStmt&>(stmt).body);
        return;
      }
      case StmtKind::DoWhile: {
        const auto& loop = static_cast<const DoWhileStmt&>(stmt);
        analyzeLoop(nullptr, loop.body);
        if (loop.cond) useExpr(*loop.cond);
        return;
      }
      case StmtKind::For: {
        const auto& loop = static_cast<const ForStmt&>(stmt);
        useExpr(*loop.iterable);
        Snapshot before = snapshot();
        scopes_.emplace_back();
        declare(loop.var);
        for (const auto& inner : loop.body) analyzeStmt(*inner);
        popScope();
        mergeWith(before);
        return;
      }
      case StmtKind::Loop: {
        const auto& loop = static_cast<const LoopStmt&>(stmt);
        analyzeLoop(nullptr, loop.body);
        return;
      }
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
    }
  }

  void analyzeAssign(const AssignStmt& assign) {
    if (assign.target->kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(*assign.target);
      if (assign.op == "=") {
        requireNotBorrowed(lenderChain(ident.name), "reassign", assign.opSpan);
        useExpr(*assign.value);
        rebind(ident.name);
        if (isViewExpression(*assign.value)) {
          detectView(ident.name, *assign.value);
        }
        return;
      }
      useVariable(ident);
      useExpr(*assign.value);
      return;
    }
    useExpr(*assign.target);
    useExpr(*assign.value);
  }

  void analyzeIf(const IfStmt& stmt) {
    Snapshot before = snapshot();
    std::vector<Snapshot> outcomes;
    bool hasElse = !stmt.elseBody.empty();
    for (const auto& branch : stmt.branches) {
      restore(before);
      useExpr(*branch.cond);
      Snapshot condState = snapshot();
      scopes_.emplace_back();
      for (const auto& inner : branch.body) analyzeStmt(*inner);
      popScope();
      outcomes.push_back(snapshot());
      (void)condState;
    }
    if (hasElse) {
      restore(before);
      scopes_.emplace_back();
      for (const auto& inner : stmt.elseBody) analyzeStmt(*inner);
      popScope();
      outcomes.push_back(snapshot());
    } else {
      outcomes.push_back(before);
    }
    restore(outcomes.front());
    for (std::size_t i = 1; i < outcomes.size(); ++i) mergeState(outcomes[i]);
  }

  // Analyze a loop body once. The state after the loop conservatively includes
  // everything the body may have moved or borrowed.
  void analyzeLoop(const Expr* condition, const std::vector<StmtPtr>& body) {
    if (condition) useExpr(*condition);
    Snapshot before = snapshot();
    scopes_.emplace_back();
    for (const auto& inner : body) analyzeStmt(*inner);
    popScope();
    mergeWith(before);
    if (condition) restoreBorrowsPreservingMoves(before);
  }

  // --------------------------------------------------------- expressions

  void useExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return;
      case ExprKind::StringLit: {
        const auto& literal = static_cast<const StringLitExpr&>(expr);
        for (const auto& part : literal.parts) {
          if (part.isExpr && part.expr) useExpr(*part.expr);
        }
        return;
      }
      case ExprKind::Ident:
        useVariable(static_cast<const IdentExpr&>(expr));
        return;
      case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        useExpr(*unary.operand);
        return;
      }
      case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        useExpr(*binary.lhs);
        useExpr(*binary.rhs);
        return;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          const auto& callee = static_cast<const IdentExpr&>(*call.callee);
          if (callee.name == "move") {
            analyzeMove(call);
            return;
          }
          useVariable(callee);
        } else if (call.callee->kind == ExprKind::Field) {
          const auto& field = static_cast<const FieldExpr&>(*call.callee);
          useExpr(*field.object);
          if (field.object->kind == ExprKind::Ident && isStructuralMutator(field.field)) {
            const auto& object = static_cast<const IdentExpr&>(*field.object);
            // Method calls on a view go through the view itself; the runtime
            // rejects structural mutation of a view on its own.
            if (!lenderOf_.count(object.name)) {
              requireNotBorrowed(lenderChain(object.name), "call '" + field.field + "' on",
                                 field.fieldSpan);
            }
          }
        } else {
          useExpr(*call.callee);
        }
        for (const auto& arg : call.args) useExpr(*arg.value);
        return;
      }
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        useExpr(*index.object);
        useExpr(*index.index);
        return;
      }
      case ExprKind::Slice: {
        const auto& slice = static_cast<const SliceExpr&>(expr);
        useExpr(*slice.object);
        if (slice.start) useExpr(*slice.start);
        if (slice.end) useExpr(*slice.end);
        if (slice.step) useExpr(*slice.step);
        return;
      }
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        useExpr(*field.object);
        return;
      }
      case ExprKind::SafeNav: {
        const auto& nav = static_cast<const SafeNavExpr&>(expr);
        useExpr(*nav.object);
        return;
      }
      case ExprKind::ArrayLit: {
        for (const auto& element : static_cast<const ArrayLitExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
      case ExprKind::Range: {
        const auto& range = static_cast<const RangeExpr&>(expr);
        useExpr(*range.start);
        useExpr(*range.end);
        if (range.step) useExpr(*range.step);
        return;
      }
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        useExpr(*ternary.cond);
        useExpr(*ternary.thenValue);
        useExpr(*ternary.elseValue);
        return;
      }
      case ExprKind::Tuple: {
        for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
      case ExprKind::Lambda:
      case ExprKind::Match:
      case ExprKind::Try:
      case ExprKind::Comprehension:
        analyzeComplexExpr(expr);
        return;
      case ExprKind::MapLit: {
        for (const auto& entry : static_cast<const MapLitExpr&>(expr).entries) {
          useExpr(*entry.first);
          useExpr(*entry.second);
        }
        return;
      }
      case ExprKind::SetLit: {
        for (const auto& element : static_cast<const SetLitExpr&>(expr).elements) {
          useExpr(*element);
        }
        return;
      }
    }
  }

  void analyzeComplexExpr(const Expr& expr) {
    if (const auto* lambda = dynamic_cast<const LambdaExpr*>(&expr)) {
      Snapshot saved = snapshot();
      scopes_.emplace_back();
      for (const auto& param : lambda->params) declare(param.name);
      useExpr(*lambda->body);
      popScope();
      restore(saved);
      return;
    }
    if (const auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
      useExpr(*match->subject);
      for (const auto& arm : match->arms) {
        if (arm.guard) useExpr(*arm.guard);
        useExpr(*arm.result);
      }
      return;
    }
    if (const auto* tryExpr = dynamic_cast<const TryExpr*>(&expr)) {
      useExpr(*tryExpr->operand);
      return;
    }
    if (const auto* comprehension = dynamic_cast<const ComprehensionExpr*>(&expr)) {
      useExpr(*comprehension->iterable);
      Snapshot saved = snapshot();
      scopes_.emplace_back();
      declare(comprehension->var);
      if (comprehension->condition) useExpr(*comprehension->condition);
      useExpr(*comprehension->element);
      popScope();
      restore(saved);
    }
  }

  void analyzeMove(const CallExpr& call) {
    if (call.args.size() != 1) return;
    const Expr& value = *call.args[0].value;
    if (value.kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(value);
      OwnershipState* state = findState(ident.name);
      if (state && state->moved) {
        sink_.error(ident.span, "Use of moved value '" + ident.name + "'.",
                    "The value was already moved out of this variable.");
      }
      requireNotBorrowed(lenderChain(ident.name), "move", value.span);
      if (!state) {
        scopes_.back().vars[ident.name] = OwnershipState{};
        state = &scopes_.back().vars[ident.name];
      }
      state->moved = true;
      state->movedAt = value.span;
      return;
    }
    useExpr(value);
  }

  // ------------------------------------------------------------- views

  bool isViewExpression(const Expr& expr) const {
    if (expr.kind != ExprKind::Slice) return false;
    const auto& slice = static_cast<const SliceExpr&>(expr);
    if (!slice.step) return true;  // unit step
    if (slice.step->kind != ExprKind::IntLit) return false;
    return static_cast<const IntLitExpr&>(*slice.step).value == 1;
  }

  void detectView(const std::string& view, const Expr& init) {
    if (!isViewExpression(init)) return;
    const auto& slice = static_cast<const SliceExpr&>(init);
    if (slice.object->kind != ExprKind::Ident) return;
    const auto& object = static_cast<const IdentExpr&>(*slice.object);
    const std::string lender = lenderChain(object.name);
    addBorrow(view, lender);
    scopes_.back().views.push_back(view);
    scopes_.back().lenders.push_back(lender);
  }

  // --------------------------------------------------------- snapshots

  struct Snapshot {
    std::vector<Scope> scopes;
    StringMap<std::string> lenderOf;
    StringMap<std::vector<std::string>> borrows;
  };

  Snapshot snapshot() const { return Snapshot{scopes_, lenderOf_, borrows_}; }

  void restore(const Snapshot& state) {
    scopes_ = state.scopes;
    lenderOf_ = state.lenderOf;
    borrows_ = state.borrows;
  }

  // Merge the branch/body state into the current state: a value moved on any
  // path counts as moved afterwards, and borrows are unioned.
  void mergeState(const Snapshot& other) {
    for (const auto& scope : other.scopes) {
      for (const auto& entry : scope.vars) {
        if (!entry.second.moved) continue;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
          auto found = it->vars.find(entry.first);
          if (found == it->vars.end()) continue;
          found->second.moved = true;
          found->second.movedAt = entry.second.movedAt;
          break;
        }
      }
    }
    for (const auto& entry : other.borrows) {
      for (const auto& view : entry.second) {
        if (!borrows_.count(entry.first)) borrows_[entry.first] = entry.second;
        lenderOf_[view] = entry.first;
      }
    }
  }

  void mergeWith(const Snapshot& before) {
    // Keep the current (post-body) state, then pull in anything moved before.
    mergeState(before);
  }

  void restoreBorrowsPreservingMoves(const Snapshot& before) {
    // Conditions run against the pre-loop state; views created earlier are
    // still live, so restore their bookkeeping while keeping moved flags.
    StringMap<std::vector<std::string>> mergedBorrows = borrows_;
    StringMap<std::string> mergedLenderOf = lenderOf_;
    borrows_ = before.borrows;
    lenderOf_ = before.lenderOf;
    for (const auto& entry : mergedBorrows) {
      for (const auto& view : entry.second) lenderOf_[view] = entry.first;
      auto& target = borrows_[entry.first];
      for (const auto& view : entry.second) {
        bool present = false;
        for (const auto& existing : target) present = present || existing == view;
        if (!present) target.push_back(view);
      }
    }
    for (const auto& entry : mergedLenderOf) lenderOf_[entry.first] = entry.second;
  }

  DiagnosticSink& sink_;
  std::vector<Scope> scopes_;
  StringMap<std::string> lenderOf_;                  // view -> original lender
  StringMap<std::vector<std::string>> borrows_;      // lender -> live views
};

}  // namespace

void analyzeOwnership(const ModuleGraph& graph, DiagnosticSink& sink) {
  for (const auto& module : graph.modules) {
    if (!module.program) continue;
    Analyzer analyzer(sink);
    analyzer.analyzeModule(module);
    for (const auto& stmt : module.program->statements) {
      if (const auto* fn = dynamic_cast<const FnDecl*>(stmt.get())) {
        Analyzer functionAnalyzer(sink);
        functionAnalyzer.analyzeFunction(*fn);
      }
    }
  }
}

}  // namespace a
