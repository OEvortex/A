#include "optimization/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace a {
namespace ir {
namespace {

bool isConstOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
      return true;
    default:
      return false;
  }
}

bool isPureOp(Op op) {
  switch (op) {
    case Op::ConstInt:
    case Op::ConstFloat:
    case Op::ConstBool:
    case Op::ConstChar:
    case Op::ConstString:
    case Op::Param:
    case Op::Copy:
    case Op::Binary:
    case Op::Unary:
    case Op::Select:
    case Op::TupleGet:
    case Op::Field:
    case Op::FuncRef:
      return true;
    default:
      return false;
  }
}

bool isEffectfulOp(Op op) {
  switch (op) {
    case Op::Call:
    case Op::CallValue:
    case Op::StoreIndex:
    case Op::StoreField:
    case Op::Opaque:
    case Op::IterInit:
    case Op::IterHasNext:
    case Op::IterNext:
      return true;
    default:
      return false;
  }
}

struct Const {
  Op op = Op::Opaque;
  std::int64_t i = 0;
  double f = 0.0;
  bool b = false;
  char32_t c = 0;
  std::string s;
};

bool sameConst(const Const& lhs, const Const& rhs) {
  if (lhs.op != rhs.op) return false;
  switch (lhs.op) {
    case Op::ConstInt: return lhs.i == rhs.i;
    case Op::ConstFloat: return lhs.f == rhs.f;
    case Op::ConstBool: return lhs.b == rhs.b;
    case Op::ConstChar: return lhs.c == rhs.c;
    case Op::ConstString: return lhs.s == rhs.s;
    default: return false;
  }
}

Const constOf(const Inst& inst) {
  Const value;
  value.op = inst.op;
  switch (inst.op) {
    case Op::ConstInt: value.i = inst.intValue; break;
    case Op::ConstFloat: value.f = inst.floatValue; break;
    case Op::ConstBool: value.b = inst.boolValue; break;
    case Op::ConstChar: value.c = inst.charValue; break;
    case Op::ConstString: value.s = inst.stringValue; break;
    default: break;
  }
  return value;
}

void applyConst(Inst& inst, const Const& value) {
  inst.op = value.op;
  inst.operands.clear();
  inst.incomingBlocks.clear();
  switch (value.op) {
    case Op::ConstInt: inst.intValue = value.i; break;
    case Op::ConstFloat: inst.floatValue = value.f; break;
    case Op::ConstBool: inst.boolValue = value.b; break;
    case Op::ConstChar: inst.charValue = value.c; break;
    case Op::ConstString: inst.stringValue = value.s; break;
    default: break;
  }
}

bool isInt(const Const& value) { return value.op == Op::ConstInt; }
bool isFloat(const Const& value) { return value.op == Op::ConstFloat; }
bool isBool(const Const& value) { return value.op == Op::ConstBool; }
bool isStr(const Const& value) { return value.op == Op::ConstString; }

Const makeInt(std::int64_t value) {
  Const out;
  out.op = Op::ConstInt;
  out.i = value;
  return out;
}

Const makeFloat(double value) {
  Const out;
  out.op = Op::ConstFloat;
  out.f = value;
  return out;
}

Const makeBool(bool value) {
  Const out;
  out.op = Op::ConstBool;
  out.b = value;
  return out;
}

Const makeStr(std::string value) {
  Const out;
  out.op = Op::ConstString;
  out.s = std::move(value);
  return out;
}

