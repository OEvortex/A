#include "ownership/ownership.hpp"

#include <algorithm>
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

enum class BorrowKind { View, Shared, Mutable };

// True when `expr` is a well-formed `borrow(x)` / `borrow_mut(x)` call whose
// result can be tied to the binding being initialized.
bool isBorrowExpression(const Expr& expr) {
  if (expr.kind != ExprKind::Call) return false;
  const auto& call = static_cast<const CallExpr&>(expr);
  if (call.callee->kind != ExprKind::Ident) return false;
  const auto& callee = static_cast<const IdentExpr&>(*call.callee);
  if (callee.name != "borrow" && callee.name != "borrow_mut") return false;
  if (call.args.size() != 1) return false;
  return call.args[0].value->kind == ExprKind::Ident;
}

// The variable at the root of a member/index chain, e.g. `a` for `a[i].name`.
std::string rootIdentName(const Expr& expr) {
  const Expr* current = &expr;
  while (current) {
    switch (current->kind) {
      case ExprKind::Ident:
        return static_cast<const IdentExpr&>(*current).name;
      case ExprKind::Field:
        current = static_cast<const FieldExpr&>(*current).object.get();
        break;
      case ExprKind::Index:
        current = static_cast<const IndexExpr&>(*current).object.get();
        break;
      case ExprKind::Slice:
        current = static_cast<const SliceExpr&>(*current).object.get();
        break;
      case ExprKind::SafeNav:
        current = static_cast<const SafeNavExpr&>(*current).object.get();
        break;
      default:
        return {};
    }
  }
  return {};
}

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
    mutableBorrows_.clear();
    borrowKind_.clear();
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

  static void removeFromList(StringMap<std::vector<std::string>>& lists,
                             const std::string& lender, const std::string& view) {
    auto list = lists.find(lender);
    if (list == lists.end()) return;
    auto& views = list->second;
    views.erase(std::remove(views.begin(), views.end(), view), views.end());
    if (views.empty()) lists.erase(list);
  }

  void releaseBorrow(const std::string& view) {
    auto lender = lenderOf_.find(view);
    if (lender == lenderOf_.end()) return;
    removeFromList(borrows_, lender->second, view);
    removeFromList(mutableBorrows_, lender->second, view);
    borrowKind_.erase(view);
    lenderOf_.erase(lender);
  }

  void addBorrow(const std::string& view, const std::string& lender,
                 BorrowKind kind = BorrowKind::View) {
    releaseBorrow(view);
    lenderOf_[view] = lender;
    borrowKind_[view] = kind;
    if (kind == BorrowKind::Mutable) {
      mutableBorrows_[lender].push_back(view);
    } else {
      borrows_[lender].push_back(view);
    }
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
      return;
    }
    // A mutable borrow gives its holder exclusive access: nobody else may
    // read or write the lender while it is live.
    const std::string root = lenderChain(ident.name);
    auto found = mutableBorrows_.find(root);
    if (found == mutableBorrows_.end()) return;
    for (const std::string& borrower : found->second) {
      if (borrower == ident.name) continue;
      sink_.error(ident.span,
                  "Cannot use '" + ident.name + "' while '" + borrower +
                      "' holds a mutable borrow.",
                  "Release the mutable borrow first, or read through '" + borrower + "'.");
      return;
    }
  }

  // First live borrower of `name` other than `except`, or empty.
  std::string firstBorrower(const std::string& name, const std::string& except = {}) const {
    if (const std::vector<std::string>* views = liveViews(name)) {
      for (const std::string& view : *views) {
        if (view != except) return view;
      }
    }
    auto found = mutableBorrows_.find(name);
    if (found != mutableBorrows_.end() && !found->second.empty()) {
      for (const std::string& borrower : found->second) {
        if (borrower != except) return borrower;
      }
    }
    return {};
  }

  void requireNotBorrowed(const std::string& name, const std::string& action, Span span,
                          const std::string& except = {}) {
    const std::string borrower = firstBorrower(name, except);
    if (borrower.empty()) return;
    sink_.error(span,
                "Cannot " + action + " '" + name + "' while '" + borrower + "' borrows it.",
                "Let '" + borrower +
                    "' go out of scope (or reassign it) before modifying the value's storage.");
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
        const bool borrowInit = let.init && isBorrowExpression(*let.init);
        if (let.init && !borrowInit) useExpr(*let.init);
        releaseBorrow(let.name);
        if (let.init) {
          if (borrowInit) {
            analyzeBorrowInit(let.name, *let.init);
          } else {
            detectView(let.name, *let.init);
          }
        }
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
        // Reassigning a borrower releases it, so it must not count as a
        // conflicting borrow of its own lender.
        requireNotBorrowed(lenderChain(ident.name), "reassign", assign.opSpan, ident.name);
        const bool borrowInit = isBorrowExpression(*assign.value);
        if (!borrowInit) useExpr(*assign.value);
        rebind(ident.name);
        if (borrowInit) {
          analyzeBorrowInit(ident.name, *assign.value);
        } else if (isViewExpression(*assign.value)) {
          detectView(ident.name, *assign.value);
        }
        return;
      }
      // Compound assignment mutates the value through the binding.
      auto kind = borrowKind_.find(ident.name);
      if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
        sink_.error(assign.opSpan,
                    "Cannot modify '" + ident.name + "' through a shared borrow.",
                    "Use borrow_mut to obtain a mutable borrow.");
      }
      useVariable(ident);
      useExpr(*assign.value);
      return;
    }
    const std::string root = rootIdentName(*assign.target);
    if (!root.empty()) {
      auto kind = borrowKind_.find(root);
      if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
        sink_.error(assign.opSpan,
                    "Cannot modify '" + root + "' through a shared borrow.",
                    "Use borrow_mut to obtain a mutable borrow.");
      }
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
            if (lenderOf_.count(object.name)) {
              auto kind = borrowKind_.find(object.name);
              if (kind != borrowKind_.end() && kind->second == BorrowKind::Shared) {
                sink_.error(field.fieldSpan,
                            "Cannot call '" + field.field + "' through shared borrow '" +
                                object.name + "'.",
                            "Use borrow_mut to obtain a mutable borrow.");
              }
            } else {
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

  // ------------------------------------------------------------ borrows

  // Registers `borrower` as a shared or mutable borrower of the variable named
  // by a `borrow(...)` / `borrow_mut(...)` initializer, rejecting borrows that
  // would alias a conflicting live borrow.
  void analyzeBorrowInit(const std::string& borrower, const Expr& init) {
    const auto& call = static_cast<const CallExpr&>(init);
    const auto& callee = static_cast<const IdentExpr&>(*call.callee);
    const BorrowKind kind =
        callee.name == "borrow_mut" ? BorrowKind::Mutable : BorrowKind::Shared;
    const auto& target = static_cast<const IdentExpr&>(*call.args[0].value);

    OwnershipState* state = findState(target.name);
    if (state && state->moved) {
      sink_.error(target.span, "Use of moved value '" + target.name + "'.",
                  "The value was already moved out of this variable.");
      return;
    }

    const std::string lender = lenderChain(target.name);
    if (kind == BorrowKind::Mutable) {
      if (const std::vector<std::string>* views = liveViews(lender)) {
        if (!views->empty()) {
          sink_.error(init.span,
                      "Cannot mutably borrow '" + target.name + "' while '" + views->front() +
                          "' borrows it.",
                      "Release every other borrow before taking a mutable one.");
          return;
        }
      }
      auto existing = mutableBorrows_.find(lender);
      if (existing != mutableBorrows_.end() && !existing->second.empty()) {
        sink_.error(init.span,
                    "Cannot mutably borrow '" + target.name + "' while '" +
                        existing->second.front() + "' holds a mutable borrow.",
                    "Only one mutable borrow may be live at a time.");
        return;
      }
    } else {
      auto existing = mutableBorrows_.find(lender);
      if (existing != mutableBorrows_.end() && !existing->second.empty()) {
        sink_.error(init.span,
                    "Cannot borrow '" + target.name + "' while '" + existing->second.front() +
                        "' holds a mutable borrow.",
                    "Release the mutable borrow before creating a shared one.");
        return;
      }
    }

    addBorrow(borrower, lender, kind);
    scopes_.back().views.push_back(borrower);
    scopes_.back().lenders.push_back(lender);
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
    StringMap<std::vector<std::string>> mutableBorrows;
    StringMap<BorrowKind> borrowKind;
  };

  Snapshot snapshot() const {
    return Snapshot{scopes_, lenderOf_, borrows_, mutableBorrows_, borrowKind_};
  }

  void restore(const Snapshot& state) {
    scopes_ = state.scopes;
    lenderOf_ = state.lenderOf;
    borrows_ = state.borrows;
    mutableBorrows_ = state.mutableBorrows;
    borrowKind_ = state.borrowKind;
  }

  // Union the borrow bookkeeping from `source` into the live maps.
  void unionBorrows(const StringMap<std::vector<std::string>>& shared,
                    const StringMap<std::vector<std::string>>& mut) {
    auto add = [&](const StringMap<std::vector<std::string>>& lists, bool mutableBorrow) {
      for (const auto& entry : lists) {
        auto& target = mutableBorrow ? mutableBorrows_[entry.first] : borrows_[entry.first];
        for (const std::string& borrower : entry.second) {
          if (std::find(target.begin(), target.end(), borrower) == target.end()) {
            target.push_back(borrower);
          }
        }
      }
    };
    add(shared, false);
    add(mut, true);
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
    unionBorrows(other.borrows, other.mutableBorrows);
    for (const auto& entry : other.lenderOf) lenderOf_[entry.first] = entry.second;
    for (const auto& entry : other.borrowKind) borrowKind_[entry.first] = entry.second;
  }

  void mergeWith(const Snapshot& before) {
    // Keep the current (post-body) state, then pull in anything moved before.
    mergeState(before);
  }

  void restoreBorrowsPreservingMoves(const Snapshot& before) {
    // Conditions run against the pre-loop state; views created earlier are
    // still live, so restore their bookkeeping while keeping moved flags.
    StringMap<std::vector<std::string>> mergedBorrows = borrows_;
    StringMap<std::vector<std::string>> mergedMut = mutableBorrows_;
    StringMap<std::string> mergedLenderOf = lenderOf_;
    StringMap<BorrowKind> mergedKind = borrowKind_;
    borrows_ = before.borrows;
    mutableBorrows_ = before.mutableBorrows;
    lenderOf_ = before.lenderOf;
    borrowKind_ = before.borrowKind;
    unionBorrows(mergedBorrows, mergedMut);
    for (const auto& entry : mergedLenderOf) lenderOf_[entry.first] = entry.second;
    for (const auto& entry : mergedKind) borrowKind_[entry.first] = entry.second;
  }

  DiagnosticSink& sink_;
  std::vector<Scope> scopes_;
  StringMap<std::string> lenderOf_;                  // borrower/view -> original lender
  StringMap<std::vector<std::string>> borrows_;      // lender -> live shared borrowers
  StringMap<std::vector<std::string>> mutableBorrows_;  // lender -> live mutable borrowers
  StringMap<BorrowKind> borrowKind_;                 // borrower -> shared/mutable
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
