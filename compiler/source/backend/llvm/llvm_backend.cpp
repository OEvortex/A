#include "backend/llvm/llvm_backend.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

#include "name-resolution/native_modules.hpp"

namespace a {
namespace {

std::string extractString(const StringLitExpr& s) {
  std::string result;
  for (const auto& p : s.parts) {
    if (!p.isExpr) {
      result += p.text;
    }
  }
  return result;
}

// ============================================================================
// LLVM IR Textual Emitter
// ============================================================================
class LLVMEmitter {
 public:
  LLVMEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    emitTargetHeader();
    emitDeclarations();

    // Collect global string literals
    collectStringLiterals();
    emitStringConstants();

    // Emit user function definitions
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit main entrypoint
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int tempCounter_ = 1;
  int labelCounter_ = 1;
  int strCounter_ = 1;
  std::unordered_map<std::string, std::string> locals_;
  std::unordered_map<std::string, std::string> strTable_;

  std::string nextTemp() {
    return "%t" + std::to_string(tempCounter_++);
  }

  std::string nextLabel(const std::string& prefix = "bb") {
    return prefix + std::to_string(labelCounter_++);
  }

  void collectStringLiterals() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        collectFromStmt(*stmt);
      }
    }
  }

  void collectFromStmt(const Stmt& stmt) {
    if (stmt.kind == StmtKind::Expr) {
      collectFromExpr(*static_cast<const ExprStmt&>(stmt).expr);
    } else if (stmt.kind == StmtKind::Let) {
      const auto& let = static_cast<const LetStmt&>(stmt);
      if (let.init) collectFromExpr(*let.init);
    } else if (stmt.kind == StmtKind::Assign) {
      const auto& a = static_cast<const AssignStmt&>(stmt);
      if (a.value) collectFromExpr(*a.value);
    } else if (stmt.kind == StmtKind::FnDecl) {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      for (const auto& s : fn.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::If) {
      const auto& ifs = static_cast<const IfStmt&>(stmt);
      for (const auto& br : ifs.branches) {
        if (br.cond) collectFromExpr(*br.cond);
        for (const auto& s : br.body) collectFromStmt(*s);
      }
      for (const auto& s : ifs.elseBody) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::While) {
      const auto& wh = static_cast<const WhileStmt&>(stmt);
      if (wh.cond) collectFromExpr(*wh.cond);
      for (const auto& s : wh.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::For) {
      const auto& fo = static_cast<const ForStmt&>(stmt);
      if (fo.iterable) collectFromExpr(*fo.iterable);
      for (const auto& s : fo.body) collectFromStmt(*s);
    }
  }

  void collectFromExpr(const Expr& expr) {
    if (expr.kind == ExprKind::StringLit) {
      std::string s = extractString(static_cast<const StringLitExpr&>(expr));
      if (strTable_.find(s) == strTable_.end()) {
        std::string name = "@.str." + std::to_string(strCounter_++);
        strTable_[s] = name;
      }
    } else if (expr.kind == ExprKind::Binary) {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      collectFromExpr(*b.lhs);
      collectFromExpr(*b.rhs);
    } else if (expr.kind == ExprKind::Call) {
      const auto& c = static_cast<const CallExpr&>(expr);
      collectFromExpr(*c.callee);
      for (const auto& a : c.args) collectFromExpr(*a.value);
    }
  }

  void emitTargetHeader() {
    out_ << "; ModuleID = 'a_program'\n";
    out_ << "source_filename = \"a_program.a\"\n";
    out_ << "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n";
    out_ << "target triple = \"x86_64-pc-linux-gnu\"\n\n";

    // Format strings for standard runtime printing
    out_ << "@.fmt_int = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\", align 1\n";
    out_ << "@.fmt_float = private unnamed_addr constant [6 x i8] c\"%.2f\\0A\\00\", align 1\n";
    out_ << "@.fmt_str = private unnamed_addr constant [4 x i8] c\"%s\\0A\\00\", align 1\n\n";
  }

  void emitStringConstants() {
    for (const auto& pair : strTable_) {
      const std::string& str = pair.first;
      const std::string& name = pair.second;
      std::size_t len = str.size() + 1;
      out_ << name << " = private unnamed_addr constant [" << len << " x i8] c\"";
      for (char c : str) {
        if (c == '\n') out_ << "\\0A";
        else if (c == '\t') out_ << "\\09";
        else if (c == '\"') out_ << "\\22";
        else if (c == '\\') out_ << "\\5C";
        else out_ << c;
      }
      out_ << "\\00\", align 1\n";
    }
    if (!strTable_.empty()) out_ << "\n";
  }

  void emitDeclarations() {
    out_ << "declare i32 @printf(ptr, ...)\n";
    out_ << "declare double @sqrt(double)\n";
    out_ << "declare double @round(double)\n";
    out_ << "declare double @pow(double, double)\n\n";
  }

  std::string mangle(const std::string& name) {
    if (name == "main") return "_a_main";
    return "_a_" + name;
  }

  void emitFunction(const FnDecl& fn) {
    locals_.clear();
    tempCounter_ = 1;
    labelCounter_ = 1;

    out_ << "define i64 @" << mangle(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i > 0) out_ << ", ";
      out_ << "i64 %arg_" << fn.params[i].name;
    }
    out_ << ") {\n";
    out_ << "entry:\n";

    // Stack space for parameters
    for (const auto& param : fn.params) {
      std::string ptr = nextTemp();
      out_ << "  " << ptr << " = alloca i64, align 8\n";
      out_ << "  store i64 %arg_" << param.name << ", ptr " << ptr << ", align 8\n";
      locals_[param.name] = ptr;
    }

    for (const auto& stmt : fn.body) {
      emitStmt(*stmt);
    }

    out_ << "  ret i64 0\n";
    out_ << "}\n\n";
  }

  void emitStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        std::string ptr = nextTemp();
        out_ << "  " << ptr << " = alloca i64, align 8\n";
        locals_[let.name] = ptr;
        if (let.init) {
          std::string val = emitExpr(*let.init);
          out_ << "  store i64 " << val << ", ptr " << ptr << ", align 8\n";
        }
        break;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        if (assign.target->kind == ExprKind::Ident) {
          const auto& id = static_cast<const IdentExpr&>(*assign.target);
          if (locals_.find(id.name) == locals_.end()) {
            std::string ptr = nextTemp();
            out_ << "  " << ptr << " = alloca i64, align 8\n";
            locals_[id.name] = ptr;
          }
          std::string val = emitExpr(*assign.value);
          if (assign.op == "=") {
            out_ << "  store i64 " << val << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "+=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string sum = nextTemp();
            out_ << "  " << sum << " = add nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << sum << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "-=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string diff = nextTemp();
            out_ << "  " << diff << " = sub nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << diff << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "*=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string prod = nextTemp();
            out_ << "  " << prod << " = mul nsw i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << prod << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "/=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string quot = nextTemp();
            out_ << "  " << quot << " = sdiv i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << quot << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "%=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string rem = nextTemp();
            out_ << "  " << rem << " = srem i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << rem << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "&=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = and i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "|=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = or i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "^=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = xor i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == "<<=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = shl i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          } else if (assign.op == ">>=") {
            std::string cur = nextTemp();
            out_ << "  " << cur << " = load i64, ptr " << locals_[id.name] << ", align 8\n";
            std::string res = nextTemp();
            out_ << "  " << res << " = ashr i64 " << cur << ", " << val << "\n";
            out_ << "  store i64 " << res << ", ptr " << locals_[id.name] << ", align 8\n";
          }
        }
        break;
      }
      case StmtKind::If: {
        const auto& ifStmt = static_cast<const IfStmt&>(stmt);
        if (ifStmt.branches.empty()) break;
        std::string endLbl = nextLabel("if.end");

        for (std::size_t i = 0; i < ifStmt.branches.size(); ++i) {
          const auto& br = ifStmt.branches[i];
          std::string condVal = emitExpr(*br.cond);
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp ne i64 " << condVal << ", 0\n";

          std::string thenLbl = nextLabel("if.then");
          std::string nextArmLbl = (i + 1 < ifStmt.branches.size())
                                       ? nextLabel("if.branch")
                                       : (!ifStmt.elseBody.empty() ? nextLabel("if.else") : endLbl);

          out_ << "  br i1 " << cmp << ", label %" << thenLbl << ", label %" << nextArmLbl << "\n\n";

          out_ << thenLbl << ":\n";
          for (const auto& s : br.body) emitStmt(*s);
          out_ << "  br label %" << endLbl << "\n\n";

          if (i + 1 < ifStmt.branches.size() || !ifStmt.elseBody.empty()) {
            out_ << nextArmLbl << ":\n";
          }
        }

        if (!ifStmt.elseBody.empty()) {
          for (const auto& s : ifStmt.elseBody) emitStmt(*s);
          out_ << "  br label %" << endLbl << "\n\n";
        }

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::While: {
        const auto& whileStmt = static_cast<const WhileStmt&>(stmt);
        std::string condLbl = nextLabel("while.cond");
        std::string bodyLbl = nextLabel("while.body");
        std::string endLbl = nextLabel("while.end");

        out_ << "  br label %" << condLbl << "\n\n";
        out_ << condLbl << ":\n";
        std::string condVal = emitExpr(*whileStmt.cond);
        std::string cmp = nextTemp();
        out_ << "  " << cmp << " = icmp ne i64 " << condVal << ", 0\n";
        out_ << "  br i1 " << cmp << ", label %" << bodyLbl << ", label %" << endLbl << "\n\n";

        out_ << bodyLbl << ":\n";
        for (const auto& s : whileStmt.body) emitStmt(*s);
        out_ << "  br label %" << condLbl << "\n\n";

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::For: {
        const auto& forStmt = static_cast<const ForStmt&>(stmt);
        if (forStmt.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*forStmt.iterable);
          std::string startVal = emitExpr(*range.start);
          std::string endVal = emitExpr(*range.end);
          
          std::string varPtr = nextTemp();
          out_ << "  " << varPtr << " = alloca i64, align 8\n";
          out_ << "  store i64 " << startVal << ", ptr " << varPtr << ", align 8\n";
          locals_[forStmt.var] = varPtr;

          std::string condLabel = nextLabel("for.cond");
          std::string bodyLabel = nextLabel("for.body");
          std::string incLabel = nextLabel("for.inc");
          std::string endLabel = nextLabel("for.end");

          out_ << "  br label %" << condLabel << "\n\n";

          out_ << condLabel << ":\n";
          std::string curVal = nextTemp();
          out_ << "  " << curVal << " = load i64, ptr " << varPtr << ", align 8\n";
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp slt i64 " << curVal << ", " << endVal << "\n";
          out_ << "  br i1 " << cmp << ", label %" << bodyLabel << ", label %" << endLabel << "\n\n";

          out_ << bodyLabel << ":\n";
          for (const auto& s : forStmt.body) emitStmt(*s);
          out_ << "  br label %" << incLabel << "\n\n";

          out_ << incLabel << ":\n";
          std::string cur2 = nextTemp();
          out_ << "  " << cur2 << " = load i64, ptr " << varPtr << ", align 8\n";
          std::string next = nextTemp();
          out_ << "  " << next << " = add nsw i64 " << cur2 << ", 1\n";
          out_ << "  store i64 " << next << ", ptr " << varPtr << ", align 8\n";
          out_ << "  br label %" << condLabel << "\n\n";

          out_ << endLabel << ":\n";
        }
        break;
      }
      case StmtKind::Expr: {
        const auto& exprStmt = static_cast<const ExprStmt&>(stmt);
        emitExpr(*exprStmt.expr);
        break;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
          std::string v = emitExpr(*ret.value);
          out_ << "  ret i64 " << v << "\n";
        } else {
          out_ << "  ret i64 0\n";
        }
        break;
      }
      default:
        break;
    }
  }

  std::string emitExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
        return std::to_string(static_cast<const IntLitExpr&>(expr).value);
      case ExprKind::BoolLit:
        return static_cast<const BoolLitExpr&>(expr).value ? "1" : "0";
      case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(expr);
        auto it = locals_.find(id.name);
        if (it != locals_.end()) {
          std::string val = nextTemp();
          out_ << "  " << val << " = load i64, ptr " << it->second << ", align 8\n";
          return val;
        }
        return "0";
      }
      case ExprKind::Binary: {
        const auto& bin = static_cast<const BinaryExpr&>(expr);
        std::string l = emitExpr(*bin.lhs);
        std::string r = emitExpr(*bin.rhs);
        std::string res = nextTemp();
        if (bin.op == "+") {
          out_ << "  " << res << " = add nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "-") {
          out_ << "  " << res << " = sub nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "*") {
          out_ << "  " << res << " = mul nsw i64 " << l << ", " << r << "\n";
        } else if (bin.op == "/") {
          out_ << "  " << res << " = sdiv i64 " << l << ", " << r << "\n";
        } else if (bin.op == "%") {
          out_ << "  " << res << " = srem i64 " << l << ", " << r << "\n";
        } else if (bin.op == "==") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp eq i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "!=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp ne i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "<") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp slt i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "<=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sle i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == ">") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sgt i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == ">=") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp sge i64 " << l << ", " << r << "\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (bin.op == "and" || bin.op == "&") {
          out_ << "  " << res << " = and i64 " << l << ", " << r << "\n";
        } else if (bin.op == "or" || bin.op == "|") {
          out_ << "  " << res << " = or i64 " << l << ", " << r << "\n";
        } else if (bin.op == "^") {
          out_ << "  " << res << " = xor i64 " << l << ", " << r << "\n";
        } else if (bin.op == "<<") {
          out_ << "  " << res << " = shl i64 " << l << ", " << r << "\n";
        } else if (bin.op == ">>") {
          out_ << "  " << res << " = ashr i64 " << l << ", " << r << "\n";
        } else {
          out_ << "  " << res << " = add i64 " << l << ", " << r << "\n";
        }
        return res;
      }
      case ExprKind::Unary: {
        const auto& un = static_cast<const UnaryExpr&>(expr);
        std::string operand = emitExpr(*un.operand);
        std::string res = nextTemp();
        if (un.op == "-") {
          out_ << "  " << res << " = sub nsw i64 0, " << operand << "\n";
        } else if (un.op == "not" || un.op == "!") {
          std::string cmp = nextTemp();
          out_ << "  " << cmp << " = icmp eq i64 " << operand << ", 0\n";
          out_ << "  " << res << " = zext i1 " << cmp << " to i64\n";
        } else if (un.op == "~") {
          out_ << "  " << res << " = xor i64 " << operand << ", -1\n";
        }
        return res;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*call.callee).name;
          if (name == "print") {
            if (!call.args.empty()) {
              const auto& argExpr = *call.args[0].value;
              if (argExpr.kind == ExprKind::StringLit) {
                std::string s = extractString(static_cast<const StringLitExpr&>(argExpr));
                std::string strSym = strTable_[s];
                std::string callRes = nextTemp();
                out_ << "  " << callRes << " = call i32 (ptr, ...) @printf(ptr @.fmt_str, ptr " << strSym << ")\n";
                return "0";
              }
              std::string arg = emitExpr(argExpr);
              std::string callRes = nextTemp();
              out_ << "  " << callRes << " = call i32 (ptr, ...) @printf(ptr @.fmt_int, i64 " << arg << ")\n";
              return "0";
            }
          }
          // General user function call
          std::vector<std::string> args;
          for (const auto& a : call.args) args.push_back(emitExpr(*a.value));
          std::string callRes = nextTemp();
          out_ << "  " << callRes << " = call i64 @" << mangle(name) << "(";
          for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) out_ << ", ";
            out_ << "i64 " << args[i];
          }
          out_ << ")\n";
          return callRes;
        }
        return "0";
      }
      default:
        return "0";
    }
  }

  void emitMain() {
    out_ << "define i32 @main(i32 %argc, ptr %argv) {\n";
    out_ << "entry:\n";
    if (graph_.entry >= 0 && static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) {
            emitStmt(*stmt);
          }
        }
      }
    }
    out_ << "  ret i32 0\n";
    out_ << "}\n";
  }
};

