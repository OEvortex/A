#pragma once

#include <cstddef>
#include <array>
#include <deque>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "diagnostics/diagnostics.hpp"
#include "errors/runtime_error.hpp"
#include "ast/ast.hpp"
#include "collections/transparent_map.hpp"
#include "memory/value.hpp"
#include "type-checker/sema.hpp"

namespace a {

class Interpreter {
 public:
  Interpreter(const ModuleGraph& graph, const SemaResult& result, DiagnosticSink& sink,
              std::istream& in, std::ostream& out, std::ostream& err);

  // Returns the process exit code: 0 on success, 2 on a runtime error.
  int run();

 private:
  struct Binding {
    Value value;
    bool mutableBinding = true;
  };

  // Environments are small: a function frame or block usually holds a handful
  // of names. A flat vector beats a hash table for that size, and the first
  // few bindings live inline so the common frame allocates nothing at all.
  class BindingMap {
   public:
    using Entry = std::pair<std::string, Binding>;
    static constexpr std::size_t kInline = 4;

    class iterator {
     public:
      iterator(BindingMap* map, std::size_t index) : map_(map), index_(index) {}
      Entry& operator*() const { return map_->at(index_); }
      Entry* operator->() const { return &map_->at(index_); }
      iterator& operator++() {
        ++index_;
        return *this;
      }
      bool operator!=(const iterator& other) const { return index_ != other.index_; }
      bool operator==(const iterator& other) const { return index_ == other.index_; }

     private:
      BindingMap* map_;
      std::size_t index_;
    };

    Binding* find(std::string_view name) {
      for (std::size_t i = 0; i < size_; ++i) {
        Entry& entry = at(i);
        if (entry.first.size() == name.size() && entry.first == name) return &entry.second;
      }
      return nullptr;
    }

    Binding& operator[](std::string_view name) {
      if (Binding* existing = find(name)) return *existing;
      if (size_ < kInline) {
        Entry& entry = inline_[size_];
        entry.first.assign(name.data(), name.size());
        entry.second = Binding{};
      } else {
        overflow_.emplace_back();
        overflow_.back().first.assign(name.data(), name.size());
        overflow_.back().second = Binding{};
      }
      ++size_;
      return at(size_ - 1).second;
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size_); }

   private:
    Entry& at(std::size_t index) {
      return index < kInline ? inline_[index] : overflow_[index - kInline];
    }

    std::array<Entry, kInline> inline_;
    std::vector<Entry> overflow_;
    std::size_t size_ = 0;
  };

  struct Env {
    BindingMap vars;
    Env* parent = nullptr;

    Binding* find(std::string_view name) {
      for (Env* env = this; env; env = env->parent) {
        if (Binding* found = env->vars.find(name)) return found;
      }
      return nullptr;
    }
  };

  struct ReturnSignal {
    Value value;
  };
  struct BreakSignal {};
  struct ContinueSignal {};

  struct LValue {
    enum class Kind { Variable, Field, Element } kind = Kind::Variable;
    Binding* binding = nullptr;
    StructPtr object;
    std::size_t fieldIndex = 0;
    ArrayPtr array;
    std::size_t index = 0;
  };

  [[noreturn]] void fail(Span span, std::string message, std::string help = {});

  void execStatements(const std::vector<StmtPtr>& statements, Env& env);
  void exec(const Stmt& stmt, Env& env);
  void execBlock(const std::vector<StmtPtr>& body, Env& parent);

  Value eval(const Expr& expr, Env& env);
  Value evalIdentifier(const IdentExpr& expr, Env& env);
  Value evalBinary(const BinaryExpr& expr, Env& env);
  Value evalCall(const CallExpr& expr, Env& env);
  Value evalString(const StringLitExpr& expr, Env& env);
  Value evalIndex(const IndexExpr& expr, Env& env);
  Value evalSlice(const SliceExpr& expr, Env& env);
  Value evalField(const FieldExpr& expr, Env& env);
  Value evalSafeNav(const SafeNavExpr& expr, Env& env);
  Value evalArray(const ArrayLitExpr& expr, Env& env);
  Value evalTuple(const TupleExpr& expr, Env& env);
  Value evalLambda(const LambdaExpr& expr, Env& env);
  Value evalMatch(const MatchExpr& expr, Env& env);
  Value evalComprehension(const ComprehensionExpr& expr, Env& env);
  Value applyBinary(BinOp op, const Value& lhs, const Value& rhs, Span span);
  bool valuesEqual(const Value& lhs, const Value& rhs);

  LValue resolveLValue(const Expr& target, Env& env);
  Value readLValue(const LValue& lv);
  void writeLValue(const LValue& lv, Value value);

  Value callFunction(const FnDecl& decl, std::vector<Value> args, Span span);
  Value callClosure(const ClosurePtr& closure, std::vector<Value> args, Span span);
  Value callValue(const Value& callee, std::vector<Value> args, Span span);
  Value callMethod(const StructPtr& object, const std::string& methodName,
                   std::vector<Value> args, Span span);
  Value callModuleMember(const ModuleObj& module, const std::string& member,
                         const std::vector<CallArg>& args, Env& env, Span span);
  Value constructStruct(const StructDecl& decl, const std::vector<CallArg>& args, Env& env,
                        Span span);
  Value callBuiltin(const std::string& name, std::vector<Value>& args, Span span);
  Value callBuiltinMethod(const Value& object, const std::string& method,
                          std::vector<Value>& args, Span span);

  const StructDecl* findStruct(const std::string& name) const;
  std::string qualifiedName(const std::string& name) const;
  std::string qualifiedName(int moduleId, const std::string& name) const;
  std::string shortStructName(const std::string& name) const;
  Env& moduleEnv(int moduleId);
  Binding* moduleBinding(const ModuleObj& module, const std::string& member);
  void bindImport(const ImportStmt& stmt, Env& env);
  void bindFromImport(const FromImportStmt& stmt, Env& env);

  std::int64_t expectInt(const Value& value, Span span, const std::string& what);
  double expectFloat(const Value& value, Span span, const std::string& what);
  bool expectBool(const Value& value, Span span, const std::string& what);
  char32_t expectChar(const Value& value, Span span, const std::string& what);
  const std::string& expectString(const Value& value, Span span, const std::string& what);
  ArrayPtr expectArray(const Value& value, Span span, const std::string& what);

  Value convertNumber(const std::string& typeName, const Value& value, Span span);
  std::int64_t normalizeIndex(std::int64_t index, std::size_t length, Span span);

  const ModuleGraph& graph_;
  const SemaResult& sema_;
  DiagnosticSink& sink_;
  std::istream& in_;
  std::ostream& out_;
  std::ostream& err_;

  std::deque<Env> moduleEnvs_;
  int currentModule_ = -1;
  int callDepth_ = 0;
};

}  // namespace a
