#include "tooling/formatter.hpp"

#include <algorithm>
#include <climits>
#include <sstream>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "tooling/ast_text.hpp"
#include "tooling/source_text.hpp"

namespace a::tooling {
namespace {

int binaryPrecedence(std::string_view op) {
  if (op == "|>") return 1;
  if (op == "??") return 3;
  if (op == "or") return 4;
  if (op == "and") return 5;
  if (op == "in" || op == "not in" || op == "==" || op == "!=" || op == "<" ||
      op == ">" || op == "<=" || op == ">=") {
    return 7;
  }
  if (op == "..") return 8;
  if (op == "|") return 9;
  if (op == "^") return 10;
  if (op == "&") return 11;
  if (op == "<<" || op == ">>") return 12;
  if (op == "+" || op == "-") return 13;
  if (op == "*" || op == "/" || op == "%") return 14;
  if (op == "**") return 16;
  return 20;
}

bool isBlockStatement(const Stmt& statement) {
  switch (statement.kind) {
    case StmtKind::FnDecl:
    case StmtKind::StructDecl:
    case StmtKind::EnumDecl:
    case StmtKind::If:
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
    case StmtKind::Loop:
      return true;
    default:
      return false;
  }
}

class Formatter {
 public:
  Formatter(std::string source, std::vector<Comment> comments, FormatOptions options)
      : source_(std::move(source)),
        comments_(std::move(comments)),
        options_(options) {
    if (options_.indentWidth < 1) options_.indentWidth = 4;
    if (options_.maxLineWidth < 20) options_.maxLineWidth = 100;
  }

  std::string run(Program& program) {
    emitStatements(program.statements, 0, INT_MAX / 4, 0);

    std::size_t end = output_.size();
    while (end > 0 && (output_[end - 1] == '\n' || output_[end - 1] == '\r')) --end;
    output_.resize(end);
    if (!output_.empty()) output_ += '\n';
    return output_;
  }

 private:
  // ------------------------------------------------------------- comments

  std::string indentText(int level) const {
    return std::string(static_cast<std::size_t>(std::max(0, level) * options_.indentWidth), ' ');
  }

  void ensureGap(int line, int& lastLine) {
    if (lastLine > 0 && line > lastLine + 1 && !output_.empty()) {
      if (!(output_.size() >= 2 && output_[output_.size() - 2] == '\n')) output_ += '\n';
    }
  }

  void emitComment(const Comment& comment, int level, int& lastLine) {
    ensureGap(comment.span.line, lastLine);
    std::string text = comment.text;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    output_ += indentText(level) + text + '\n';
    lastLine = comment.span.endLine > 0 ? comment.span.endLine : comment.span.line;
  }

  // Emits pending comments whose source line is strictly before `line`.
  void emitLeadingComments(int line, int level, int& lastLine) {
    while (cursor_ < comments_.size() && comments_[cursor_].span.line < line) {
      emitComment(comments_[cursor_], level, lastLine);
      ++cursor_;
    }
  }

  // Emits pending comments inside a statement's source range before the
  // statement itself. This keeps comments that were written inside multiline
  // expressions from being lost when the expression is collapsed.
  void emitInteriorComments(const Span& span, int level, int& lastLine) {
    if (!span.valid()) return;
    while (cursor_ < comments_.size() && comments_[cursor_].span.line < span.endLine) {
      emitComment(comments_[cursor_], level, lastLine);
      ++cursor_;
    }
  }

  bool pendingTrailingComment(const Span& span, int& commentIndex) const {
    if (!span.valid() || cursor_ >= comments_.size()) return false;
    const Comment& comment = comments_[cursor_];
    if (comment.span.line != span.endLine || comment.span.col < span.endCol) return false;
    commentIndex = static_cast<int>(cursor_);
    return true;
  }

  void attachTrailing(const Span& span, int& lastLine) {
    int index = 0;
    if (!pendingTrailingComment(span, index)) return;
    std::string text = comments_[static_cast<std::size_t>(index)].text;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    if (!output_.empty() && output_.back() == '\n') output_.pop_back();
    if (!output_.empty() && output_.back() == '\r') output_.pop_back();
    output_ += "  " + text + '\n';
    lastLine = span.endLine;
    cursor_ = static_cast<std::size_t>(index) + 1;
  }

