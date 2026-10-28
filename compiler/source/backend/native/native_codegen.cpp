#include "backend/native/native_codegen.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <set>

namespace a {

namespace {

class NativeEmitter {
 public:
  NativeEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    emitHeaders();
    emitRuntimeHelpers();
    
    // Emit function prototypes
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      currentModule_ = static_cast<int>(i);
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunctionDecl(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Emit function definitions
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      currentModule_ = static_cast<int>(i);
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          emitFunction(*static_cast<const FnDecl*>(stmt.get()));
        }
      }
    }

    // Main entry point
    out_ << "\nint main(int argc, char** argv) {\n";
    out_ << "    (void)argc; (void)argv;\n";
    if (graph_.entry >= 0 && static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      currentModule_ = graph_.entry;
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) {
            emitStmt(*stmt, 1);
          }
        }
      }
    }
    out_ << "    return 0;\n";
    out_ << "}\n";

    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  int currentModule_ = 0;
  std::set<std::string> declaredLocals_;

  void emitHeaders() {
    out_ << "#include <iostream>\n";
    out_ << "#include <vector>\n";
    out_ << "#include <string>\n";
    out_ << "#include <cmath>\n";
    out_ << "#include <cstdint>\n";
    out_ << "#include <iomanip>\n";
    out_ << "#include <algorithm>\n";
    out_ << "#include <functional>\n\n";
  }

  void emitRuntimeHelpers() {
    out_ << "using int_t = std::int64_t;\n";
    out_ << "using float_t_ = double;\n\n";

    out_ << "template <typename T>\n";
    out_ << "void a_print(const T& val) { std::cout << std::fixed << std::setprecision(1) << val << std::endl; }\n";
    out_ << "inline void a_print(int_t val) { std::cout << val << std::endl; }\n";
    out_ << "inline void a_print(const std::string& val) { std::cout << val << std::endl; }\n\n";

    out_ << "inline float_t_ a_sqrt(float_t_ val) { return std::sqrt(val); }\n";
    out_ << "inline float_t_ a_round(float_t_ val) { return std::round(val); }\n";
    out_ << "inline float_t_ a_f64(int_t val) { return static_cast<float_t_>(val); }\n";
    out_ << "inline int_t a_i64(float_t_ val) { return static_cast<int_t>(val); }\n";
    out_ << "inline std::string a_str(int_t val) { return std::to_string(val); }\n";
    out_ << "inline std::string a_str(float_t_ val) { return std::to_string(val); }\n";
    out_ << "inline std::string a_str(const std::string& val) { return val; }\n";
    out_ << "inline int_t a_len(const std::string& s) { return static_cast<int_t>(s.size()); }\n";
    out_ << "template <typename T>\n";
    out_ << "inline int_t a_len(const std::vector<T>& v) { return static_cast<int_t>(v.size()); }\n\n";

    out_ << "inline std::string a_join(const std::vector<std::string>& parts, const std::string& delim) {\n";
    out_ << "    std::string res;\n";
    out_ << "    for (std::size_t i = 0; i < parts.size(); ++i) {\n";
    out_ << "        if (i > 0) res += delim;\n";
    out_ << "        res += parts[i];\n";
    out_ << "    }\n";
    out_ << "    return res;\n";
    out_ << "}\n\n";

    out_ << "template <typename T, typename Pred>\n";
    out_ << "inline std::vector<T> a_filter(const std::vector<T>& in, Pred p) {\n";
    out_ << "    std::vector<T> out;\n";
    out_ << "    for (const auto& item : in) if (p(item)) out.push_back(item);\n";
    out_ << "    return out;\n";
    out_ << "}\n\n";

    out_ << "template <typename T, typename Fn>\n";
    out_ << "inline auto a_map(const std::vector<T>& in, Fn f) -> std::vector<decltype(f(in[0]))> {\n";
    out_ << "    std::vector<decltype(f(in[0]))> out;\n";
    out_ << "    out.reserve(in.size());\n";
    out_ << "    for (const auto& item : in) out.push_back(f(item));\n";
    out_ << "    return out;\n";
    out_ << "}\n\n";

    out_ << "inline int_t a_sum(const std::vector<int_t>& in) {\n";
    out_ << "    int_t s = 0;\n";
    out_ << "    for (auto v : in) s += v;\n";
    out_ << "    return s;\n";
    out_ << "}\n";
    out_ << "inline float_t_ a_sum(const std::vector<float_t_>& in) {\n";
    out_ << "    float_t_ s = 0;\n";
    out_ << "    for (auto v : in) s += v;\n";
    out_ << "    return s;\n";
    out_ << "}\n\n";
  }

  void indent(int level) {
    for (int i = 0; i < level; ++i) out_ << "    ";
  }

  std::string mangle(const std::string& name) {
    if (name == "main") return "_a_main";
    return "_a_" + name;
  }

  void emitFunctionDecl(const FnDecl& fn) {
    out_ << "auto " << mangle(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i > 0) out_ << ", ";
      out_ << "auto " << mangle(fn.params[i].name);
    }
    out_ << ");\n";
  }

  void emitFunction(const FnDecl& fn) {
    declaredLocals_.clear();
    for (const auto& param : fn.params) {
      declaredLocals_.insert(param.name);
    }
    out_ << "\nauto " << mangle(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i > 0) out_ << ", ";
      out_ << "auto " << mangle(fn.params[i].name);
    }
    out_ << ") {\n";
    for (const auto& stmt : fn.body) {
      emitStmt(*stmt, 1);
    }
    out_ << "}\n";
  }

  void emitStmt(const Stmt& stmt, int level) {
    indent(level);
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& let = static_cast<const LetStmt&>(stmt);
        declaredLocals_.insert(let.name);
        out_ << "auto " << mangle(let.name);
        if (let.init) {
          out_ << " = ";
          emitExpr(*let.init);
        }
        out_ << ";\n";
        break;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(stmt);
        if (assign.target->kind == ExprKind::Ident) {
          const auto& id = static_cast<const IdentExpr&>(*assign.target);
          if (assign.op == "=" && !declaredLocals_.count(id.name)) {
            declaredLocals_.insert(id.name);
            out_ << "auto " << mangle(id.name) << " = ";
            emitExpr(*assign.value);
            out_ << ";\n";
            break;
          }
        }
        emitExpr(*assign.target);
        out_ << " " << assign.op << " ";
        emitExpr(*assign.value);
        out_ << ";\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& exprStmt = static_cast<const ExprStmt&>(stmt);
        emitExpr(*exprStmt.expr);
        out_ << ";\n";
        break;
      }
      case StmtKind::For: {
        const auto& forStmt = static_cast<const ForStmt&>(stmt);
        if (forStmt.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*forStmt.iterable);
          out_ << "for (int_t " << mangle(forStmt.var) << " = ";
          emitExpr(*range.start);
          out_ << "; " << mangle(forStmt.var) << " < ";
          emitExpr(*range.end);
          out_ << "; " << mangle(forStmt.var) << " += ";
          if (range.step) emitExpr(*range.step);
          else out_ << "1";
          out_ << ") {\n";
          for (const auto& s : forStmt.body) emitStmt(*s, level + 1);
          indent(level);
          out_ << "}\n";
        } else {
          out_ << "for (const auto& " << mangle(forStmt.var) << " : ";
          emitExpr(*forStmt.iterable);
          out_ << ") {\n";
          for (const auto& s : forStmt.body) emitStmt(*s, level + 1);
          indent(level);
          out_ << "}\n";
        }
        break;
      }
      case StmtKind::While: {
        const auto& whileStmt = static_cast<const WhileStmt&>(stmt);
        out_ << "while (";
        emitExpr(*whileStmt.cond);
        out_ << ") {\n";
        for (const auto& s : whileStmt.body) emitStmt(*s, level + 1);
        indent(level);
        out_ << "}\n";
        break;
      }
      case StmtKind::If: {
        const auto& ifStmt = static_cast<const IfStmt&>(stmt);
        for (std::size_t i = 0; i < ifStmt.branches.size(); ++i) {
          if (i == 0) out_ << "if (";
          else { indent(level); out_ << "else if ("; }
          emitExpr(*ifStmt.branches[i].cond);
          out_ << ") {\n";
          for (const auto& s : ifStmt.branches[i].body) emitStmt(*s, level + 1);
          indent(level);
          out_ << "}\n";
        }
        if (!ifStmt.elseBody.empty()) {
          indent(level);
          out_ << "else {\n";
          for (const auto& s : ifStmt.elseBody) emitStmt(*s, level + 1);
          indent(level);
          out_ << "}\n";
        }
        break;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(stmt);
        out_ << "return";
        if (ret.value) {
          out_ << " ";
          emitExpr(*ret.value);
        }
        out_ << ";\n";
        break;
      }
      case StmtKind::Break:
        out_ << "break;\n";
        break;
      case StmtKind::Continue:
        out_ << "continue;\n";
        break;
      default:
        out_ << "// unsupported stmt\n";
        break;
    }
  }

  void emitExpr(const Expr& expr) {
    switch (expr.kind) {
      case ExprKind::IntLit:
        out_ << "int_t(" << static_cast<const IntLitExpr&>(expr).value << "LL)";
        break;
      case ExprKind::FloatLit: {
        double val = static_cast<const FloatLitExpr&>(expr).value;
        std::ostringstream ss;
        ss << std::defaultfloat << val;
        std::string s = ss.str();
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) {
          s += ".0";
        }
        out_ << "float_t_(" << s << ")";
        break;
      }
      case ExprKind::BoolLit:
        out_ << (static_cast<const BoolLitExpr&>(expr).value ? "true" : "false");
        break;
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(expr);
        out_ << "std::string(\"";
        for (const auto& p : s.parts) {
          if (!p.isExpr) {
            for (char c : p.text) {
              if (c == '"' || c == '\\') out_ << '\\';
              out_ << c;
            }
          }
        }
        out_ << "\")";
        break;
      }
      case ExprKind::Ident: {
        const auto& id = static_cast<const IdentExpr&>(expr);
        out_ << mangle(id.name);
        break;
      }
      case ExprKind::Binary: {
        const auto& bin = static_cast<const BinaryExpr&>(expr);
        out_ << "(";
        emitExpr(*bin.lhs);
        out_ << " " << bin.op << " ";
        emitExpr(*bin.rhs);
        out_ << ")";
        break;
      }
      case ExprKind::Unary: {
        const auto& un = static_cast<const UnaryExpr&>(expr);
        std::string op = un.op == "not" ? "!" : un.op;
        out_ << "(" << op;
        emitExpr(*un.operand);
        out_ << ")";
        break;
      }
      case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        if (call.callee->kind == ExprKind::Ident) {
          const auto& calleeName = static_cast<const IdentExpr&>(*call.callee).name;
          if (calleeName == "print") {
            out_ << "a_print(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "sqrt") {
            out_ << "a_sqrt(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "round") {
            out_ << "a_round(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "f64") {
            out_ << "a_f64(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "i64") {
            out_ << "a_i64(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "str") {
            out_ << "a_str(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "len") {
            out_ << "a_len(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
          if (calleeName == "join") {
            out_ << "a_join(";
            emitExpr(*call.args[0].value);
            out_ << ", ";
            emitExpr(*call.args[1].value);
            out_ << ")";
            return;
          }
          if (calleeName == "filter") {
            out_ << "a_filter(";
            emitExpr(*call.args[0].value);
            out_ << ", ";
            emitExpr(*call.args[1].value);
            out_ << ")";
            return;
          }
          if (calleeName == "map") {
            out_ << "a_map(";
            emitExpr(*call.args[0].value);
            out_ << ", ";
            emitExpr(*call.args[1].value);
            out_ << ")";
            return;
          }
          if (calleeName == "sum") {
            out_ << "a_sum(";
            if (!call.args.empty()) emitExpr(*call.args[0].value);
            out_ << ")";
            return;
          }
        }
        emitExpr(*call.callee);
        out_ << "(";
        for (std::size_t i = 0; i < call.args.size(); ++i) {
          if (i > 0) out_ << ", ";
          emitExpr(*call.args[i].value);
        }
        out_ << ")";
        break;
      }
      case ExprKind::Lambda: {
        const auto& lam = static_cast<const LambdaExpr&>(expr);
        out_ << "[&](";
        for (std::size_t i = 0; i < lam.params.size(); ++i) {
          if (i > 0) out_ << ", ";
          out_ << "auto " << mangle(lam.params[i].name);
        }
        out_ << ") { return ";
        emitExpr(*lam.body);
        out_ << "; }";
        break;
      }
      case ExprKind::ArrayLit: {
        const auto& arr = static_cast<const ArrayLitExpr&>(expr);
        out_ << "std::vector{";
        for (std::size_t i = 0; i < arr.elements.size(); ++i) {
          if (i > 0) out_ << ", ";
          emitExpr(*arr.elements[i]);
        }
        out_ << "}";
        break;
      }
      case ExprKind::Comprehension: {
        const auto& comp = static_cast<const ComprehensionExpr&>(expr);
        out_ << "([&]() {\n";
        if (comp.iterable->kind == ExprKind::Range) {
          const auto& range = static_cast<const RangeExpr&>(*comp.iterable);
          out_ << "        auto " << mangle(comp.var) << " = ";
          emitExpr(*range.start);
          out_ << ";\n";
          out_ << "        std::vector<decltype(";
          emitExpr(*comp.element);
          out_ << ")> _res;\n";
          out_ << "        for (; " << mangle(comp.var) << " < ";
          emitExpr(*range.end);
          out_ << "; " << mangle(comp.var) << " += ";
          if (range.step) emitExpr(*range.step);
          else out_ << "1";
          out_ << ") {\n";
        } else {
          out_ << "        std::vector<int_t> _res;\n";
          out_ << "        for (const auto& " << mangle(comp.var) << " : ";
          emitExpr(*comp.iterable);
          out_ << ") {\n";
        }
        if (comp.condition) {
          out_ << "            if (";
          emitExpr(*comp.condition);
          out_ << ") ";
        }
        out_ << "            _res.push_back(";
        emitExpr(*comp.element);
        out_ << ");\n";
        out_ << "        }\n";
        out_ << "        return _res;\n";
        out_ << "    })()";
        break;
      }
      default:
        out_ << "/* expr */";
        break;
    }
  }
};

}  // namespace

bool emitNativeCpp(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  NativeEmitter emitter(graph, sema, out);
  return emitter.emit();
}

bool buildNativeBinary(const ModuleGraph& graph, const SemaResult& sema,
                       const std::string& outputPath, std::ostream& err) {
  std::string tmpCpp = outputPath + ".tmp.cpp";
  {
    std::ofstream ofs(tmpCpp);
    if (!ofs) {
      err << "error: failed to open temporary file " << tmpCpp << "\n";
      return false;
    }
    if (!emitNativeCpp(graph, sema, ofs)) {
      err << "error: failed to generate C++ code\n";
      std::filesystem::remove(tmpCpp);
      return false;
    }
  }

  std::string cmd = "g++ -O3 -std=c++20 " + tmpCpp + " -o " + outputPath;
  int code = std::system(cmd.c_str());
  std::filesystem::remove(tmpCpp);

  if (code != 0) {
    err << "error: compilation command failed with code " << code << "\n";
    return false;
  }
  return true;
}

}  // namespace a