// ============================================================================
// Direct GNU x86-64 Assembly Emitter
// ============================================================================
class X86Emitter {
 public:
  X86Emitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    collectStringLiterals();

    out_ << ".intel_syntax noprefix\n";
    out_ << ".text\n\n";

    // Format strings
    out_ << ".section .rodata\n";
    out_ << ".Lfmt_int:\n  .asciz \"%lld\\n\"\n";
    out_ << ".Lfmt_str:\n  .asciz \"%s\\n\"\n";

    for (const auto& pair : strTable_) {
      out_ << pair.second << ":\n  .asciz \"";
      for (char c : pair.first) {
        if (c == '\n') out_ << "\\n";
        else if (c == '\t') out_ << "\\t";
        else if (c == '\"') out_ << "\\\"";
        else if (c == '\\') out_ << "\\\\";
        else out_ << c;
      }
      out_ << "\"\n";
    }

    out_ << "\n.text\n";

    // Emit functions
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit main
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int labelCounter_ = 1;
  int strCounter_ = 1;
  int currentStackOffset_ = 0;
  std::unordered_map<std::string, int> stackSlots_;
  std::unordered_map<std::string, std::string> strTable_;

  std::string nextLabel(const std::string& prefix = "L") {
    return "." + prefix + "_" + std::to_string(labelCounter_++);
  }

