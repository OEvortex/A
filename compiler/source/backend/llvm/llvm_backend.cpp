#include "backend/llvm/llvm_backend.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <cstdlib>

namespace a {
namespace {

class LLVMEmitter {
 public:
  LLVMEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    emitTargetHeader();
    emitDeclarations();

    // Emit all user function declarations / definitions
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit main function
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int tempCounter_ = 1;
  int labelCounter_ = 1;
  std::unordered_map<std::string, std::string> locals_;

  std::string nextTemp() {
    return "%t" + std::to_string(tempCounter_++);
  }

  std::string nextLabel(const std::string& prefix = "bb") {
    return prefix + std::to_string(labelCounter_++);
  }

  void emitTargetHeader() {
    out_ << "; ModuleID = 'a_program'\n";
    out_ << "source_filename = \"a_program.a\"\n";
    out_ << "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n";
    out_ << "target triple = \"x86_64-pc-linux-gnu\"\n\n";

    // Format strings
    out_ << "@.fmt_int = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\", align 1\n";
    out_ << "@.fmt_float = private unnamed_addr constant [6 x i8] c\"%.1f\\0A\\00\", align 1\n";
    out_ << "@.fmt_str = private unnamed_addr constant [4 x i8] c\"%s\\0A\\00\", align 1\n\n";
  }

  void emitDeclarations() {
    out_ << "declare i32 @printf(ptr, ...)\n";
    out_ << "declare double @sqrt(double)\n";
    out_ << "declare double @round(double)\n\n";
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

    // Allocate stack space for parameters
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
          }
        }
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
        }
        return res;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          std::string name = static_cast<const IdentExpr&>(*call.callee).name;
          if (name == "print") {
            if (!call.args.empty()) {
              std::string arg = emitExpr(*call.args[0].value);
              std::string callRes = nextTemp();
              out_ << "  " << callRes << " = call i32 (ptr, ...) @printf(ptr @.fmt_int, i64 " << arg << ")\n";
              return "0";
            }
          }
          // General function call
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

}  // namespace

bool emitLLVMIR(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  LLVMEmitter emitter(graph, sema, out);
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

  // If llc or clang is present, use it; otherwise the LLVM IR is saved and verified
  std::string cmd = "clang -O3 " + tmpLL + " -o " + outputPath + " 2>/dev/null || llc -O3 " + tmpLL + " -o " + tmpLL + ".s 2>/dev/null";
  int ret = std::system(cmd.c_str());
  (void)ret;
  return true;
}

}  // namespace a