  void emitLeftoverComments(int limitLine, int level, int& lastLine) {
    const int minimumColumn = level * options_.indentWidth + 1;
    while (cursor_ < comments_.size()) {
      const Comment& comment = comments_[cursor_];
      if (comment.span.line >= limitLine) break;
      if (comment.span.line <= lastLine) {
        ++cursor_;
        continue;
      }
      if (comment.span.col < minimumColumn) break;
      emitComment(comment, level, lastLine);
      ++cursor_;
    }
  }

  // --------------------------------------------------------- statements

  int emitStatements(const std::vector<StmtPtr>& body, int level, int limitLine,
                     int headerLine) {
    int lastLine = headerLine;
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (!body[i]) continue;
      const int nextLine = i + 1 < body.size() && body[i + 1]->span.valid()
                               ? body[i + 1]->span.line
                               : limitLine;
      emitStatement(*body[i], level, nextLine, lastLine);
    }
    emitLeftoverComments(limitLine, level, lastLine);
    return lastLine;
  }

  void emitStatement(const Stmt& statement, int level, int nextLine, int& lastLine) {
    const Span& span = statement.span;

    if (isBlockStatement(statement)) {
      if (span.valid()) emitLeadingComments(span.line, level, lastLine);
      emitBlockStatement(statement, level, nextLine, lastLine);
      attachTrailing(span, lastLine);
      if (span.valid()) lastLine = std::max(lastLine, span.endLine);
      return;
    }

    if (span.valid()) {
      // Hoist comments written inside multiline simple statements so no source
      // text is lost when the expression is normalised.
      emitLeadingComments(span.line, level, lastLine);
      emitInteriorComments(span, level, lastLine);
      ensureGap(span.line, lastLine);
    }

    const std::string text = inlineStatementText(statement, level);
    output_ += indentText(level) + text + '\n';
    if (span.valid()) lastLine = span.endLine;

    attachTrailing(span, lastLine);
    if (span.valid()) lastLine = std::max(lastLine, span.endLine);
  }