  void collectStringLiterals() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        collectFromStmt(*stmt);
      }
    }
  }

  void collectFromStmt(const Stmt& stmt) {
    if (stmt.kind == StmtKind::Expr) {
      collectFromExpr(*static_cast<const ExprStmt&>(stmt).expr);
    } else if (stmt.kind == StmtKind::Let) {
      const auto& let = static_cast<const LetStmt&>(stmt);
      if (let.init) collectFromExpr(*let.init);
    } else if (stmt.kind == StmtKind::Assign) {
      const auto& a = static_cast<const AssignStmt&>(stmt);
      if (a.value) collectFromExpr(*a.value);
    } else if (stmt.kind == StmtKind::FnDecl) {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      for (const auto& s : fn.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::If) {
      const auto& ifs = static_cast<const IfStmt&>(stmt);
      for (const auto& br : ifs.branches) {
        if (br.cond) collectFromExpr(*br.cond);
        for (const auto& s : br.body) collectFromStmt(*s);
      }
      for (const auto& s : ifs.elseBody) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::While) {
      const auto& wh = static_cast<const WhileStmt&>(stmt);
      if (wh.cond) collectFromExpr(*wh.cond);
      for (const auto& s : wh.body) collectFromStmt(*s);
    } else if (stmt.kind == StmtKind::For) {
      const auto& fo = static_cast<const ForStmt&>(stmt);
      if (fo.iterable) collectFromExpr(*fo.iterable);
      for (const auto& s : fo.body) collectFromStmt(*s);
    }
  }

  void collectFromExpr(const Expr& expr) {
    if (expr.kind == ExprKind::StringLit) {
      std::string s = extractString(static_cast<const StringLitExpr&>(expr));
      if (strTable_.find(s) == strTable_.end()) {
        strTable_[s] = ".Lstr_" + std::to_string(strCounter_++);
      }
    } else if (expr.kind == ExprKind::Call) {
      const auto& c = static_cast<const CallExpr&>(expr);
      for (const auto& a : c.args) collectFromExpr(*a.value);
    }
  }

  int allocateSlot(const std::string& name) {
    auto it = stackSlots_.find(name);
    if (it != stackSlots_.end()) return it->second;
    currentStackOffset_ += 8;
    stackSlots_[name] = currentStackOffset_;
    return currentStackOffset_;
  }

  void emitFunction(const FnDecl& fn) {
    stackSlots_.clear();
    currentStackOffset_ = 0;

    std::string fnLabel = fn.name == "main" ? "_a_main" : "_a_" + fn.name;
    out_ << ".globl " << fnLabel << "\n";
    out_ << ".type " << fnLabel << ", @function\n";
    out_ << fnLabel << ":\n";
    out_ << "  push rbp\n";
    out_ << "  mov rbp, rsp\n";
    out_ << "  sub rsp, 256\n";  // Reserve stack frame

    static const char* kArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    for (std::size_t i = 0; i < fn.params.size() && i < 6; ++i) {
      int slot = allocateSlot(fn.params[i].name);
      out_ << "  mov [rbp - " << slot << "], " << kArgRegs[i] << "\n";
    }

    for (const auto& s : fn.body) emitStmt(*s);

    out_ << "  mov rax, 0\n";
    out_ << "  mov rsp, rbp\n";
    out_ << "  pop rbp\n";
    out_ << "  ret\n\n";
  }

  void emitStmt(const Stmt& stmt) {
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        int slot = allocateSlot(let.name);
        if (let.init) {
          emitExpr(*let.init);
          out_ << "  mov [rbp - " << slot << "], rax\n";
        }
        break;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        if (assign.target->kind == ExprKind::Ident) {
          const auto& id = static_cast<const IdentExpr&>(*assign.target);
          int slot = allocateSlot(id.name);
          emitExpr(*assign.value);
          if (assign.op == "=") {
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "+=") {
            out_ << "  add [rbp - " << slot << "], rax\n";
          } else if (assign.op == "-=") {
            out_ << "  sub [rbp - " << slot << "], rax\n";
          } else if (assign.op == "*=") {
            out_ << "  mov rcx, [rbp - " << slot << "]\n";
            out_ << "  imul rax, rcx\n";
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "/=") {
            out_ << "  mov rbx, rax\n";
            out_ << "  mov rax, [rbp - " << slot << "]\n";
            out_ << "  cqo\n";
            out_ << "  idiv rbx\n";
            out_ << "  mov [rbp - " << slot << "], rax\n";
          } else if (assign.op == "%=") {
            out_ << "  mov rbx, rax\n";
            out_ << "  mov rax, [rbp - " << slot << "]\n";
            out_ << "  cqo\n";
            out_ << "  idiv rbx\n";
            out_ << "  mov [rbp - " << slot << "], rdx\n";
          } else if (assign.op == "&=") {
            out_ << "  and [rbp - " << slot << "], rax\n";
          } else if (assign.op == "|=") {
            out_ << "  or [rbp - " << slot << "], rax\n";
          } else if (assign.op == "^=") {
            out_ << "  xor [rbp - " << slot << "], rax\n";
          } else if (assign.op == "<<=") {
            out_ << "  mov rcx, rax\n";
            out_ << "  shl qword ptr [rbp - " << slot << "], cl\n";
          } else if (assign.op == ">>=") {
            out_ << "  mov rcx, rax\n";
            out_ << "  sar qword ptr [rbp - " << slot << "], cl\n";
          }
        }
        break;
      }
      case StmtKind::If: {
        const auto& ifStmt = static_cast<const IfStmt&>(stmt);
        if (ifStmt.branches.empty()) break;
        std::string endLbl = nextLabel("endif");

        for (std::size_t i = 0; i < ifStmt.branches.size(); ++i) {
          const auto& br = ifStmt.branches[i];
          emitExpr(*br.cond);
          out_ << "  cmp rax, 0\n";

          std::string nextArmLbl = (i + 1 < ifStmt.branches.size())
                                       ? nextLabel("elif")
                                       : (!ifStmt.elseBody.empty() ? nextLabel("else") : endLbl);
          out_ << "  je " << nextArmLbl << "\n";

          for (const auto& s : br.body) emitStmt(*s);
          out_ << "  jmp " << endLbl << "\n";

          if (i + 1 < ifStmt.branches.size() || !ifStmt.elseBody.empty()) {
            out_ << nextArmLbl << ":\n";
          }
        }

        if (!ifStmt.elseBody.empty()) {
          for (const auto& s : ifStmt.elseBody) emitStmt(*s);
        }

        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::While: {
        const auto& wh = static_cast<const WhileStmt&>(stmt);
        std::string condLbl = nextLabel("while_cond");
        std::string endLbl = nextLabel("while_end");

        out_ << condLbl << ":\n";
        emitExpr(*wh.cond);
        out_ << "  cmp rax, 0\n";
        out_ << "  je " << endLbl << "\n";

        for (const auto& s : wh.body) emitStmt(*s);
        out_ << "  jmp " << condLbl << "\n";
        out_ << endLbl << ":\n";
        break;
      }
      case StmtKind::For: {
        const auto& forStmt = static_cast<const ForStmt&>(stmt);
        if (forStmt.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*forStmt.iterable);
          int varSlot = allocateSlot(forStmt.var);
          emitExpr(*range.start);
          out_ << "  mov [rbp - " << varSlot << "], rax\n";

          int endSlot = allocateSlot("__end_" + forStmt.var);
          emitExpr(*range.end);
          out_ << "  mov [rbp - " << endSlot << "], rax\n";

          std::string condLbl = nextLabel("for_cond");
          std::string endLbl = nextLabel("for_end");

          out_ << condLbl << ":\n";
          out_ << "  mov rax, [rbp - " << varSlot << "]\n";
          out_ << "  cmp rax, [rbp - " << endSlot << "]\n";
          out_ << "  jge " << endLbl << "\n";

          for (const auto& s : forStmt.body) emitStmt(*s);

          out_ << "  add qword ptr [rbp - " << varSlot << "], 1\n";
          out_ << "  jmp " << condLbl << "\n";
          out_ << endLbl << ":\n";
        }
        break;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        if (ret.value) {
          emitExpr(*ret.value);
        } else {
          out_ << "  mov rax, 0\n";
        }
        out_ << "  mov rsp, rbp\n";
        out_ << "  pop rbp\n";
        out_ << "  ret\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& es = static_cast<const ExprStmt&>(stmt);
        emitExpr(*es.expr);
        break;
      }
      default:
        break;
    }
  }

  void emitExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
        out_ << "  mov rax, " << static_cast<const IntLitExpr&>(expr).value << "\n";
        break;
      case ExprKind::BoolLit:
        out_ << "  mov rax, " << (static_cast<const BoolLitExpr&>(expr).value ? 1 : 0) << "\n";
        break;
      case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(expr);
        auto it = stackSlots_.find(id.name);
        if (it != stackSlots_.end()) {
          out_ << "  mov rax, [rbp - " << it->second << "]\n";
        } else {
          out_ << "  mov rax, 0\n";
        }
        break;
      }
      case ExprKind::Binary: {
        const auto& bin = static_cast<const BinaryExpr&>(expr);
        emitExpr(*bin.lhs);
        out_ << "  push rax\n";
        emitExpr(*bin.rhs);
        out_ << "  mov rbx, rax\n";
        out_ << "  pop rax\n";

        if (bin.op == "+") {
          out_ << "  add rax, rbx\n";
        } else if (bin.op == "-") {
          out_ << "  sub rax, rbx\n";
        } else if (bin.op == "*") {
          out_ << "  imul rax, rbx\n";
        } else if (bin.op == "/") {
          out_ << "  cqo\n";
          out_ << "  idiv rbx\n";
        } else if (bin.op == "%") {
          out_ << "  cqo\n";
          out_ << "  idiv rbx\n";
          out_ << "  mov rax, rdx\n";
        } else if (bin.op == "==") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  sete al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "!=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setne al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "<") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setl al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "<=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setle al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == ">") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setg al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == ">=") {
          out_ << "  cmp rax, rbx\n";
          out_ << "  setge al\n";
          out_ << "  movzx rax, al\n";
        } else if (bin.op == "and" || bin.op == "&") {
          out_ << "  and rax, rbx\n";
        } else if (bin.op == "or" || bin.op == "|") {
          out_ << "  or rax, rbx\n";
        } else if (bin.op == "^") {
          out_ << "  xor rax, rbx\n";
        } else if (bin.op == "<<") {
          out_ << "  mov rcx, rbx\n";
          out_ << "  shl rax, cl\n";
        } else if (bin.op == ">>") {
          out_ << "  mov rcx, rbx\n";
          out_ << "  sar rax, cl\n";
        }
        break;
      }
      case ExprKind::Unary: {
        const auto& un = static_cast<const UnaryExpr&>(expr);
        emitExpr(*un.operand);
        if (un.op == "-") {
          out_ << "  neg rax\n";
        } else if (un.op == "~") {
          out_ << "  not rax\n";
        } else if (un.op == "not" || un.op == "!") {
          out_ << "  cmp rax, 0\n";
          out_ << "  sete al\n";
          out_ << "  movzx rax, al\n";
        }
        break;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*call.callee).name;
          if (name == "print") {
            if (!call.args.empty()) {
              const auto& argExpr = *call.args[0].value;
              if (argExpr.kind == ExprKind::StringLit) {
                std::string s = extractString(static_cast<const StringLitExpr&>(argExpr));
                out_ << "  lea rdi, .Lfmt_str[rip]\n";
                out_ << "  lea rsi, " << strTable_[s] << "[rip]\n";
                out_ << "  mov rax, 0\n";
                out_ << "  call printf@PLT\n";
                return;
              }
              emitExpr(argExpr);
              out_ << "  mov rsi, rax\n";
              out_ << "  lea rdi, .Lfmt_int[rip]\n";
              out_ << "  mov rax, 0\n";
              out_ << "  call printf@PLT\n";
              return;
            }
          }

          // User function call
          static const char* kArgRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
          for (std::size_t i = 0; i < call.args.size() && i < 6; ++i) {
            emitExpr(*call.args[i].value);
            out_ << "  push rax\n";
          }
          for (int i = static_cast<int>(call.args.size()) - 1; i >= 0 && i < 6; --i) {
            out_ << "  pop " << kArgRegs[i] << "\n";
          }
          out_ << "  call " << (name == "main" ? "_a_main" : "_a_" + name) << "\n";
        }
        break;
      }
      default:
        out_ << "  mov rax, 0\n";
        break;
    }
  }

  void emitMain() {
    out_ << ".globl main\n";
    out_ << ".type main, @function\n";
    out_ << "main:\n";
    out_ << "  push rbp\n";
    out_ << "  mov rbp, rsp\n";
    out_ << "  sub rsp, 256\n";

    if (graph_.entry >= 0 && static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) {
            emitStmt(*stmt);
          }
        }
      }
    }

    out_ << "  mov rax, 0\n";
    out_ << "  mov rsp, rbp\n";
    out_ << "  pop rbp\n";
    out_ << "  ret\n";
  }
};

}  // namespace

