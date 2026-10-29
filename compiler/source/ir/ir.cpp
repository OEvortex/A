#include "ir/ir.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "collections/transparent_map.hpp"
#include "name-resolution/module_loader.hpp"
#include "strings/utf8.hpp"
#include "type-checker/sema.hpp"

namespace a {
namespace ir {
namespace {

TypePtr intType() { return typeInt(64, true); }

bool lookupEnv(const StringMap<int>& env, const std::string& name, int& out) {
  auto found = env.find(name);
  if (found == env.end()) return false;
  out = found->second;
  return true;
}

// Names assigned by a statement list. Loop headers need this before the body is
// lowered so the phis that merge the preheader and backedge values exist.
void collectAssignedExpr(const Expr& expr, std::set<std::string>& names);
void collectAssignedStmt(const Stmt& stmt, std::set<std::string>& names);

void collectAssigned(const std::vector<StmtPtr>& stmts, std::set<std::string>& names) {
  for (const auto& stmt : stmts) {
    switch (stmt->kind) {
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(*stmt);
        if (assign.target->kind == ExprKind::Ident) {
          names.insert(static_cast<const IdentExpr&>(*assign.target).name);
        } else {
          collectAssignedExpr(*assign.target, names);
        }
        collectAssignedExpr(*assign.value, names);
        break;
      }
      case StmtKind::Destructure: {
        const auto& destructure = static_cast<const DestructureStmt&>(*stmt);
        for (const auto& name : destructure.names) {
          if (name != "_") names.insert(name);
        }
        collectAssignedExpr(*destructure.value, names);
        break;
      }
      case StmtKind::Expr:
        collectAssignedExpr(*static_cast<const ExprStmt&>(*stmt).expr, names);
        break;
      case StmtKind::If: {
        const auto& branch = static_cast<const IfStmt&>(*stmt);
        for (const auto& part : branch.branches) {
          collectAssignedExpr(*part.cond, names);
          collectAssigned(part.body, names);
        }
        collectAssigned(branch.elseBody, names);
        break;
      }
      case StmtKind::While:
        collectAssigned(static_cast<const WhileStmt&>(*stmt).body, names);
        break;
      case StmtKind::DoWhile:
        collectAssigned(static_cast<const DoWhileStmt&>(*stmt).body, names);
        break;
      case StmtKind::For:
        collectAssigned(static_cast<const ForStmt&>(*stmt).body, names);
        break;
      case StmtKind::Loop:
        collectAssigned(static_cast<const LoopStmt&>(*stmt).body, names);
        break;
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(*stmt);
        if (defer.body) collectAssignedStmt(*defer.body, names);
        break;
      }
      default:
        break;
    }
  }
}

void collectAssignedStmt(const Stmt& stmt, std::set<std::string>& names) {
  switch (stmt.kind) {
    case StmtKind::Assign: {
      const auto& assign = static_cast<const AssignStmt&>(stmt);
      if (assign.target->kind == ExprKind::Ident) {
        names.insert(static_cast<const IdentExpr&>(*assign.target).name);
      }
      collectAssignedExpr(*assign.value, names);
      break;
    }
    case StmtKind::Expr:
      collectAssignedExpr(*static_cast<const ExprStmt&>(stmt).expr, names);
      break;
    default:
      break;
  }
}

void collectAssignedExpr(const Expr& expr, std::set<std::string>& names) {
  switch (expr.kind) {
    case ExprKind::Ident:
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
      return;
    case ExprKind::StringLit: {
      for (const auto& part : static_cast<const StringLitExpr&>(expr).parts) {
        if (part.isExpr && part.expr) collectAssignedExpr(*part.expr, names);
      }
      return;
    }
    case ExprKind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if ((unary.op == "++" || unary.op == "--") && unary.operand->kind == ExprKind::Ident) {
        names.insert(static_cast<const IdentExpr&>(*unary.operand).name);
      }
      collectAssignedExpr(*unary.operand, names);
      return;
    }
    case ExprKind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);
      collectAssignedExpr(*binary.lhs, names);
      collectAssignedExpr(*binary.rhs, names);
      return;
    }
    case ExprKind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      collectAssignedExpr(*call.callee, names);
      for (const auto& arg : call.args) collectAssignedExpr(*arg.value, names);
      return;
    }
    case ExprKind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expr);
      collectAssignedExpr(*index.object, names);
      collectAssignedExpr(*index.index, names);
      return;
    }
    case ExprKind::Slice: {
      const auto& slice = static_cast<const SliceExpr&>(expr);
      collectAssignedExpr(*slice.object, names);
      if (slice.start) collectAssignedExpr(*slice.start, names);
      if (slice.end) collectAssignedExpr(*slice.end, names);
      if (slice.step) collectAssignedExpr(*slice.step, names);
      return;
    }
    case ExprKind::Field: {
      collectAssignedExpr(*static_cast<const FieldExpr&>(expr).object, names);
      return;
    }
    case ExprKind::SafeNav: {
      collectAssignedExpr(*static_cast<const SafeNavExpr&>(expr).object, names);
      return;
    }
    case ExprKind::ArrayLit: {
      for (const auto& element : static_cast<const ArrayLitExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::Range: {
      const auto& range = static_cast<const RangeExpr&>(expr);
      collectAssignedExpr(*range.start, names);
      collectAssignedExpr(*range.end, names);
      if (range.step) collectAssignedExpr(*range.step, names);
      return;
    }
    case ExprKind::Ternary: {
      const auto& ternary = static_cast<const TernaryExpr&>(expr);
      collectAssignedExpr(*ternary.cond, names);
      collectAssignedExpr(*ternary.thenValue, names);
      collectAssignedExpr(*ternary.elseValue, names);
      return;
    }
    case ExprKind::Tuple: {
      for (const auto& element : static_cast<const TupleExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::MapLit: {
      for (const auto& entry : static_cast<const MapLitExpr&>(expr).entries) {
        collectAssignedExpr(*entry.first, names);
        collectAssignedExpr(*entry.second, names);
      }
      return;
    }
    case ExprKind::SetLit: {
      for (const auto& element : static_cast<const SetLitExpr&>(expr).elements) {
        collectAssignedExpr(*element, names);
      }
      return;
    }
    case ExprKind::Lambda:
    case ExprKind::Comprehension:
    case ExprKind::Match:
    case ExprKind::Try:
      return;
  }
}

class Builder {
 public:
  explicit Builder(Function& function) : fn_(function) {}

  void lowerParameters() {
    int entry = addBlock("entry");
    current_ = entry;
    for (auto& param : fn_.params) {
      Inst inst;
      inst.op = Op::Param;
      inst.type = param.type;
      inst.text = param.name;
      int value = emit(inst);
      param.value = value;
      scopes_.back()[param.name] = value;
    }
  }

  void lowerBody(const std::vector<StmtPtr>& body) {
    lowerStatements(body);
    finish();
  }

  void lowerTopLevel(const std::vector<StmtPtr>& statements) {
    for (const auto& stmt : statements) {
      if (terminated_) break;
      if (stmt->kind == StmtKind::FnDecl || stmt->kind == StmtKind::StructDecl ||
          stmt->kind == StmtKind::EnumDecl || stmt->kind == StmtKind::TypeAlias ||
          stmt->kind == StmtKind::Import || stmt->kind == StmtKind::FromImport) {
        continue;
      }
      lowerStmt(*stmt);
    }
    finish();
  }

 private:
  struct LoopContext {
    int header = -1;
    int exit = -1;
    int continueTarget = -1;
    std::vector<std::pair<std::string, int>> headerPhis;  // empty name = synthetic
  };
  struct Edge {
    int from = -1;
    StringMap<int> env;
  };

  void finish() {
    if (terminated_) return;
    Terminator term;
    term.kind = Terminator::Kind::Return;
    fn_.blocks[static_cast<std::size_t>(current_)].term = term;
    terminated_ = true;
  }

  int addBlock(std::string label) {
    Block block;
    block.label = std::move(label);
    fn_.blocks.push_back(std::move(block));
    return static_cast<int>(fn_.blocks.size()) - 1;
  }

  std::string uniqueLabel(const std::string& base) {
    return base + "." + std::to_string(labelCounter_++);
  }

  Block& block(int index) { return fn_.blocks[static_cast<std::size_t>(index)]; }

  Inst& instruction(int id) {
    auto found = defs_.find(id);
    return block(found->second.first).insts[found->second.second];
  }

  int emit(Inst inst) {
    inst.id = fn_.nextValue++;
    defs_[inst.id] = {current_, block(current_).insts.size()};
    if (inst.type) types_[inst.id] = inst.type;
    block(current_).insts.push_back(std::move(inst));
    return fn_.nextValue - 1;
  }

  int emitConstInt(std::int64_t value, TypePtr type = intType()) {
    Inst inst;
    inst.op = Op::ConstInt;
    inst.type = std::move(type);
    inst.intValue = value;
    return emit(std::move(inst));
  }

  int emitConst(const Expr& expr) {
    Inst inst;
    inst.span = expr.span;
    switch (expr.kind) {
      case ExprKind::IntLit:
        inst.op = Op::ConstInt;
        inst.intValue = static_cast<const IntLitExpr&>(expr).value;
        break;
      case ExprKind::FloatLit:
        inst.op = Op::ConstFloat;
        inst.floatValue = static_cast<const FloatLitExpr&>(expr).value;
        break;
      case ExprKind::BoolLit:
        inst.op = Op::ConstBool;
        inst.boolValue = static_cast<const BoolLitExpr&>(expr).value;
        break;
      case ExprKind::CharLit:
        inst.op = Op::ConstChar;
        inst.charValue = static_cast<const CharLitExpr&>(expr).value;
        break;
      default:
        inst.op = Op::ConstString;
        break;
    }
    inst.type = defaultType(expr.inferredType);
    return emit(std::move(inst));
  }

  int emitBinary(const std::string& op, int lhs, int rhs, TypePtr type, Span span) {
    Inst inst;
    inst.op = Op::Binary;
    inst.text = op;
    inst.type = std::move(type);
    inst.operands = {lhs, rhs};
    inst.span = span;
    return emit(std::move(inst));
  }

  TypePtr typeOf(int value) const {
    auto found = types_.find(value);
    return found == types_.end() ? nullptr : found->second;
  }

  int lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) return found->second;
    }
    return -1;
  }

  bool assignVar(const std::string& name, int value) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found == it->end()) continue;
      found->second = value;
      return true;
    }
    return false;
  }

  StringMap<int> snapshotEnv() const {
    StringMap<int> env;
    for (const auto& scope : scopes_) {
      for (const auto& entry : scope) env[entry.first] = entry.second;
    }
    return env;
  }

  void applyEnv(const StringMap<int>& env) {
    for (const auto& entry : env) assignVar(entry.first, entry.second);
  }

  void lowerStatements(const std::vector<StmtPtr>& body) {
    scopes_.emplace_back();
    for (const auto& stmt : body) {
      if (terminated_) break;
      lowerStmt(*stmt);
    }
    scopes_.pop_back();
  }

  void lowerStatementsRaw(const std::vector<StmtPtr>& body) {
    for (const auto& stmt : body) {
      if (terminated_) break;
      lowerStmt(*stmt);
    }
  }

  // ------------------------------------------------------- terminators

  void recordEdge(int target, const StringMap<int>& env, int extra = -1) {
    blockIncoming_[target].push_back(Edge{current_, env});
    for (auto& loop : loops_) {
      if (loop.header != target) continue;
      for (const auto& phi : loop.headerPhis) {
        int value = -1;
        if (phi.first.empty()) {
          value = extra;
        } else {
          lookupEnv(env, phi.first, value);
        }
        if (value < 0) continue;
        Inst& inst = instruction(phi.second);
        inst.operands.push_back(value);
        inst.incomingBlocks.push_back(current_);
      }
    }
  }

  void jumpTo(int target, const StringMap<int>& env, int extra = -1) {
    recordEdge(target, env, extra);
    Terminator term;
    term.kind = Terminator::Kind::Jump;
    term.target = target;
    block(current_).term = term;
    terminated_ = true;
  }

  void branchTo(int condition, int trueTarget, int falseTarget, const StringMap<int>& env) {
    recordEdge(trueTarget, env);
    recordEdge(falseTarget, env);
    Terminator term;
    term.kind = Terminator::Kind::Branch;
    term.target = trueTarget;
    term.falseTarget = falseTarget;
    term.value = condition;
    block(current_).term = term;
    terminated_ = true;
  }

  int pushPhi(int blockIndex, TypePtr type, int value, int from) {
    Inst inst;
    inst.op = Op::Phi;
    inst.type = std::move(type);
    inst.operands.push_back(value);
    inst.incomingBlocks.push_back(from);
    inst.id = fn_.nextValue++;
    defs_[inst.id] = {blockIndex, block(blockIndex).insts.size()};
    if (inst.type) types_[inst.id] = inst.type;
    block(blockIndex).insts.push_back(std::move(inst));
    return fn_.nextValue - 1;
  }

  // Creates the phis a join block needs given every incoming edge. Variables
  // that keep the same value on every edge are bound directly, without a phi.
  std::size_t materializeJoin(int blockIndex) {
    auto found = blockIncoming_.find(blockIndex);
    if (found == blockIncoming_.end() || found->second.empty()) return 0;
    const std::vector<Edge>& edges = found->second;
    std::size_t created = 0;
    if (edges.size() == 1) {
      applyEnv(edges.front().env);
      return 0;
    }
    std::vector<std::string> names;
    for (const auto& entry : edges.front().env) {
      bool present = true;
      for (std::size_t i = 1; i < edges.size(); ++i) {
        if (!edges[i].env.count(entry.first)) {
          present = false;
          break;
        }
      }
      if (present) names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      std::vector<int> values;
      values.reserve(edges.size());
      bool same = true;
      for (const auto& edge : edges) {
        int value = edge.env.at(name);
        if (!values.empty() && values.front() != value) same = false;
        values.push_back(value);
      }
      if (same) {
        assignVar(name, values.front());
        continue;
      }
      Inst phi;
      phi.op = Op::Phi;
      phi.type = defaultType(typeOf(values.front()));
      phi.id = fn_.nextValue++;
      defs_[phi.id] = {blockIndex, block(blockIndex).insts.size()};
      if (phi.type) types_[phi.id] = phi.type;
      for (std::size_t i = 0; i < edges.size(); ++i) {
        phi.operands.push_back(values[i]);
        phi.incomingBlocks.push_back(edges[i].from);
      }
      block(blockIndex).insts.push_back(std::move(phi));
      assignVar(name, fn_.nextValue - 1);
      ++created;
    }
    return created;
  }

  // ---------------------------------------------------------- statements

  void lowerStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let:
        lowerLet(static_cast<const LetStmt&>(stmt));
        return;
      case StmtKind::Assign:
        lowerAssign(static_cast<const AssignStmt&>(stmt));
        return;
      case StmtKind::Expr:
        (void)lowerExpr(*static_cast<const ExprStmt&>(stmt).expr);
        return;
      case StmtKind::Destructure:
        lowerDestructure(static_cast<const DestructureStmt&>(stmt));
        return;
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        int value = ret.value ? lowerExpr(*ret.value) : -1;
        Terminator term;
        term.kind = Terminator::Kind::Return;
        term.value = value;
        term.span = ret.span;
        block(current_).term = term;
        terminated_ = true;
        return;
      }
      case StmtKind::Break: {
        if (loops_.empty()) return;
        jumpTo(loops_.back().exit, snapshotEnv());
        return;
      }
      case StmtKind::Continue: {
        if (loops_.empty()) return;
        jumpTo(loops_.back().continueTarget, snapshotEnv());
        return;
      }
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(stmt);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "defer";
        inst.span = defer.span;
        (void)emit(std::move(inst));
        return;
      }
      case StmtKind::If:
        lowerIf(static_cast<const IfStmt&>(stmt));
        return;
      case StmtKind::While:
        lowerWhile(static_cast<const WhileStmt&>(stmt));
        return;
      case StmtKind::DoWhile:
        lowerDoWhile(static_cast<const DoWhileStmt&>(stmt));
        return;
      case StmtKind::For:
        lowerFor(static_cast<const ForStmt&>(stmt));
        return;
      case StmtKind::Loop:
        lowerLoop(static_cast<const LoopStmt&>(stmt));
        return;
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
    }
  }

  void lowerLet(const LetStmt& let) {
    int value = -1;
    if (let.init) {
      value = lowerExpr(*let.init);
    } else {
      Inst inst;
      inst.op = Op::Opaque;
      inst.text = "uninitialized";
      value = emit(std::move(inst));
    }
    scopes_.back()[let.name] = value;
  }

  void lowerAssign(const AssignStmt& assign) {
    if (assign.op == "=") {
      int value = lowerExpr(*assign.value);
      if (assign.target->kind == ExprKind::Ident) {
        const auto& ident = static_cast<const IdentExpr&>(*assign.target);
        if (!assignVar(ident.name, value)) scopes_.back()[ident.name] = value;
        return;
      }
      storeInto(*assign.target, value, assign.opSpan);
      return;
    }
    int current = lowerExpr(*assign.target);
    int value = lowerExpr(*assign.value);
    int result = emitBinary(assign.op.substr(0, assign.op.size() - 1), current, value,
                            defaultType(assign.target->inferredType), assign.opSpan);
    if (assign.target->kind == ExprKind::Ident) {
      assignVar(static_cast<const IdentExpr&>(*assign.target).name, result);
    } else {
      storeInto(*assign.target, result, assign.opSpan);
    }
  }

  void storeInto(const Expr& target, int value, Span span) {
    Inst inst;
    inst.span = span;
    if (target.kind == ExprKind::Index) {
      const auto& index = static_cast<const IndexExpr&>(target);
      inst.op = Op::StoreIndex;
      inst.operands = {lowerExpr(*index.object), lowerExpr(*index.index), value};
    } else if (target.kind == ExprKind::Field) {
      const auto& field = static_cast<const FieldExpr&>(target);
      inst.op = Op::StoreField;
      inst.text = field.field;
      inst.operands = {lowerExpr(*field.object), value};
    } else {
      inst.op = Op::Opaque;
      inst.text = "store";
      inst.operands = {value};
    }
    (void)emit(std::move(inst));
  }

  void lowerDestructure(const DestructureStmt& stmt) {
    int value = lowerExpr(*stmt.value);
    for (std::size_t i = 0; i < stmt.names.size(); ++i) {
      Inst inst;
      inst.op = Op::TupleGet;
      inst.intValue = static_cast<std::int64_t>(i);
      inst.operands = {value};
      int element = emit(std::move(inst));
      if (stmt.names[i] != "_") scopes_.back()[stmt.names[i]] = element;
    }
  }

  void lowerIf(const IfStmt& stmt) {
    auto saved = scopes_;
    int merge = addBlock(uniqueLabel("if.end"));
    int condBlock = current_;
    bool hasElse = !stmt.elseBody.empty();
    for (std::size_t i = 0; i < stmt.branches.size(); ++i) {
      current_ = condBlock;
      terminated_ = false;
      scopes_ = saved;
      int condition = lowerExpr(*stmt.branches[i].cond);
      int body = addBlock(uniqueLabel("if.then"));
      int next = i + 1 < stmt.branches.size()
                     ? addBlock(uniqueLabel("if.cond"))
                     : (hasElse ? addBlock(uniqueLabel("if.else")) : merge);
      branchTo(condition, body, next, snapshotEnv());

      current_ = body;
      terminated_ = false;
      scopes_ = saved;
      scopes_.emplace_back();
      lowerStatementsRaw(stmt.branches[i].body);
      scopes_.pop_back();
      if (!terminated_) jumpTo(merge, snapshotEnv());
      condBlock = next;
    }
    if (hasElse) {
      current_ = condBlock;
      terminated_ = false;
      scopes_ = saved;
      scopes_.emplace_back();
      lowerStatementsRaw(stmt.elseBody);
      scopes_.pop_back();
      if (!terminated_) jumpTo(merge, snapshotEnv());
    }
    scopes_ = saved;
    current_ = merge;
    terminated_ = false;
    materializeJoin(merge);
    if (blockIncoming_[merge].empty()) {
      block(merge).term = Terminator{};
      terminated_ = true;
    }
  }

  // Shared prologue for loops: create the header phis and return the
  // environment variables should see at the header.
  StringMap<int> createHeaderPhis(int header, int preheader,
                                  const std::vector<std::string>& names) {
    StringMap<int> headerEnv;
    const Edge& entry = blockIncoming_[header].front();
    for (const auto& name : names) {
      int initial = -1;
      if (!lookupEnv(entry.env, name, initial)) continue;
      int phi = pushPhi(header, defaultType(typeOf(initial)), initial, preheader);
      headerEnv[name] = phi;
    }
    return headerEnv;
  }

  std::vector<std::string> assignedNames(const std::vector<StmtPtr>& body) {
    std::set<std::string> names;
    collectAssigned(body, names);
    std::vector<std::string> out;
    for (const auto& name : names) {
      if (lookup(name) >= 0) out.push_back(name);
    }
    return out;
  }

  void lowerWhile(const WhileStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int cond = addBlock(uniqueLabel("while.cond"));
    int body = addBlock(uniqueLabel("while.body"));
    int exit = addBlock(uniqueLabel("while.exit"));
    jumpTo(cond, snapshotEnv());

    LoopContext loop;
    loop.header = cond;
    loop.exit = exit;
    loop.continueTarget = cond;
    StringMap<int> headerEnv = createHeaderPhis(cond, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = cond;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    int condition = lowerExpr(*stmt.cond);
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(cond, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

  void lowerDoWhile(const DoWhileStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int body = addBlock(uniqueLabel("do.body"));
    int cond = addBlock(uniqueLabel("do.cond"));
    int exit = addBlock(uniqueLabel("do.exit"));
    jumpTo(body, snapshotEnv());

    LoopContext loop;
    loop.header = body;
    loop.exit = exit;
    loop.continueTarget = cond;
    StringMap<int> headerEnv = createHeaderPhis(body, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(cond, snapshotEnv());

    current_ = cond;
    terminated_ = false;
    scopes_ = saved;
    materializeJoin(cond);
    int condition = lowerExpr(*stmt.cond);
    branchTo(condition, body, exit, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

  void lowerLoop(const LoopStmt& stmt) {
    auto saved = scopes_;
    const int preheader = current_;
    int header = addBlock(uniqueLabel("loop.header"));
    int body = addBlock(uniqueLabel("loop.body"));
    int exit = addBlock(uniqueLabel("loop.exit"));
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = header;
    StringMap<int> headerEnv = createHeaderPhis(header, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    jumpTo(body, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(header, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

  void lowerFor(const ForStmt& stmt) {
    if (stmt.iterable->kind == ExprKind::Range) {
      lowerForRange(stmt, static_cast<const RangeExpr&>(*stmt.iterable));
      return;
    }
    lowerForIterable(stmt);
  }

  void lowerForRange(const ForStmt& stmt, const RangeExpr& range) {
    auto saved = scopes_;
    int start = lowerExpr(*range.start);
    int end = lowerExpr(*range.end);
    int step = range.step ? lowerExpr(*range.step) : emitConstInt(1);

    int header = addBlock(uniqueLabel("for.cond"));
    int body = addBlock(uniqueLabel("for.body"));
    int latch = addBlock(uniqueLabel("for.latch"));
    int exit = addBlock(uniqueLabel("for.exit"));
    int preheader = current_;
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = latch;
    int index = pushPhi(header, intType(), start, preheader);
    loop.headerPhis.push_back({"", index});
    StringMap<int> headerEnv = createHeaderPhis(header, preheader, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    int condition = emitBinary("<", index, end, typeBool(), range.span);
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    scopes_.back()[stmt.var] = index;
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(latch, snapshotEnv());

    current_ = latch;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    materializeJoin(latch);
    int next = emitBinary("+", index, step, intType(), range.span);
    jumpTo(header, snapshotEnv(), next);
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

  TypePtr iterableElement(const TypePtr& type) {
    TypePtr resolved = defaultType(type);
    if (!resolved) return nullptr;
    switch (resolved->kind) {
      case TypeKind::Array:
      case TypeKind::Set:
      case TypeKind::Range:
        return resolved->elem;
      case TypeKind::Map:
        return resolved->elem;
      case TypeKind::String:
        return typeChar();
      default:
        return nullptr;
    }
  }

  void lowerForIterable(const ForStmt& stmt) {
    auto saved = scopes_;
    int iterable = lowerExpr(*stmt.iterable);
    Inst init;
    init.op = Op::IterInit;
    init.type = stmt.iterable->inferredType;
    init.operands = {iterable};
    int iterator = emit(std::move(init));

    int header = addBlock(uniqueLabel("for.cond"));
    int body = addBlock(uniqueLabel("for.body"));
    int exit = addBlock(uniqueLabel("for.exit"));
    jumpTo(header, snapshotEnv());

    LoopContext loop;
    loop.header = header;
    loop.exit = exit;
    loop.continueTarget = header;
    StringMap<int> headerEnv = createHeaderPhis(header, header, assignedNames(stmt.body));
    for (const auto& entry : headerEnv) {
      loop.headerPhis.push_back({entry.first, entry.second});
    }
    loops_.push_back(loop);

    current_ = header;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    Inst has;
    has.op = Op::IterHasNext;
    has.type = typeBool();
    has.operands = {iterator};
    int condition = emit(std::move(has));
    branchTo(condition, body, exit, snapshotEnv());

    current_ = body;
    terminated_ = false;
    scopes_ = saved;
    applyEnv(headerEnv);
    scopes_.emplace_back();
    Inst next;
    next.op = Op::IterNext;
    next.type = iterableElement(stmt.iterable->inferredType);
    next.operands = {iterator};
    scopes_.back()[stmt.var] = emit(std::move(next));
    lowerStatementsRaw(stmt.body);
    scopes_.pop_back();
    if (!terminated_) jumpTo(header, snapshotEnv());
    loops_.pop_back();

    scopes_ = saved;
    current_ = exit;
    terminated_ = false;
    materializeJoin(exit);
    if (blockIncoming_[exit].empty()) {
      block(exit).term = Terminator{};
      terminated_ = true;
    }
  }

  // --------------------------------------------------------- expressions

  int lowerExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::BoolLit:
      case ExprKind::CharLit:
        return emitConst(expr);
      case ExprKind::StringLit:
        return lowerString(static_cast<const StringLitExpr&>(expr));
      case ExprKind::Ident:
        return lowerIdent(static_cast<const IdentExpr&>(expr));
      case ExprKind::Unary:
        return lowerUnary(static_cast<const UnaryExpr&>(expr));
      case ExprKind::Binary:
        return lowerBinary(static_cast<const BinaryExpr&>(expr));
      case ExprKind::Call:
        return lowerCall(static_cast<const CallExpr&>(expr));
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        Inst inst;
        inst.op = Op::Index;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*index.object), lowerExpr(*index.index)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Slice: {
        const auto& slice = static_cast<const SliceExpr&>(expr);
        Inst inst;
        inst.op = Op::Slice;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*slice.object),
                         slice.start ? lowerExpr(*slice.start) : -1,
                         slice.end ? lowerExpr(*slice.end) : -1,
                         slice.step ? lowerExpr(*slice.step) : -1};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        Inst inst;
        inst.op = Op::Field;
        inst.type = defaultType(expr.inferredType);
        inst.text = field.field;
        inst.operands = {lowerExpr(*field.object)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::SafeNav: {
        const auto& nav = static_cast<const SafeNavExpr&>(expr);
        Inst inst;
        inst.op = Op::Field;
        inst.type = defaultType(expr.inferredType);
        inst.text = nav.field + "?";
        inst.operands = {lowerExpr(*nav.object)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::ArrayLit: {
        const auto& array = static_cast<const ArrayLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Array;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : array.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::Tuple: {
        const auto& tuple = static_cast<const TupleExpr&>(expr);
        Inst inst;
        inst.op = Op::Tuple;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : tuple.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::MapLit: {
        const auto& map = static_cast<const MapLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Map;
        inst.type = defaultType(expr.inferredType);
        for (const auto& entry : map.entries) {
          inst.operands.push_back(lowerExpr(*entry.first));
          inst.operands.push_back(lowerExpr(*entry.second));
        }
        return emit(std::move(inst));
      }
      case ExprKind::SetLit: {
        const auto& set = static_cast<const SetLitExpr&>(expr);
        Inst inst;
        inst.op = Op::Set;
        inst.type = defaultType(expr.inferredType);
        for (const auto& element : set.elements) inst.operands.push_back(lowerExpr(*element));
        return emit(std::move(inst));
      }
      case ExprKind::Range: {
        const auto& range = static_cast<const RangeExpr&>(expr);
        Inst inst;
        inst.op = Op::Range;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*range.start), lowerExpr(*range.end)};
        if (range.step) inst.operands.push_back(lowerExpr(*range.step));
        return emit(std::move(inst));
      }
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        Inst inst;
        inst.op = Op::Select;
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*ternary.cond), lowerExpr(*ternary.thenValue),
                         lowerExpr(*ternary.elseValue)};
        return emit(std::move(inst));
      }
      case ExprKind::Lambda: {
        Inst inst;
        inst.op = Op::Lambda;
        inst.type = defaultType(expr.inferredType);
        inst.text = "<lambda>";
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Comprehension: {
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "comprehension";
        inst.type = defaultType(expr.inferredType);
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Match: {
        const auto& match = static_cast<const MatchExpr&>(expr);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "match";
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*match.subject)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
      case ExprKind::Try: {
        const auto& tryExpr = static_cast<const TryExpr&>(expr);
        Inst inst;
        inst.op = Op::Opaque;
        inst.text = "try";
        inst.type = defaultType(expr.inferredType);
        inst.operands = {lowerExpr(*tryExpr.operand)};
        inst.span = expr.span;
        return emit(std::move(inst));
      }
    }
    Inst inst;
    inst.op = Op::Opaque;
    inst.text = "unsupported";
    return emit(std::move(inst));
  }

  int lowerString(const StringLitExpr& literal) {
    bool pure = true;
    for (const auto& part : literal.parts) {
      if (part.isExpr && part.expr) {
        pure = false;
        break;
      }
    }
    if (pure) {
      Inst inst;
      inst.op = Op::ConstString;
      inst.type = typeString();
      for (const auto& part : literal.parts) inst.stringValue += part.text;
      inst.span = literal.span;
      return emit(std::move(inst));
    }
    Inst concat;
    concat.op = Op::StringConcat;
    concat.type = typeString();
    concat.span = literal.span;
    for (const auto& part : literal.parts) {
      if (part.isExpr && part.expr) {
        concat.operands.push_back(lowerExpr(*part.expr));
      } else if (!part.text.empty()) {
        Inst text;
        text.op = Op::ConstString;
        text.type = typeString();
        text.stringValue = part.text;
        concat.operands.push_back(emit(std::move(text)));
      }
    }
    return emit(std::move(concat));
  }

  int lowerIdent(const IdentExpr& ident) {
    int value = lookup(ident.name);
    if (value >= 0) return value;
    Inst inst;
    inst.op = Op::Global;
    inst.type = defaultType(ident.inferredType);
    inst.text = ident.name;
    inst.span = ident.span;
    return emit(std::move(inst));
  }

  int lowerUnary(const UnaryExpr& unary) {
    if ((unary.op == "++" || unary.op == "--") && unary.operand->kind == ExprKind::Ident) {
      const auto& ident = static_cast<const IdentExpr&>(*unary.operand);
      int old = lowerIdent(ident);
      int one = emitConstInt(1, defaultType(unary.operand->inferredType));
      int updated = emitBinary(unary.op == "++" ? "+" : "-", old, one,
                               defaultType(unary.operand->inferredType), unary.opSpan);
      assignVar(ident.name, updated);
      return unary.postfix ? old : updated;
    }
    Inst inst;
    inst.op = Op::Unary;
    inst.text = unary.op;
    inst.type = defaultType(unary.inferredType);
    inst.operands = {lowerExpr(*unary.operand)};
    inst.span = unary.span;
    return emit(std::move(inst));
  }

  int lowerBinary(const BinaryExpr& binary) {
    if (binary.binop == BinOp::Range) {
      Inst inst;
      inst.op = Op::Range;
      inst.type = defaultType(binary.inferredType);
      inst.operands = {lowerExpr(*binary.lhs), lowerExpr(*binary.rhs)};
      return emit(std::move(inst));
    }
    // Short-circuit `and`/`or` are lowered as strict binary operations; the
    // core IR tracks data flow and loops, not operand evaluation order.
    int lhs = lowerExpr(*binary.lhs);
    int rhs = lowerExpr(*binary.rhs);
    return emitBinary(binary.op, lhs, rhs, defaultType(binary.inferredType), binary.opSpan);
  }

  int lowerCall(const CallExpr& call) {
    Inst inst;
    inst.span = call.span;
    inst.type = defaultType(call.inferredType);
    if (call.callee->kind == ExprKind::Ident) {
      const auto& callee = static_cast<const IdentExpr&>(*call.callee);
      if (lookup(callee.name) < 0) {
        inst.op = Op::Call;
        inst.text = callee.name;
      } else {
        inst.op = Op::CallValue;
        inst.operands.push_back(lowerExpr(*call.callee));
      }
    } else if (call.callee->kind == ExprKind::Field) {
      const auto& field = static_cast<const FieldExpr&>(*call.callee);
      inst.op = Op::Call;
      inst.text = "." + field.field;
      inst.operands.push_back(lowerExpr(*field.object));
    } else {
      inst.op = Op::CallValue;
      inst.operands.push_back(lowerExpr(*call.callee));
    }
    for (const auto& arg : call.args) inst.operands.push_back(lowerExpr(*arg.value));
    return emit(std::move(inst));
  }

  Function& fn_;
  std::vector<StringMap<int>> scopes_{{}};
  std::unordered_map<int, std::pair<int, std::size_t>> defs_;
  std::unordered_map<int, TypePtr> types_;
  std::unordered_map<int, std::vector<Edge>> blockIncoming_;
  std::vector<LoopContext> loops_;
  int current_ = 0;
  bool terminated_ = false;
  int labelCounter_ = 0;
};

std::string typeText(const TypePtr& type) {
  TypePtr resolved = defaultType(type);
  return resolved ? typeName(resolved) : "?";
}

std::string quoteText(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  out += '"';
  return out;
}

std::string floatText(double value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

void printInst(std::ostream& os, const Inst& inst) {
  if (inst.id >= 0) os << "%" << inst.id << " = ";
  switch (inst.op) {
    case Op::ConstInt: os << "const " << inst.intValue; break;
    case Op::ConstFloat: os << "const " << floatText(inst.floatValue); break;
    case Op::ConstBool: os << "const " << (inst.boolValue ? "true" : "false"); break;
    case Op::ConstChar: os << "const '" << utf8Encode(inst.charValue) << "'"; break;
    case Op::ConstString: os << "const " << quoteText(inst.stringValue); break;
    case Op::Param: os << "param " << inst.text; break;
    case Op::Phi: break;  // operands print in the phi-specific format below
    case Op::Copy: os << "copy"; break;
    case Op::Binary: os << inst.text; break;
    case Op::Unary: os << inst.text; break;
    case Op::Select: os << "select"; break;
    case Op::Call: os << "call " << inst.text; break;
    case Op::CallValue: os << "call_value"; break;
    case Op::Index: os << "index"; break;
    case Op::Slice: os << "slice"; break;
    case Op::Field: os << "field " << inst.text; break;
    case Op::Array: os << "array"; break;
    case Op::Tuple: os << "tuple"; break;
    case Op::Map: os << "map"; break;
    case Op::Set: os << "set"; break;
    case Op::Range: os << "range"; break;
    case Op::StringConcat: os << "concat"; break;
    case Op::IterInit: os << "iter_init"; break;
    case Op::IterHasNext: os << "iter_has_next"; break;
    case Op::IterNext: os << "iter_next"; break;
    case Op::TupleGet: os << "tuple_get " << inst.intValue; break;
    case Op::StoreIndex: os << "store_index"; break;
    case Op::StoreField: os << "store_field " << inst.text; break;
    case Op::Global: os << "global " << inst.text; break;
    case Op::FuncRef: os << "func " << inst.text; break;
    case Op::Lambda: os << "lambda"; break;
    case Op::Opaque: os << inst.text; break;
  }
  if (inst.op == Op::Phi) {
    os << "phi";
    for (std::size_t i = 0; i < inst.operands.size(); ++i) {
      if (i) os << ",";
      os << " [" << (inst.operands[i] >= 0 ? "%" + std::to_string(inst.operands[i]) : "-")
         << ", bb" << (i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1) << "]";
    }
  } else {
    for (std::size_t i = 0; i < inst.operands.size(); ++i) {
      if (i == 0) os << " ";
      else os << ", ";
      if (inst.operands[i] >= 0) os << "%" << inst.operands[i];
      else os << "_";
    }
  }
  if (inst.type) os << " : " << typeText(inst.type);
  os << "\n";
}

}  // namespace

const char* opName(Op op) {
  switch (op) {
    case Op::ConstInt: return "const";
    case Op::ConstFloat: return "const";
    case Op::ConstBool: return "const";
    case Op::ConstChar: return "const";
    case Op::ConstString: return "const";
    case Op::Param: return "param";
    case Op::Phi: return "phi";
    case Op::Copy: return "copy";
    case Op::Binary: return "binary";
    case Op::Unary: return "unary";
    case Op::Select: return "select";
    case Op::Call: return "call";
    case Op::CallValue: return "call_value";
    case Op::Index: return "index";
    case Op::Slice: return "slice";
    case Op::Field: return "field";
    case Op::Array: return "array";
    case Op::Tuple: return "tuple";
    case Op::Map: return "map";
    case Op::Set: return "set";
    case Op::Range: return "range";
    case Op::StringConcat: return "concat";
    case Op::IterInit: return "iter_init";
    case Op::IterHasNext: return "iter_has_next";
    case Op::IterNext: return "iter_next";
    case Op::TupleGet: return "tuple_get";
    case Op::StoreIndex: return "store_index";
    case Op::StoreField: return "store_field";
    case Op::Global: return "global";
    case Op::FuncRef: return "func";
    case Op::Lambda: return "lambda";
    case Op::Opaque: return "opaque";
  }
  return "?";
}

TypePtr defaultType(const TypePtr& type) {
  TypePtr resolved = resolveType(type);
  if (!resolved) return nullptr;
  if (resolved->kind != TypeKind::Var) return resolved;
  if (resolved->constraint == Constraint::Integer) return typeInt(64, true);
  if (resolved->constraint == Constraint::Floating) return typeFloat(64);
  return resolved;
}

std::string printFunction(const Function& function) {
  std::ostringstream os;
  os << "fn @" << function.name << "(";
  for (std::size_t i = 0; i < function.params.size(); ++i) {
    if (i) os << ", ";
    os << function.params[i].name << ": " << typeText(function.params[i].type);
  }
  os << ") -> " << typeText(function.returnType) << " {\n";
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    const Block& block = function.blocks[b];
    os << "bb" << b << " (" << block.label << "):\n";
    for (const auto& inst : block.insts) printInst(os, inst);
    switch (block.term.kind) {
      case Terminator::Kind::Jump:
        os << "  jump bb" << block.term.target << "\n";
        break;
      case Terminator::Kind::Branch:
        os << "  branch %" << block.term.value << ", bb" << block.term.target << ", bb"
           << block.term.falseTarget << "\n";
        break;
      case Terminator::Kind::Return:
        os << "  ret";
        if (block.term.value >= 0) os << " %" << block.term.value;
        os << "\n";
        break;
      case Terminator::Kind::Unreachable:
        os << "  unreachable\n";
        break;
    }
  }
  os << "}\n";
  return os.str();
}

std::string printProgram(const Program& program) {
  std::ostringstream os;
  for (std::size_t i = 0; i < program.functions.size(); ++i) {
    if (i) os << "\n";
    os << printFunction(program.functions[i]);
  }
  return os.str();
}

Program lowerProgram(const ModuleGraph& graph, const SemaResult& sema) {
  Program program;

  auto functionName = [&](const FnDecl& decl) {
    auto module = sema.fnModule.find(&decl);
    if (module == sema.fnModule.end()) return decl.name;
    return graph.at(module->second).name + "::" + decl.name;
  };

  for (const auto& module : graph.modules) {
    if (!module.program) continue;
    for (const auto& stmt : module.program->statements) {
      const auto* decl = dynamic_cast<const FnDecl*>(stmt.get());
      if (!decl) continue;
      Function function;
      function.name = functionName(*decl);
      function.decl = decl;
      auto params = sema.fnParamTypes.find(decl);
      for (std::size_t i = 0; i < decl->params.size(); ++i) {
        Parameter parameter;
        parameter.name = decl->params[i].name;
        if (params != sema.fnParamTypes.end() && i < params->second.size()) {
          parameter.type = defaultType(params->second[i]);
        }
        function.params.push_back(std::move(parameter));
      }
      auto ret = sema.fnReturnTypes.find(decl);
      function.returnType = ret != sema.fnReturnTypes.end() ? defaultType(ret->second) : nullptr;
      Builder builder(function);
      builder.lowerParameters();
      builder.lowerBody(decl->body);
      program.functions.push_back(std::move(function));
    }
  }

  if (graph.entry >= 0 && graph.at(graph.entry).program) {
    Function entry;
    entry.name = "main";
    entry.isEntry = true;
    entry.returnType = typeVoid();
    Builder builder(entry);
    builder.lowerParameters();
    builder.lowerTopLevel(graph.at(graph.entry).program->statements);
    program.functions.push_back(std::move(entry));
  }
  return program;
}

}  // namespace ir
}  // namespace a