std::int64_t wrapAdd(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

std::int64_t wrapSub(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

std::int64_t wrapMul(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

bool compareInts(const std::string& op, std::int64_t a, std::int64_t b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

bool compareFloats(const std::string& op, double a, double b, bool& out) {
  if (op == "==") out = a == b;
  else if (op == "!=") out = a != b;
  else if (op == "<") out = a < b;
  else if (op == "<=") out = a <= b;
  else if (op == ">") out = a > b;
  else if (op == ">=") out = a >= b;
  else return false;
  return true;
}

// Returns false when the operation cannot be folded (for example integer
// division by zero or a non-constant operand).
bool foldBinary(const std::string& op, const Const& lhs, const Const& rhs, Const& out) {
  if (isInt(lhs) && isInt(rhs)) {
    const std::int64_t a = lhs.i;
    const std::int64_t b = rhs.i;
    if (op == "+") { out = makeInt(wrapAdd(a, b)); return true; }
    if (op == "-") { out = makeInt(wrapSub(a, b)); return true; }
    if (op == "*") { out = makeInt(wrapMul(a, b)); return true; }
    if (op == "/" || op == "//") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      std::int64_t value = a / b;
      if (op == "//" && ((a % b) != 0) && ((a < 0) != (b < 0))) --value;
      out = makeInt(value);
      return true;
    }
    if (op == "%") {
      if (b == 0 || (a == INT64_MIN && b == -1)) return false;
      out = makeInt(a % b);
      return true;
    }
    if (op == "&") { out = makeInt(a & b); return true; }
    if (op == "|") { out = makeInt(a | b); return true; }
    if (op == "^") { out = makeInt(a ^ b); return true; }
    if (op == "<<" || op == ">>") {
      if (b < 0 || b >= 64) return false;
      out = makeInt(op == "<<" ? static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << b)
                               : (a >> b));
      return true;
    }
    bool cmp = false;
    if (compareInts(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if ((isInt(lhs) || isFloat(lhs)) && (isInt(rhs) || isFloat(rhs))) {
    const double a = isInt(lhs) ? static_cast<double>(lhs.i) : lhs.f;
    const double b = isInt(rhs) ? static_cast<double>(rhs.i) : rhs.f;
    if (op == "+") { out = makeFloat(a + b); return true; }
    if (op == "-") { out = makeFloat(a - b); return true; }
    if (op == "*") { out = makeFloat(a * b); return true; }
    if (op == "/" || op == "//") {
      if (b == 0.0) return false;
      double value = a / b;
      if (op == "//") value = std::floor(value);
      out = makeFloat(value);
      return true;
    }
    if (op == "**") { out = makeFloat(std::pow(a, b)); return true; }
    bool cmp = false;
    if (compareFloats(op, a, b, cmp)) { out = makeBool(cmp); return true; }
    return false;
  }
  if (isBool(lhs) && isBool(rhs)) {
    if (op == "==") { out = makeBool(lhs.b == rhs.b); return true; }
    if (op == "!=") { out = makeBool(lhs.b != rhs.b); return true; }
    if (op == "and") { out = makeBool(lhs.b && rhs.b); return true; }
    if (op == "or") { out = makeBool(lhs.b || rhs.b); return true; }
    return false;
  }
  if (isStr(lhs) && isStr(rhs) && op == "+") {
    out = makeStr(lhs.s + rhs.s);
    return true;
  }
  return false;
}

bool foldUnary(const std::string& op, const Const& value, Const& out) {
  if (isInt(value)) {
    if (op == "-") { out = makeInt(wrapSub(0, value.i)); return true; }
    if (op == "+") { out = value; return true; }
    if (op == "~") { out = makeInt(~value.i); return true; }
    return false;
  }
  if (isFloat(value)) {
    if (op == "-") { out = makeFloat(-value.f); return true; }
    if (op == "+") { out = value; return true; }
    return false;
  }
  if (isBool(value)) {
    if (op == "!" || op == "not") { out = makeBool(!value.b); return true; }
    return false;
  }
  return false;
}

}  // namespace

OptimizationStats constantFold(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 100) {
      changed = false;
      std::unordered_map<int, Const> known;
      for (Block& block : function.blocks) {
        for (Inst& inst : block.insts) {
          if (isConstOp(inst.op)) {
            known[inst.id] = constOf(inst);
            continue;
          }
          if (inst.op == Op::Copy && inst.operands.size() == 1) {
            auto found = known.find(inst.operands[0]);
            if (found == known.end()) continue;
            applyConst(inst, found->second);
            known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Binary && inst.operands.size() == 2) {
            auto lhs = known.find(inst.operands[0]);
            auto rhs = known.find(inst.operands[1]);
            if (lhs == known.end() || rhs == known.end()) continue;
            Const folded;
            if (!foldBinary(inst.text, lhs->second, rhs->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Unary && inst.operands.size() == 1) {
            auto operand = known.find(inst.operands[0]);
            if (operand == known.end()) continue;
            Const folded;
            if (!foldUnary(inst.text, operand->second, folded)) continue;
            applyConst(inst, folded);
            known[inst.id] = folded;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Select && inst.operands.size() == 3) {
            auto cond = known.find(inst.operands[0]);
            if (cond == known.end() || !isBool(cond->second)) continue;
            int chosen = cond->second.b ? inst.operands[1] : inst.operands[2];
            Inst copy;
            copy.op = Op::Copy;
            copy.id = inst.id;
            copy.type = inst.type;
            copy.operands = {chosen};
            copy.span = inst.span;
            inst = std::move(copy);
            auto found = known.find(chosen);
            if (found != known.end()) known[inst.id] = found->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
          if (inst.op == Op::Phi && !inst.operands.empty()) {
            auto first = known.find(inst.operands.front());
            if (first == known.end()) continue;
            bool allConst = true;
            bool same = true;
            for (std::size_t i = 1; i < inst.operands.size(); ++i) {
              auto other = known.find(inst.operands[i]);
              if (other == known.end()) { allConst = false; break; }
              if (!sameConst(first->second, other->second)) same = false;
            }
            if (!allConst || !same) continue;
            applyConst(inst, first->second);
            known[inst.id] = first->second;
            changed = true;
            ++stats.constantsFolded;
            continue;
          }
        }
      }
    }
  }
  return stats;
}

OptimizationStats deadCodeElimination(Program& program) {
  OptimizationStats stats;

  // Remove blocks that are not reachable from the entry block.
  for (Function& function : program.functions) {
    if (function.blocks.empty()) continue;
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<int> worklist{0};
    reachable[0] = true;
    while (!worklist.empty()) {
      int index = worklist.back();
      worklist.pop_back();
      const Terminator& term = function.blocks[static_cast<std::size_t>(index)].term;
      auto visit = [&](int target) {
        if (target < 0 || static_cast<std::size_t>(target) >= function.blocks.size()) return;
        if (reachable[static_cast<std::size_t>(target)]) return;
        reachable[static_cast<std::size_t>(target)] = true;
        worklist.push_back(target);
      };
      if (term.kind == Terminator::Kind::Jump) visit(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        visit(term.target);
        visit(term.falseTarget);
      }
    }
    bool removedBlock = false;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      if (!reachable[b]) {
        removedBlock = true;
        ++stats.deadBlocks;
      }
    }
    if (removedBlock) {
      std::vector<int> remap(function.blocks.size(), -1);
      std::vector<Block> kept;
      kept.reserve(function.blocks.size());
      for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        remap[b] = static_cast<int>(kept.size());
        kept.push_back(std::move(function.blocks[b]));
      }
      function.blocks = std::move(kept);
      for (Block& block : function.blocks) {
        if (block.term.target >= 0) block.term.target = remap[block.term.target];
        if (block.term.falseTarget >= 0) block.term.falseTarget = remap[block.term.falseTarget];
        for (Inst& inst : block.insts) {
          if (inst.op != Op::Phi) continue;
          std::vector<int> operands;
          std::vector<int> incoming;
          for (std::size_t i = 0; i < inst.operands.size(); ++i) {
            int from = i < inst.incomingBlocks.size() ? inst.incomingBlocks[i] : -1;
            if (from < 0) continue;
            int mapped = remap[from];
            if (mapped < 0) continue;
            operands.push_back(inst.operands[i]);
            incoming.push_back(mapped);
          }
          inst.operands = std::move(operands);
          inst.incomingBlocks = std::move(incoming);
        }
      }
    }

    // Value-level DCE: mark everything reachable from effectful instructions,
    // terminators, and other roots.
    std::unordered_map<int, Inst*> defs;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) defs[inst.id] = &inst;
    }
    std::unordered_set<int> live;
    std::vector<int> liveWorklist;
    for (Block& block : function.blocks) {
      for (Inst& inst : block.insts) {
        if (isEffectfulOp(inst.op) && inst.producesValue()) {
          live.insert(inst.id);
          liveWorklist.push_back(inst.id);
        }
      }
      if (block.term.value >= 0 && live.insert(block.term.value).second) {
        liveWorklist.push_back(block.term.value);
      }
    }
    while (!liveWorklist.empty()) {
      int value = liveWorklist.back();
      liveWorklist.pop_back();
      auto found = defs.find(value);
      if (found == defs.end()) continue;
      for (int operand : found->second->operands) {
        if (operand >= 0 && live.insert(operand).second) liveWorklist.push_back(operand);
      }
    }
    for (Block& block : function.blocks) {
      std::vector<Inst> keptInsts;
      keptInsts.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        if (inst.producesValue() && live.count(inst.id) == 0) {
          ++stats.deadInstructions;
          continue;
        }
        keptInsts.push_back(std::move(inst));
      }
      block.insts = std::move(keptInsts);
    }
  }
  return stats;
}