bool emitLLVMIR(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  LLVMEmitter emitter(graph, sema, out);
  return emitter.emit();
}

bool emitX86Assembly(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  X86Emitter emitter(graph, sema, out);
  return emitter.emit();
}

bool buildLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                     const std::string& outputPath, std::ostream& err) {
  std::string tmpLL = outputPath + ".ll";
  {
    std::ofstream ofs(tmpLL);
    if (!ofs) {
      err << "error: failed to open temporary file " << tmpLL << "\n";
      return false;
    }
    if (!emitLLVMIR(graph, sema, ofs)) {
      err << "error: failed to generate LLVM IR\n";
      return false;
    }
  }

  // Also emit direct x86-64 assembly to guarantee compilation into standalone ELF executable
  std::string tmpS = outputPath + ".s";
  {
    std::ofstream ofs(tmpS);
    if (!ofs) {
      err << "error: failed to open temporary assembly file " << tmpS << "\n";
      return false;
    }
    if (!emitX86Assembly(graph, sema, ofs)) {
      err << "error: failed to generate x86 assembly\n";
      return false;
    }
  }

  // Assemble and link via gcc/as
  std::string cmd = "gcc -no-pie " + tmpS + " -o " + outputPath + " -lm 2>/dev/null";
  int ret = std::system(cmd.c_str());
  if (ret != 0) {
    err << "error: assembly/linking failed with exit code " << ret << "\n";
    return false;
  }

  return true;
}

