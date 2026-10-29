#include "backend/native/native_codegen.hpp"

#include <cmath>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
#include <set>
#include <unistd.h>
#include <sys/wait.h>

namespace a {

namespace {

class NativeEmitter {
 public:
  NativeEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    emitHeaders();
    emitRuntimeHelpers();
    
    // Emit class / struct definitions
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      currentModule_ = static_cast<int>(i);
      const auto& mod = graph_.modules[i];
      if (!mod.program) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::StructDecl) {
          emitStructDef(*static_cast<const StructDecl*>(stmt.get()));
        }
      }
    }

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
    out_ << "#include <cstdlib>\n";
    out_ << "#include <vector>\n";
    out_ << "#include <string>\n";
    out_ << "#include <cmath>\n";
    out_ << "#include <cstdint>\n";
    out_ << "#include <iomanip>\n";
    out_ << "#include <algorithm>\n";
    out_ << "#include <functional>\n";
    out_ << "#include <charconv>\n";
    out_ << "#include <sstream>\n\n";
  }

  void emitRuntimeHelpers() {
    out_ << "using int_t = std::int64_t;\n";
    out_ << "using float_t_ = double;\n\n";

    out_ << "namespace {\n";
    out_ << "std::string a_quoteString(const std::string& text) {\n";
    out_ << "  std::string out;\n";
    out_ << "  out.reserve(text.size() + 2);\n";
    out_ << "  out.push_back('\"');\n";
    out_ << "  for (char c : text) {\n";
    out_ << "    switch (c) {\n";
    out_ << "      case '\"': out += \"\\\\\\\"\"; break;\n";
    out_ << "      case '\\\\': out += \"\\\\\\\\\"; break;\n";
    out_ << "      case '\\n': out += \"\\\\\\n\"; break;\n";
    out_ << "      case '\\t': out += \"\\\\\\t\"; break;\n";
    out_ << "      case '\\r': out += \"\\\\\\r\"; break;\n";
    out_ << "      default: out.push_back(c); break;\n";
    out_ << "    }\n";
    out_ << "  }\n";
    out_ << "  out.push_back('\"');\n";
    out_ << "  return out;\n";
    out_ << "}\n";
    out_ << "std::string a_formatDouble(double value) {\n";
    out_ << "  if (std::isnan(value)) return \"nan\";\n";
    out_ << "  if (std::isinf(value)) return value < 0 ? \"-inf\" : \"inf\";\n";
    out_ << "  char buffer[64];\n";
    out_ << "  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);\n";
    out_ << "  std::string text(buffer, result.ptr);\n";
    out_ << "  if (text.find_first_of(\".eE\") == std::string::npos) text += \".0\";\n";
    out_ << "  return text;\n";
    out_ << "}\n";
    out_ << "template <typename T>\n";
    out_ << "std::string a_format(const T& value, bool quoteStrings = false) {\n";
    out_ << "  if constexpr (std::is_floating_point_v<T>) {\n";
    out_ << "    return a_formatDouble(static_cast<double>(value));\n";
    out_ << "  }\n";
    out_ << "  if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {\n";
    out_ << "    char buffer[32];\n";
    out_ << "    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);\n";
    out_ << "    return std::string(buffer, result.ptr);\n";
    out_ << "  }\n";
    out_ << "  std::ostringstream s;\n";
    out_ << "  s << value;\n";
    out_ << "  return s.str();\n";
    out_ << "}\n";
    out_ << "inline std::string a_format(bool value, bool quoteStrings = false) {\n";
    out_ << "  return value ? \"true\" : \"false\";\n";
    out_ << "}\n";
    out_ << "inline std::string a_format(const std::string& value, bool quoteStrings = false) {\n";
    out_ << "  return quoteStrings ? a_quoteString(value) : value;\n";
    out_ << "}\n";
    out_ << "template <typename T>\n";
    out_ << "std::string a_format(const std::vector<T>& value, bool quoteStrings = false) {\n";
    out_ << "  std::string out = \"[\";\n";
    out_ << "  for (std::size_t i = 0; i < value.size(); ++i) {\n";
    out_ << "    if (i) out += \", \";\n";
    out_ << "    out += a_format(value[i], true);\n";
    out_ << "  }\n";
    out_ << "  out += \"]\";\n";
    out_ << "  return out;\n";
    out_ << "}\n";
    out_ << "template <typename... Args>\n";
    out_ << "void a_print(const Args&... args) {\n";
    out_ << "  std::string out;\n";
    out_ << "  bool first = true;\n";
    out_ << "  auto append = [&](const auto& v) {\n";
    out_ << "    if (!first) out += ' ';\n";
    out_ << "    first = false;\n";
    out_ << "    out += a_format(v);\n";
    out_ << "  };\n";
    out_ << "  (append(args), ...);\n";
    out_ << "  std::cout << out << '\\n';\n";
    out_ << "}\n";
    out_ << "}  // namespace\n\n";

    out_ << "inline void a_fail_div() {\n";
    out_ << "    std::cerr << \"Division by zero.\\nCheck the divisor before dividing.\" << std::endl;\n";
    out_ << "    std::exit(2);\n";
    out_ << "}\n\n";

    out_ << "inline void a_fail_shift(std::int64_t amount) {\n";
    out_ << "    std::cerr << \"Shift amount \" << amount << \" is out of range.\" << std::endl;\n";
    out_ << "    std::exit(2);\n";
    out_ << "}\n\n";

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

  void emitStructDef(const StructDecl& decl) {
    out_ << "struct " << mangle(decl.name);
    if (!decl.baseClass.empty()) {
      out_ << " : public " << mangle(decl.baseClass);
    }
    out_ << " {\n";
    for (const auto& field : decl.fields) {
      out_ << "    auto " << mangle(field.name) << ";\n";
    }
    for (const auto& method : decl.methods) {
      out_ << "    auto " << mangle(method->name) << "(";
      for (std::size_t i = 1; i < method->params.size(); ++i) {
        if (i > 1) out_ << ", ";
        out_ << "auto " << mangle(method->params[i].name);
      }
      out_ << ") {\n";
      for (const auto& s : method->body) {
        emitStmt(*s, 2);
      }
      out_ << "    }\n";
    }
    out_ << "};\n\n";
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
        // Division, remainder and shifts need runtime guards; the native
        // backend would otherwise turn divisor/remainder zero or out-of-range
        // shift amounts into hardware exceptions instead of clean errors.
        if (bin.binop == BinOp::Div || bin.binop == BinOp::Mod) {
          out_ << "(";
          emitExpr(*bin.rhs);
          out_ << " == 0 ? (a_fail_div(), int_t(0)) : (";
          emitExpr(*bin.lhs);
          out_ << " " << bin.op << " ";
          emitExpr(*bin.rhs);
          out_ << "))";
          break;
        }
        if (bin.binop == BinOp::Shl || bin.binop == BinOp::Shr) {
          out_ << "(";
          emitExpr(*bin.rhs);
          out_ << " < 0 || ";
          emitExpr(*bin.rhs);
          out_ << " > 63 ? (a_fail_shift(";
          emitExpr(*bin.rhs);
          out_ << "), int_t(0)) : (";
          emitExpr(*bin.lhs);
          out_ << " " << bin.op << " ";
          emitExpr(*bin.rhs);
          out_ << "))";
          break;
        }
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
            for (std::size_t i = 0; i < call.args.size(); ++i) {
              if (i) out_ << ", ";
              emitExpr(*call.args[i].value);
            }
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

int runNativeBinary(const ModuleGraph& graph, const SemaResult& sema,
                    std::istream& in, std::ostream& out, std::ostream& err) {
  (void)in;
  std::string tmpBase = "/tmp/a_native_run_" + std::to_string(getpid()) + "_" + std::to_string(rand());
  if (!buildNativeBinary(graph, sema, tmpBase, err)) {
    return -1;  // native emitter could not compile the program
  }

  // Execute directly and stream stdout/stderr.
  std::string runCmd = tmpBase + " 2>/tmp/a_native_err.txt";
  FILE* pipe = popen(runCmd.c_str(), "r");
  if (!pipe) {
    err << "error: failed to execute native binary\n";
    std::filesystem::remove(tmpBase);
    return 2;
  }

  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    out << buf;
  }
  int status = pclose(pipe);

  std::ifstream errFile("/tmp/a_native_err.txt");
  if (errFile) {
    std::string errLine;
    while (std::getline(errFile, errLine)) {
      err << errLine << "\n";
    }
  }
  std::filesystem::remove("/tmp/a_native_err.txt");
  std::filesystem::remove(tmpBase);

  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 2;
}

namespace {

bool checkNativeExpr(const Expr& expr) {
  switch (expr.kind) {
  case ExprKind::IntLit:
  case ExprKind::FloatLit:
  case ExprKind::BoolLit:
    return true;
  case ExprKind::CharLit:
    return false;  // native emitter has no character-literal support
  case ExprKind::StringLit: {
    // Interpolated parts are silently dropped by the native emitter.
    const auto& s = static_cast<const StringLitExpr&>(expr);
    for (const auto& p : s.parts)
      if (p.isExpr) return false;
    return true;
  }
  case ExprKind::Ident:
    return true;
  case ExprKind::Unary: {
    const auto& u = static_cast<const UnaryExpr&>(expr);
    // "-", "!", "not", "~", "++", "--" all map to valid C++ unary operators.
    const bool ok = u.op == "-" || u.op == "!" || u.op == "not" || u.op == "~" ||
                    u.op == "++" || u.op == "--";
    return ok && checkNativeExpr(*u.operand);
  }
  case ExprKind::Binary: {
    const auto& b = static_cast<const BinaryExpr&>(expr);
    // `**` has no C++ operator.
    if (b.binop == BinOp::Pow) return false;
    return checkNativeExpr(*b.lhs) && checkNativeExpr(*b.rhs);
  }
  case ExprKind::Call: {
    const auto& c = static_cast<const CallExpr&>(expr);
    if (c.callee->kind != ExprKind::Ident) return false;  // closures unsupported
    const std::string& name = static_cast<const IdentExpr&>(*c.callee).name;
    // `print` forwards every argument to the native `a_print` helper, which
    // formats values the same way the interpreter does (including arrays).
    if (name == "print") {
      for (const auto& arg : c.args)
        if (!checkNativeExpr(*arg.value)) return false;
      return true;
    }
    // These builtins are not provided by the native runtime helpers; they
    // would produce an undeclared-identifier error, so route them to the
    // interpreter instead.
    if (name == "input" || name == "read_file" || name == "write_file" ||
        name == "append_file" || name == "file_exists" || name == "remove_file" ||
        name == "flush" || name == "read_line" || name == "eprint" ||
        name == "panic" || name == "assert" || name == "assert_eq" ||
        name == "int" || name == "float" || name == "set" ||
        name == "unsafe_get" || name == "unsafe_set" || name == "move" ||
        name == "arena_new" || name == "arena_pool" || name == "arena_region" ||
        name == "arena_alloc" || name == "arena_release" || name == "arena_reset" ||
        name == "arena_used" || name == "arena_total" || name == "copy" ||
        name == "borrow" || name == "borrow_mut" || name == "unsafe_address_of" ||
        name == "unsafe_load" || name == "unsafe_store" || name == "unsafe_offset")
      return false;
    for (const auto& a : c.args)
      if (!checkNativeExpr(*a.value)) return false;
    return true;
  }
  case ExprKind::ArrayLit: {
    const auto& arr = static_cast<const ArrayLitExpr&>(expr);
    for (const auto& e : arr.elements)
      if (!checkNativeExpr(*e)) return false;
    return true;
  }
  default:
    // Index, Slice, Field, SafeNav, Range, Ternary, Tuple, Lambda,
    // Comprehension, Match, Try, MapLit, SetLit are not emitted correctly.
    return false;
  }
}

bool checkNativeStmt(const Stmt& stmt);

bool checkNativeStmts(const std::vector<StmtPtr>& stmts) {
  for (const auto& s : stmts)
    if (!checkNativeStmt(*s)) return false;
  return true;
}

bool checkNativeStmt(const Stmt& stmt) {
  switch (stmt.kind) {
  case StmtKind::Let: {
    const auto& s = static_cast<const LetStmt&>(stmt);
    return !s.init || checkNativeExpr(*s.init);
  }
  case StmtKind::Assign: {
    const auto& s = static_cast<const AssignStmt&>(stmt);
    if (s.target->kind != ExprKind::Ident) return false;
    return checkNativeExpr(*s.value);
  }
  case StmtKind::Expr: {
    const auto& s = static_cast<const ExprStmt&>(stmt);
    return checkNativeExpr(*s.expr);
  }
  case StmtKind::Return: {
    const auto& s = static_cast<const ReturnStmt&>(stmt);
    return !s.value || checkNativeExpr(*s.value);
  }
  case StmtKind::Break:
  case StmtKind::Continue:
    return true;
  case StmtKind::If: {
    const auto& s = static_cast<const IfStmt&>(stmt);
    for (const auto& br : s.branches) {
      if (!checkNativeExpr(*br.cond)) return false;
      if (!checkNativeStmts(br.body)) return false;
    }
    return checkNativeStmts(s.elseBody);
  }
  case StmtKind::While: {
    const auto& s = static_cast<const WhileStmt&>(stmt);
    if (!checkNativeExpr(*s.cond)) return false;
    return checkNativeStmts(s.body);
  }
  case StmtKind::For: {
    const auto& s = static_cast<const ForStmt&>(stmt);
    // A literal zero step would make the native loop run forever.
    if (s.iterable->kind == ExprKind::Range) {
      const auto& r = static_cast<const RangeExpr&>(*s.iterable);
      if (r.step) {
        if (const auto* lit = static_cast<const IntLitExpr*>(r.step.get()))
          if (lit->value == 0) return false;
      }
    }
    if (!checkNativeExpr(*s.iterable)) return false;
    return checkNativeStmts(s.body);
  }
  default:
    // DoWhile, Defer, Destructure, Loop, Import, FromImport are not emitted.
    return false;
  }
}

}  // namespace

bool isNativeSupported(const ModuleGraph& graph) {
  if (graph.modules.empty()) return false;
  for (const auto& mod : graph.modules) {
    if (!mod.program) continue;
    for (const auto& st : mod.program->statements) {
      switch (st->kind) {
      case StmtKind::FnDecl: {
        const auto& fn = static_cast<const FnDecl&>(*st);
        if (!checkNativeStmts(fn.body)) return false;
        break;
      }
      case StmtKind::StructDecl: {
        const auto& sd = static_cast<const StructDecl&>(*st);
        for (const auto& m : sd.methods)
          if (!checkNativeStmts(m->body)) return false;
        break;
      }
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
        break;
      default:
        if (!checkNativeStmt(*st)) return false;
      }
    }
  }
  return true;
}

}  // namespace a