namespace {

void replaceValue(Function& function, int from, int to) {
  if (from == to) return;
  for (Block& block : function.blocks) {
    for (Inst& inst : block.insts) {
      for (int& operand : inst.operands) {
        if (operand == from) operand = to;
      }
    }
    if (block.term.value == from) block.term.value = to;
  }
}

bool allPure(const Block& block) {
  for (const Inst& inst : block.insts) {
    if (inst.op == Op::Phi) return false;
    if (!isPureOp(inst.op)) return false;
    if (inst.op == Op::Binary && (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
      return false;
    }
  }
  return true;
}

}  // namespace

OptimizationStats inlineFunctions(Program& program) {
  OptimizationStats stats;
  std::unordered_map<std::string, const Function*> candidates;
  for (const Function& function : program.functions) {
    if (function.isEntry) continue;
    if (function.blocks.size() != 1) continue;
    const Block& block = function.blocks.front();
    if (block.term.kind != Terminator::Kind::Return) continue;
    if (!allPure(block)) continue;
    candidates.emplace(function.name, &function);
    if (function.decl) candidates.emplace(function.decl->name, &function);
  }
  if (candidates.empty()) return stats;

  for (Function& caller : program.functions) {
    for (Block& block : caller.blocks) {
      std::vector<Inst> rewritten;
      rewritten.reserve(block.insts.size());
      for (Inst& inst : block.insts) {
        auto candidate = inst.op == Op::Call ? candidates.find(inst.text) : candidates.end();
        if (candidate == candidates.end()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Function& callee = *candidate->second;
        if (inst.operands.size() != callee.params.size()) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        const Block& calleeBlock = callee.blocks.front();
        std::unordered_map<int, int> env;
        std::size_t paramIndex = 0;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param && paramIndex < inst.operands.size()) {
            env[calleeInst.id] = inst.operands[paramIndex++];
          }
        }
        bool ok = true;
        std::unordered_set<int> defined;
        for (const auto& entry : env) defined.insert(entry.first);
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          for (int operand : calleeInst.operands) {
            if (operand >= 0 && !defined.count(operand)) { ok = false; break; }
          }
          if (!ok) break;
          defined.insert(calleeInst.id);
        }
        if (!ok) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        int returnValue = calleeBlock.term.value;
        if (returnValue < 0) {
          if (inst.id >= 0) {
            // Void call: drop it entirely, its result is never used.
            ++stats.functionsInlined;
            continue;
          }
          rewritten.push_back(std::move(inst));
          continue;
        }
        int mappedReturn = env.count(returnValue) ? env[returnValue] : -1;
        if (mappedReturn >= 0) {
          replaceValue(caller, inst.id, mappedReturn);
          ++stats.functionsInlined;
          continue;
        }
        // Clone the callee body, reusing the call's value id for the returned
        // instruction so downstream users keep referring to the same value.
        std::vector<Inst> clones;
        int produced = -1;
        for (const Inst& calleeInst : calleeBlock.insts) {
          if (calleeInst.op == Op::Param) continue;
          Inst clone = calleeInst;
          for (int& operand : clone.operands) {
            if (operand >= 0) operand = env[operand];
          }
          int targetId = calleeInst.id == returnValue ? inst.id : caller.nextValue++;
          env[calleeInst.id] = targetId;
          clone.id = targetId;
          if (calleeInst.id == returnValue) produced = targetId;
          clones.push_back(std::move(clone));
        }
        if (produced < 0) {
          rewritten.push_back(std::move(inst));
          continue;
        }
        for (Inst& clone : clones) rewritten.push_back(std::move(clone));
        ++stats.functionsInlined;
      }
      block.insts = std::move(rewritten);
    }
  }
  return stats;
}