int runLLVMModule(const ModuleGraph& graph, const SemaResult& sema,
                  std::istream& in, std::ostream& out, std::ostream& err) {
  (void)in;
  std::string tmpBase = "/tmp/a_llvm_run_" + std::to_string(getpid()) + "_" + std::to_string(rand());
  if (!buildLLVMModule(graph, sema, tmpBase, err)) {
    return 1;
  }

  // Execute directly and stream stdout/stderr
  std::string runCmd = tmpBase + " 2>/tmp/a_llvm_err.txt";
  FILE* pipe = popen(runCmd.c_str(), "r");
  if (!pipe) {
    err << "error: failed to execute LLVM binary\n";
    std::filesystem::remove(tmpBase);
    std::filesystem::remove(tmpBase + ".s");
    std::filesystem::remove(tmpBase + ".ll");
    return 2;
  }

  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    out << buf;
  }
  int status = pclose(pipe);

  std::ifstream errFile("/tmp/a_llvm_err.txt");
  if (errFile) {
    std::string errLine;
    while (std::getline(errFile, errLine)) {
      err << errLine << "\n";
    }
  }
  std::filesystem::remove("/tmp/a_llvm_err.txt");
  std::filesystem::remove(tmpBase);
  std::filesystem::remove(tmpBase + ".s");
  std::filesystem::remove(tmpBase + ".ll");

  return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