  void emitBlockStatement(const Stmt& statement, int level, int nextLine, int& lastLine) {
    switch (statement.kind) {
      case StmtKind::FnDecl:
        emitFunction(static_cast<const FnDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::StructDecl:
        emitStruct(static_cast<const StructDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::EnumDecl:
        emitEnum(static_cast<const EnumDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::If:
        emitIf(static_cast<const IfStmt&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(statement);
        emitHeader("while " + exprText(*loop.cond, 0, level) + ":", level, statement.span,
                   lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine, loop.cond->span.endLine);
        return;
      }
      case StmtKind::For: {
        const auto& loop = static_cast<const ForStmt&>(statement);
        emitHeader("for " + loop.var + " in " + exprText(*loop.iterable, 0, level) + ":",
                   level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine,
                                  loop.iterable->span.endLine);
        return;
      }
      case StmtKind::Loop: {
        const auto& loop = static_cast<const LoopStmt&>(statement);
        emitHeader("loop:", level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine, statement.span.line);
        return;
      }
      case StmtKind::DoWhile: {
        const auto& loop = static_cast<const DoWhileStmt&>(statement);
        emitHeader("do:", level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, loop.cond->span.line,
                                  statement.span.line);
        ensureGap(loop.cond->span.line, lastLine);
        output_ += indentText(level) + "while " + exprText(*loop.cond, 0, level) + '\n';
        lastLine = loop.cond->span.endLine;
        return;
      }
      default:
        return;
    }
  }

  void emitHeader(const std::string& text, int level, const Span& span, int& lastLine) {
    if (span.valid()) ensureGap(span.line, lastLine);
    output_ += indentText(level) + text + '\n';
    if (span.valid()) lastLine = span.line;
    // A comment written after the header (for example `fn f():  # helper`)
    // stays on the header line.
    if (cursor_ < comments_.size() && comments_[cursor_].trailing &&
        comments_[cursor_].span.line == span.line) {
      std::string comment = comments_[cursor_].text;
      while (!comment.empty() && (comment.back() == '\r' || comment.back() == '\n')) {
        comment.pop_back();
      }
      output_.pop_back();
      output_ += "  " + comment + '\n';
      ++cursor_;
    }
  }

  void emitFunction(const FnDecl& decl, int level, int nextLine, int& lastLine) {
    std::string header = signatureText(decl) + ":";
    emitHeader(header, level, decl.span, lastLine);
    lastLine = emitStatements(decl.body, level + 1, nextLine, decl.span.line);
  }

  void emitIf(const IfStmt& statement, int level, int nextLine, int& lastLine) {
    for (std::size_t i = 0; i < statement.branches.size(); ++i) {
      const IfStmt::Branch& branch = statement.branches[i];
      const std::string keyword = i == 0 ? "if" : "elif";
      std::string header = keyword + " " + exprText(*branch.cond, 0, level) + ":";
      if (i == 0) {
        emitHeader(header, level, statement.span, lastLine);
      } else {
        ensureGap(branch.cond->span.line, lastLine);
        output_ += indentText(level) + header + '\n';
        lastLine = branch.cond->span.endLine;
      }

      int limit = nextLine;
      if (i + 1 < statement.branches.size()) {
        limit = statement.branches[i + 1].cond->span.line;
      } else if (!statement.elseBody.empty() && statement.elseBody.front()->span.valid()) {
        limit = statement.elseBody.front()->span.line;
      }
      lastLine = emitStatements(branch.body, level + 1, limit, branch.cond->span.endLine);
    }

    if (!statement.elseBody.empty()) {
      const int elseLine = statement.elseBody.front()->span.valid()
                               ? statement.elseBody.front()->span.line
                               : lastLine + 1;
      ensureGap(elseLine, lastLine);
      output_ += indentText(level) + "else:\n";
      lastLine = elseLine;
      lastLine = emitStatements(statement.elseBody, level + 1, nextLine, elseLine);
    }
  }

  void emitStruct(const StructDecl& decl, int level, int nextLine, int& lastLine) {
    const std::string keyword = declarationKeyword(source_, decl.span, decl.nameSpan);
    std::string header = keyword.empty() ? (decl.isClass ? "class" : "struct") : keyword;
    header += " " + decl.name + typeParamsText(decl.typeParams);

    std::string parents;
    if (!decl.baseClass.empty()) parents += decl.baseClass;
    if (!decl.metaclass.empty()) {
      if (!parents.empty()) parents += ", ";
      parents += "metaclass=" + decl.metaclass;
    }
    if (!parents.empty()) header += "(" + parents + ")";
    header += ":";
    emitHeader(header, level, decl.span, lastLine);

    struct Member {
      const FieldDecl* field = nullptr;
      const FnDecl* method = nullptr;
      int line = 0;
    };
    std::vector<Member> members;
    members.reserve(decl.fields.size() + decl.methods.size());
    for (const FieldDecl& field : decl.fields) {
      members.push_back(Member{&field, nullptr, field.span.line});
    }
    for (const auto& method : decl.methods) {
      if (method) members.push_back(Member{nullptr, method.get(), method->span.line});
    }
    std::stable_sort(members.begin(), members.end(),
                     [](const Member& a, const Member& b) { return a.line < b.line; });

    if (members.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }

    for (std::size_t i = 0; i < members.size(); ++i) {
      const Member& member = members[i];
      const int memberNext = i + 1 < members.size() ? members[i + 1].line : nextLine;
      if (member.field) {
        emitLeadingComments(member.line, level + 1, lastLine);
        ensureGap(member.line, lastLine);
        output_ += indentText(level + 1) + member.field->name + ": " +
                   typeText(member.field->type.get()) + '\n';
        lastLine = member.field->span.endLine;
        attachTrailing(member.field->span, lastLine);
      } else if (member.method) {
        emitStatement(*member.method, level + 1, memberNext, lastLine);
      }
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

  void emitEnum(const EnumDecl& decl, int level, int nextLine, int& lastLine) {
    emitHeader("enum " + decl.name + ":", level, decl.span, lastLine);
    if (decl.variants.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }
    for (std::size_t i = 0; i < decl.variants.size(); ++i) {
      const EnumVariant& variant = decl.variants[i];
      const int next = i + 1 < decl.variants.size() ? decl.variants[i + 1].span.line : nextLine;
      emitLeadingComments(variant.span.line, level + 1, lastLine);
      ensureGap(variant.span.line, lastLine);
      std::string text = variant.name;
      if (variant.value) text += " = " + exprText(*variant.value, 0, level + 1);
      output_ += indentText(level + 1) + text + '\n';
      lastLine = variant.value ? variant.value->span.endLine : variant.span.endLine;
      attachTrailing(variant.span, lastLine);
      (void)next;
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

  std::string inlineStatementText(const Stmt& statement, int level) {
    switch (statement.kind) {
      case StmtKind::Let: {
        const auto& decl = static_cast<const LetStmt&>(statement);
        const std::string keyword = declarationKeyword(source_, decl.span, decl.nameSpan);
        std::string text;
        if (keyword == "pvt") {
          text = "pvt ";
        } else if (keyword == "const" || decl.constantBinding) {
          text = "const ";
        } else if (decl.mutableBinding || keyword == "var") {
          text = "var ";
        }
        text += decl.name;
        if (decl.type) text += ": " + typeText(decl.type.get());
        text += " = " + exprText(*decl.init, 0, level);
        return text;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(statement);
        return exprText(*assign.target, 0, level) + " " + assign.op + " " +
               exprText(*assign.value, 0, level);
      }
      case StmtKind::Expr: {
        const auto& expr = static_cast<const ExprStmt&>(statement);
        if (source_.slice(statement.span) == "pass") return "pass";
        if (!expr.expr) return "pass";
        return exprText(*expr.expr, 0, level);
      }
      case StmtKind::Destructure: {
        const auto& decl = static_cast<const DestructureStmt&>(statement);
        std::string text;
        if (source_.firstChar(decl.span) != '(') text = "var ";
        text += "(";
        for (std::size_t i = 0; i < decl.names.size(); ++i) {
          if (i) text += ", ";
          text += decl.names[i];
        }
        if (decl.names.size() == 1) text += ",";
        text += ") = " + exprText(*decl.value, 0, level);
        return text;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(statement);
        if (!ret.value) return "return";
        return "return " + exprText(*ret.value, 0, level);
      }
      case StmtKind::Break:
        return "break";
      case StmtKind::Continue:
        return "continue";
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(statement);
        if (!defer.body) return "defer pass";
        return "defer " + inlineStatementText(*defer.body, level);
      }
      case StmtKind::Import: {
        const auto& import = static_cast<const ImportStmt&>(statement);
        std::string text = "import ";
        const std::string target = source_.slice(import.targetSpan);
        text += target.empty() ? (import.quoted ? "\"" + import.path + "\"" : import.bareName)
                               : target;
        if (!import.alias.empty()) text += " as " + import.alias;
        return text;
      }
      case StmtKind::FromImport: {
        const auto& import = static_cast<const FromImportStmt&>(statement);
        std::string text = "from ";
        const std::string target = source_.slice(import.targetSpan);
        text += target.empty() ? (import.quoted ? "\"" + import.path + "\"" : import.bareName)
                               : target;
        text += " import ";
        for (std::size_t i = 0; i < import.items.size(); ++i) {
          if (i) text += ", ";
          text += import.items[i].name;
          if (!import.items[i].alias.empty() && import.items[i].alias != import.items[i].name) {
            text += " as " + import.items[i].alias;
          }
        }
        return text;
      }
      default:
        return source_.slice(statement.span);
    }
  }

  // -------------------------------------------------------- expressions

  std::string exprText(const Expr& expr, int parentPrecedence, int indent) {
    switch (expr.kind) {
      case ExprKind::IntLit:
      case ExprKind::FloatLit:
      case ExprKind::CharLit: {
        const std::string literal = source_.slice(expr.span);
        if (!literal.empty()) return literal;
        if (expr.kind == ExprKind::IntLit) {
          return std::to_string(static_cast<const IntLitExpr&>(expr).value);
        }
        if (expr.kind == ExprKind::FloatLit) {
          return std::to_string(static_cast<const FloatLitExpr&>(expr).value);
        }
        return "0";
      }
      case ExprKind::BoolLit:
        return static_cast<const BoolLitExpr&>(expr).value ? "true" : "false";
      case ExprKind::StringLit: {
        const std::string literal = source_.slice(expr.span);
        if (!literal.empty()) return literal;
        return "\"\"";
      }
      case ExprKind::Ident:
        return static_cast<const IdentExpr&>(expr).name;
      case ExprKind::Unary:
        return unaryText(static_cast<const UnaryExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Binary:
        return binaryText(static_cast<const BinaryExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Call:
        return callText(static_cast<const CallExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Index: {
        const auto& index = static_cast<const IndexExpr&>(expr);
        return postfixWrap(exprText(*index.object, 17, indent) + "[" +
                               exprText(*index.index, 0, indent) + "]",
                           parentPrecedence, 17);
      }
      case ExprKind::Slice:
        return sliceText(static_cast<const SliceExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Field: {
        const auto& field = static_cast<const FieldExpr&>(expr);
        return postfixWrap(exprText(*field.object, 17, indent) + "." + field.field,
                           parentPrecedence, 17);
      }
      case ExprKind::SafeNav: {
        const auto& field = static_cast<const SafeNavExpr&>(expr);
        return postfixWrap(exprText(*field.object, 17, indent) + "?." + field.field,
                           parentPrecedence, 17);
      }
      case ExprKind::ArrayLit:
        return arrayText(static_cast<const ArrayLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::MapLit:
        return mapText(static_cast<const MapLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::SetLit:
        return setText(static_cast<const SetLitExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Range:
        return rangeText(static_cast<const RangeExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Ternary: {
        const auto& ternary = static_cast<const TernaryExpr&>(expr);
        const std::string text = exprText(*ternary.cond, 2, indent) + " ? " +
                                 exprText(*ternary.thenValue, 0, indent) + " : " +
                                 exprText(*ternary.elseValue, 2, indent);
        return postfixWrap(text, parentPrecedence, 2);
      }
      case ExprKind::Tuple:
        return tupleText(static_cast<const TupleExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Lambda:
        return lambdaText(static_cast<const LambdaExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Comprehension: {
        const auto& comp = static_cast<const ComprehensionExpr&>(expr);
        std::string text = std::string(comp.isSet ? "{" : "[") +
                           exprText(*comp.element, 0, indent) + " for " + comp.var +
                           " in " + exprText(*comp.iterable, 0, indent);
        if (comp.condition) text += " if " + exprText(*comp.condition, 0, indent);
        text += comp.isSet ? "}" : "]";
        return postfixWrap(text, parentPrecedence, 17);
      }
      case ExprKind::Match:
        return matchText(static_cast<const MatchExpr&>(expr), parentPrecedence, indent);
      case ExprKind::Try: {
        const auto& tryExpr = static_cast<const TryExpr&>(expr);
        return postfixWrap(exprText(*tryExpr.operand, 17, indent) + "?", parentPrecedence, 17);
      }
      default:
        break;
    }
    return {};
  }

  static std::string postfixWrap(const std::string& text, int parent, int precedence) {
    if (precedence < parent) return "(" + text + ")";
    return text;
  }

  std::string unaryText(const UnaryExpr& unary, int parent, int indent) {
    if (unary.op == "!") {
      return postfixWrap(exprText(*unary.operand, 17, indent) + "!", parent, 17);
    }
    if (unary.postfix) {
      return postfixWrap(exprText(*unary.operand, 17, indent) + unary.op, parent, 17);
    }
    if (unary.op == "not") {
      const std::string text = "not " + exprText(*unary.operand, 6, indent);
      return postfixWrap(text, parent, 6);
    }
    const std::string text = unary.op + exprText(*unary.operand, 15, indent);
    return postfixWrap(text, parent, 15);
  }

  std::string binaryText(const BinaryExpr& binary, int parent, int indent) {
    const int precedence = binaryPrecedence(binary.op);
    const bool rightAssociative = binary.op == "**";
    const int lhsParent = rightAssociative ? precedence + 1 : precedence;
    const int rhsParent = rightAssociative ? precedence : precedence + 1;
    const std::string text = exprText(*binary.lhs, lhsParent, indent) + " " + binary.op + " " +
                             exprText(*binary.rhs, rhsParent, indent);
    return postfixWrap(text, parent, precedence);
  }

  std::string callText(const CallExpr& call, int parent, int indent) {
    const std::string callee = exprText(*call.callee, 17, indent);
    std::vector<std::string> args;
    args.reserve(call.args.size());
    for (const CallArg& arg : call.args) {
      std::string text;
      if (!arg.name.empty()) text = arg.name + "=";
      text += exprText(*arg.value, 0, indent + 1);
      args.push_back(std::move(text));
    }

    const std::string flat = joinList(callee + "(", args, ")");
    bool multiline = false;
    for (const std::string& arg : args) {
      if (arg.find('\n') != std::string::npos) multiline = true;
    }
    if (static_cast<int>(flat.size() + indent * options_.indentWidth) > options_.maxLineWidth) {
      multiline = true;
    }

    std::string text;
    if (multiline && !args.empty()) {
      text = callee + "(\n";
      for (std::string& arg : args) {
        reindentContinuation(arg, indent + 1);
        text += indentText(indent + 1) + arg + ",\n";
      }
      text += indentText(indent) + ")";
    } else {
      text = flat;
    }
    return postfixWrap(text, parent, 17);
  }

  std::string enclosedText(const std::string& open, const std::string& close,
                           const std::vector<std::string>& parts, int parent, int indent) {
    const std::string flat = open + joinList("", parts, "") + close;
    bool multiline = false;
    for (const std::string& part : parts) {
      if (part.find('\n') != std::string::npos) multiline = true;
    }
    if (static_cast<int>(flat.size() + indent * options_.indentWidth) > options_.maxLineWidth) {
      multiline = true;
    }

    std::string text;
    if (multiline && !parts.empty()) {
      text = open + "\n";
      for (const std::string& part : parts) {
        text += indentText(indent + 1) + part + ",\n";
      }
      text += indentText(indent) + close;
    } else {
      text = flat;
    }
    return postfixWrap(text, parent, 17);
  }

  std::string arrayText(const ArrayLitExpr& array, int parent, int indent) {
    std::vector<std::string> elements;
    elements.reserve(array.elements.size());
    for (const auto& element : array.elements) {
      elements.push_back(exprText(*element, 0, indent + 1));
    }
    return enclosedText("[", "]", elements, parent, indent);
  }

  std::string mapText(const MapLitExpr& map, int parent, int indent) {
    std::vector<std::string> entries;
    entries.reserve(map.entries.size());
    for (const auto& entry : map.entries) {
      entries.push_back(exprText(*entry.first, 0, indent + 1) + ": " +
                        exprText(*entry.second, 0, indent + 1));
    }
    return enclosedText("{", "}", entries, parent, indent);
  }

  std::string setText(const SetLitExpr& set, int parent, int indent) {
    std::vector<std::string> elements;
    elements.reserve(set.elements.size());
    for (const auto& element : set.elements) {
      elements.push_back(exprText(*element, 0, indent + 1));
    }
    return enclosedText("{", "}", elements, parent, indent);
  }

  std::string tupleText(const TupleExpr& tuple, int parent, int indent) {
    std::string text = "(";
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
      if (i) text += ", ";
      text += exprText(*tuple.elements[i], 0, indent);
    }
    if (tuple.elements.size() == 1) text += ",";
    text += ")";
    return postfixWrap(text, parent, 17);
  }

  std::string rangeText(const RangeExpr& range, int parent, int indent) {
    std::string text;
    if (range.start) text += exprText(*range.start, 9, indent);
    text += "..";
    if (range.end) text += exprText(*range.end, 9, indent);
    if (range.step) text += ".." + exprText(*range.step, 9, indent);
    return postfixWrap(text, parent, 8);
  }

  std::string sliceText(const SliceExpr& slice, int parent, int indent) {
    std::string text = exprText(*slice.object, 17, indent) + "[";
    if (slice.start) text += exprText(*slice.start, 0, indent);
    text += "..";
    if (slice.end) text += exprText(*slice.end, 0, indent);
    if (slice.step) text += ".." + exprText(*slice.step, 0, indent);
    text += "]";
    return postfixWrap(text, parent, 17);
  }

  std::string lambdaText(const LambdaExpr& lambda, int parent, int indent) {
    std::string text = "fn" + paramsText(lambda.params);
    if (lambda.returnType) text += " -> " + typeText(lambda.returnType.get());
    text += " => " + exprText(*lambda.body, 0, indent);
    return postfixWrap(text, parent, 0);
  }

  std::string patternText(const Pattern& pattern, int indent) {
    if (pattern.kind == Pattern::Kind::Wildcard) return "_";
    if (pattern.kind == Pattern::Kind::Range) {
      std::string text;
      if (pattern.rangeStart) text += exprText(*pattern.rangeStart, 9, indent);
      text += "..";
      if (pattern.rangeEnd) text += exprText(*pattern.rangeEnd, 9, indent);
      return text;
    }
    return pattern.value ? exprText(*pattern.value, 0, indent) : "_";
  }

  std::string matchText(const MatchExpr& match, int parent, int indent) {
    std::string text = "match " + exprText(*match.subject, 0, indent) + ":\n";
    int lastLine = match.subject->span.valid() ? match.subject->span.endLine : 0;
    for (const MatchArm& arm : match.arms) {
      const int armLine = arm.pattern.span.valid() ? arm.pattern.span.line : lastLine + 1;
      if (lastLine > 0 && armLine > lastLine + 1) text += '\n';
      text += indentText(indent + 1) + patternText(arm.pattern, indent + 1);
      if (arm.guard) text += " if " + exprText(*arm.guard, 0, indent + 1);
      text += " => " + exprText(*arm.result, 0, indent + 1) + "\n";
      lastLine = arm.result && arm.result->span.valid() ? arm.result->span.endLine : armLine;
    }
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return postfixWrap(text, parent, 2);
  }

  static std::string joinList(const std::string& prefix, const std::vector<std::string>& items,
                              const std::string& suffix) {
    std::string text = prefix;
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i) text += ", ";
      text += items[i];
    }
    text += suffix;
    return text;
  }

  // Continuation lines of a nested expression already carry absolute
  // indentation; this only fixes the common case where a child expression
  // produced newlines relative to its own indent level.
  void reindentContinuation(std::string& text, int level) const {
    std::size_t position = 0;
    while ((position = text.find('\n', position)) != std::string::npos) {
      ++position;
      std::size_t spaces = 0;
      while (position + spaces < text.size() && text[position + spaces] == ' ') ++spaces;
      if (spaces == 0) {
        text.insert(position, indentText(level));
        position += static_cast<std::size_t>(level * options_.indentWidth);
      }
    }
  }

  SourceText source_;
  std::vector<Comment> comments_;
  FormatOptions options_;
  std::size_t cursor_ = 0;
  std::string output_;
};

}  // namespace

FormatResult formatSource(const std::string& source, DiagnosticSink& sink,
                          const FormatOptions& options) {
  FormatResult result;
  Lexer lexer(source, sink, 0);
  std::vector<Token> tokens = lexer.tokenize();
  std::vector<Comment> comments = lexer.takeComments();
  if (sink.hasErrors()) return result;

  Parser parser(std::move(tokens), sink);
  std::unique_ptr<Program> program = parser.parseProgram();
  if (!program || sink.hasErrors()) return result;

  Formatter formatter(source, std::move(comments), options);
  result.text = formatter.run(*program);
  result.ok = true;
  return result;
}

}  // namespace a::tooling