namespace {

std::vector<std::vector<int>> computePredecessors(const Function& function) {
  std::vector<std::vector<int>> preds(function.blocks.size());
  for (std::size_t b = 0; b < function.blocks.size(); ++b) {
    const Terminator& term = function.blocks[b].term;
    auto add = [&](int target) {
      if (target >= 0 && static_cast<std::size_t>(target) < preds.size()) {
        preds[static_cast<std::size_t>(target)].push_back(static_cast<int>(b));
      }
    };
    if (term.kind == Terminator::Kind::Jump) add(term.target);
    if (term.kind == Terminator::Kind::Branch) {
      add(term.target);
      add(term.falseTarget);
    }
  }
  return preds;
}

std::vector<std::unordered_set<int>> computeDominators(const Function& function,
                                                       const std::vector<std::vector<int>>& preds) {
  const std::size_t count = function.blocks.size();
  std::vector<std::unordered_set<int>> dom(count);
  std::unordered_set<int> all;
  for (std::size_t b = 0; b < count; ++b) all.insert(static_cast<int>(b));
  for (std::size_t b = 0; b < count; ++b) {
    dom[b] = b == 0 ? std::unordered_set<int>{0} : all;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t b = 1; b < count; ++b) {
      std::unordered_set<int> next;
      bool first = true;
      for (int pred : preds[b]) {
        if (first) {
          next = dom[static_cast<std::size_t>(pred)];
          first = false;
        } else {
          for (auto it = next.begin(); it != next.end();) {
            if (!dom[static_cast<std::size_t>(pred)].count(*it)) it = next.erase(it);
            else ++it;
          }
        }
      }
      if (first) next.clear();
      next.insert(static_cast<int>(b));
      if (next != dom[b]) {
        dom[b] = std::move(next);
        changed = true;
      }
    }
  }
  return dom;
}

}  // namespace