namespace {
bool checkSupportedExpr(const Expr& expr);
bool checkSupportedStmt(const Stmt& stmt, bool isGlobal = false) {
  switch (stmt.kind) {
    case StmtKind::Let: {
      if (isGlobal) return false;
      const auto& s = static_cast<const LetStmt&>(stmt);
      return !s.init || checkSupportedExpr(*s.init);
    }
    case StmtKind::Assign: {
      const auto& s = static_cast<const AssignStmt&>(stmt);
      return s.target->kind == ExprKind::Ident && checkSupportedExpr(*s.value);
    }
    case StmtKind::Expr: {
      const auto& s = static_cast<const ExprStmt&>(stmt);
      return checkSupportedExpr(*s.expr);
    }
    case StmtKind::Return: {
      const auto& s = static_cast<const ReturnStmt&>(stmt);
      return !s.value || checkSupportedExpr(*s.value);
    }
    case StmtKind::If: {
      const auto& s = static_cast<const IfStmt&>(stmt);
      for (const auto& br : s.branches) {
        if (!checkSupportedExpr(*br.cond)) return false;
        for (const auto& st : br.body) if (!checkSupportedStmt(*st, false)) return false;
      }
      for (const auto& st : s.elseBody) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::While: {
      const auto& s = static_cast<const WhileStmt&>(stmt);
      if (!checkSupportedExpr(*s.cond)) return false;
      for (const auto& st : s.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::For: {
      const auto& s = static_cast<const ForStmt&>(stmt);
      if (s.iterable->kind != ExprKind::Range) return false;
      const auto& r = static_cast<const RangeExpr&>(*s.iterable);
      if (r.step) return false;
      if (!checkSupportedExpr(*r.start) || !checkSupportedExpr(*r.end)) return false;
      // Scoped shadowing check: if any nested statement declares a variable with loop var's name,
      // fallback to interpreter
      std::vector<const Stmt*> queue;
      for (const auto& st : s.body) queue.push_back(st.get());
      while (!queue.empty()) {
        const Stmt* cur = queue.back();
        queue.pop_back();
        if (cur->kind == StmtKind::Let) {
          const auto& let = static_cast<const LetStmt&>(*cur);
          if (let.name == s.var) return false;
        } else if (cur->kind == StmtKind::If) {
          const auto& ifs = static_cast<const IfStmt&>(*cur);
          for (const auto& br : ifs.branches) {
            for (const auto& st : br.body) queue.push_back(st.get());
          }
          for (const auto& st : ifs.elseBody) queue.push_back(st.get());
        } else if (cur->kind == StmtKind::While) {
          const auto& wh = static_cast<const WhileStmt&>(*cur);
          for (const auto& st : wh.body) queue.push_back(st.get());
        }
      }
      for (const auto& st : s.body) {
        if (!checkSupportedStmt(*st, false)) return false;
      }
      return true;
    }
    case StmtKind::FnDecl: {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      for (const auto& st : fn.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    default:
      return false;
  }
}

bool checkSupportedExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::IntLit:
    case ExprKind::BoolLit:
    case ExprKind::Ident:
      return true;
    case ExprKind::StringLit: {
      const auto& s = static_cast<const StringLitExpr&>(expr);
      for (const auto& p : s.parts) if (p.isExpr) return false;
      return true;
    }
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      // Division or remainder requires runtime checks for zero, fallback to interpreter
      if (b.op == "/" || b.op == "%") {
        return false;
      } else if (b.op == "<<" || b.op == ">>") {
        if (b.rhs->kind == ExprKind::IntLit) {
          int64_t val = static_cast<const IntLitExpr&>(*b.rhs).value;
          if (val < 0 || val >= 64) return false;
        } else {
          return false;
        }
      }
      return checkSupportedExpr(*b.lhs) && checkSupportedExpr(*b.rhs);
    }
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(expr);
      if (u.op == "!") return false; // unwrap is handled by runtime/interpreter
      return checkSupportedExpr(*u.operand);
    }
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(expr);
      if (c.callee->kind != ExprKind::Ident) return false;
      const std::string& name = static_cast<const IdentExpr&>(*c.callee).name;
      // Standard-library builtins (json, binary, testing, log, metrics,
      // trace, net, http) only exist in the interpreter.
      if (isStdlibBuiltin(name)) return false;
      // Native runtime functions are handled by interpreter or native backend
      if (name == "write_file" || name == "read_file" || name == "append_file" ||
          name == "file_exists" || name == "remove_file" || name == "flush" ||
          name == "input" || name == "read_line" || name == "len" ||
          name == "int" || name == "float" || name == "set" ||
          name == "unsafe_get" || name == "unsafe_set" || name == "move" ||
          name == "arena_new" || name == "arena_pool" || name == "arena_region" ||
          name == "arena_alloc" || name == "arena_release" || name == "arena_reset" ||
          name == "arena_used" || name == "arena_total" || name == "copy" ||
          name == "borrow" || name == "borrow_mut" || name == "unsafe_address_of" ||
          name == "unsafe_load" || name == "unsafe_store" || name == "unsafe_offset") {
        return false;
      }
      for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
      return true;
    }
    default:
      return false;
  }
}
}  // namespace

bool isLLVMSupported(const ModuleGraph& graph) {
  if (graph.modules.empty()) return false;
  for (const auto& mod : graph.modules) {
    if (!mod.program) continue;
    for (const auto& st : mod.program->statements) {
      if (!checkSupportedStmt(*st, true)) return false;
    }
  }
  return true;
}

}  // namespace a