OptimizationStats loopInvariantCodeMotion(Program& program) {
  OptimizationStats stats;
  for (Function& function : program.functions) {
    if (function.blocks.size() < 2) continue;
    const auto preds = computePredecessors(function);
    const auto dom = computeDominators(function, preds);
    std::unordered_map<int, int> defBlock;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      for (const Inst& inst : function.blocks[b].insts) defBlock[inst.id] = static_cast<int>(b);
    }

    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
      const Terminator& term = function.blocks[b].term;
      std::vector<int> headers;
      if (term.kind == Terminator::Kind::Jump) headers.push_back(term.target);
      if (term.kind == Terminator::Kind::Branch) {
        headers.push_back(term.target);
        headers.push_back(term.falseTarget);
      }
      for (int header : headers) {
        if (header < 0 || static_cast<std::size_t>(header) >= function.blocks.size()) continue;
        if (!dom[b].count(header)) continue;  // not a back edge
        // Collect the natural loop body: reverse reachability from the latch
        // back to the header.
        std::unordered_set<int> body;
        body.insert(header);
        std::vector<int> worklist{static_cast<int>(b)};
        while (!worklist.empty()) {
          int node = worklist.back();
          worklist.pop_back();
          if (!body.insert(node).second) continue;
          if (node == header) continue;
          for (int pred : preds[static_cast<std::size_t>(node)]) worklist.push_back(pred);
        }
        // The preheader must be a unique non-loop predecessor that only jumps
        // into the header.
        int preheader = -1;
        for (int pred : preds[static_cast<std::size_t>(header)]) {
          if (body.count(pred)) continue;
          if (preheader >= 0) { preheader = -1; break; }
          preheader = pred;
        }
        if (preheader < 0) continue;
        const Terminator& preTerm = function.blocks[static_cast<std::size_t>(preheader)].term;
        if (preTerm.kind != Terminator::Kind::Jump || preTerm.target != header) continue;

        std::vector<int> blockOrder(body.begin(), body.end());
        std::sort(blockOrder.begin(), blockOrder.end());
        for (int loopBlock : blockOrder) {
          Block& block = function.blocks[static_cast<std::size_t>(loopBlock)];
          std::vector<Inst> kept;
          kept.reserve(block.insts.size());
          for (Inst& inst : block.insts) {
            if (inst.op == Op::Phi || !isPureOp(inst.op) || !inst.producesValue()) {
              kept.push_back(std::move(inst));
              continue;
            }
            if (inst.op == Op::Binary &&
                (inst.text == "/" || inst.text == "//" || inst.text == "%")) {
              kept.push_back(std::move(inst));
              continue;
            }
            bool invariant = true;
            for (int operand : inst.operands) {
              if (operand < 0) continue;
              auto found = defBlock.find(operand);
              if (found == defBlock.end()) { invariant = false; break; }
              int operandBlock = found->second;
              if (body.count(operandBlock)) { invariant = false; break; }
              if (operandBlock != preheader &&
                  !dom[static_cast<std::size_t>(preheader)].count(operandBlock)) {
                invariant = false;
                break;
              }
            }
            if (!invariant) {
              kept.push_back(std::move(inst));
              continue;
            }
            defBlock[inst.id] = preheader;
            function.blocks[static_cast<std::size_t>(preheader)].insts.push_back(
                std::move(inst));
            ++stats.loopsHoisted;
          }
          block.insts = std::move(kept);
        }
      }
    }
  }
  return stats;
}

OptimizationStats runAll(Program& program) {
  OptimizationStats total;
  for (int round = 0; round < 8; ++round) {
    OptimizationStats pass;
    pass.merge(constantFold(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(inlineFunctions(program));
    pass.merge(constantFold(program));
    pass.merge(deadCodeElimination(program));
    pass.merge(loopInvariantCodeMotion(program));
    pass.merge(constantFold(program));
    pass.merge(deadCodeElimination(program));
    total.merge(pass);
    if (pass.constantsFolded == 0 && pass.deadInstructions == 0 && pass.deadBlocks == 0 &&
        pass.functionsInlined == 0 && pass.loopsHoisted == 0) {
      break;
    }
  }
  return total;
}

}  // namespace ir
}  // namespace a
