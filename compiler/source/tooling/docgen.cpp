#include "tooling/docgen.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <utility>

#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "tooling/ast_text.hpp"
#include "tooling/source_text.hpp"

namespace a::tooling {
namespace {

const char* kindName(DocItem::Kind kind) {
  switch (kind) {
    case DocItem::Kind::Function: return "function";
    case DocItem::Kind::Class: return "class";
    case DocItem::Kind::Struct: return "struct";
    case DocItem::Kind::Metaclass: return "metaclass";
    case DocItem::Kind::Enum: return "enum";
    case DocItem::Kind::TypeAlias: return "type";
    case DocItem::Kind::Constant: return "constant";
    case DocItem::Kind::Variable: return "variable";
  }
  return "item";
}

std::string moduleStem(const std::string& path) {
  std::size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name.empty() ? "module" : name;
}

std::string stripDocComment(const std::string& text) {
  std::string body = text;
  while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) body.pop_back();
  if (body.rfind("##", 0) == 0) body.erase(0, 2);
  if (!body.empty() && body.front() == ' ') body.erase(0, 1);
  return body;
}

std::string trimText(std::string text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

using DocMap = std::map<int, std::string>;

DocMap extractDocMap(const std::vector<Comment>& comments) {
  DocMap docs;
  std::size_t i = 0;
  while (i < comments.size()) {
    const Comment& comment = comments[i];
    if (!comment.doc || comment.trailing) {
      ++i;
      continue;
    }
    int lastLine = comment.span.line - 1;
    std::string text;
    while (i < comments.size() && comments[i].doc && !comments[i].trailing &&
           comments[i].span.line == lastLine + 1) {
      text += stripDocComment(comments[i].text) + "\n";
      lastLine = comments[i].span.endLine > 0 ? comments[i].span.endLine : comments[i].span.line;
      ++i;
    }
    docs[lastLine + 1] = trimText(text);
  }
  return docs;
}

const std::string& docFor(const DocMap& docs, const Span& span) {
  static const std::string empty;
  if (!span.valid()) return empty;
  auto it = docs.find(span.line);
  return it == docs.end() ? empty : it->second;
}

DocItem functionItem(const FnDecl& decl, const DocMap& docs) {
  DocItem item;
  item.kind = DocItem::Kind::Function;
  item.name = decl.name;
  item.signature = signatureText(decl);
  item.doc = docFor(docs, decl.span);
  item.returnType = typeText(decl.returnType.get());
  for (const ParamDecl& param : decl.params) {
    DocParameter documented;
    documented.name = param.name;
    documented.type = typeText(param.type.get());
    item.params.push_back(std::move(documented));
  }
  return item;
}

std::string structSignature(const StructDecl& decl, const std::string& keyword) {
  std::string text = keyword + " " + decl.name + typeParamsText(decl.typeParams);
  std::string parents;
  if (!decl.baseClass.empty()) parents += decl.baseClass;
  if (!decl.metaclass.empty()) {
    if (!parents.empty()) parents += ", ";
    parents += "metaclass=" + decl.metaclass;
  }
  if (!parents.empty()) text += "(" + parents + ")";
  return text;
}

DocItem structItem(const StructDecl& decl, const DocMap& docs, const SourceText& source) {
  DocItem item;
  const std::string keyword = declarationKeyword(source, decl.span, decl.nameSpan);
  if (keyword == "struct") {
    item.kind = DocItem::Kind::Struct;
  } else if (keyword == "metaclass") {
    item.kind = DocItem::Kind::Metaclass;
  } else {
    item.kind = DocItem::Kind::Class;
  }
  item.name = decl.name;
  item.signature = structSignature(decl, keyword.empty() ? (decl.isClass ? "class" : "struct")
                                                         : keyword);
  item.doc = docFor(docs, decl.span);

  for (const FieldDecl& field : decl.fields) {
    DocMember member;
    member.name = field.name;
    member.type = typeText(field.type.get());
    member.signature = field.name + ": " + member.type;
    member.doc = docFor(docs, field.span);
    item.fields.push_back(std::move(member));
  }
  for (const auto& method : decl.methods) {
    if (!method) continue;
    item.methods.push_back(functionItem(*method, docs));
  }
  return item;
}

DocItem enumItem(const EnumDecl& decl, const DocMap& docs) {
  DocItem item;
  item.kind = DocItem::Kind::Enum;
  item.name = decl.name;
  item.signature = "enum " + decl.name;
  item.doc = docFor(docs, decl.span);
  for (const EnumVariant& variant : decl.variants) {
    DocMember member;
    member.name = variant.name;
    member.signature = variant.name;
    member.doc = docFor(docs, variant.span);
    item.variants.push_back(std::move(member));
  }
  return item;
}

DocItem letItem(const LetStmt& decl, const DocMap& docs, const SourceText& source,
                bool topLevel) {
  DocItem item;
  const std::string keyword = declarationKeyword(source, decl.span, decl.nameSpan);
  item.name = decl.name;
  if (keyword == "const" || keyword == "pvt" || decl.constantBinding) {
    item.kind = DocItem::Kind::Constant;
    item.isPrivate = keyword == "pvt";
  } else if (decl.mutableBinding || keyword == "var") {
    item.kind = DocItem::Kind::Variable;
  } else {
    item.kind = DocItem::Kind::Constant;
  }
  (void)topLevel;

  std::string signature;
  if (keyword == "pvt") {
    signature = "pvt " + decl.name;
  } else if (keyword == "const" || decl.constantBinding) {
    signature = "const " + decl.name;
  } else if (decl.mutableBinding || keyword == "var") {
    signature = "var " + decl.name;
  } else {
    signature = decl.name;
  }
  if (decl.type) signature += ": " + typeText(decl.type.get());
  item.signature = signature;
  item.doc = docFor(docs, decl.span);
  item.returnType = typeText(decl.type.get());
  return item;
}


void renderDoc(std::ostringstream& out, const std::string& doc) {
  if (doc.empty()) return;
  out << doc << "\n\n";
}

void renderFunction(std::ostringstream& out, const DocItem& item, int headingLevel) {
  const std::string hashes(static_cast<std::size_t>(headingLevel), '#');
  out << hashes << " `" << item.name << "`\n\n";
  out << "```a\n" << item.signature << "\n```\n\n";
  renderDoc(out, item.doc);
  if (!item.params.empty()) {
    out << "**Parameters**\n\n";
    for (const DocParameter& param : item.params) {
      out << "- `" << param.name << "`";
      if (!param.type.empty()) out << " (`" << param.type << "`)";
      out << "\n";
    }
    out << "\n";
  }
}

void renderMembers(std::ostringstream& out, const std::string& title,
                   const std::vector<DocMember>& members) {
  if (members.empty()) return;
  out << "**" << title << "**\n\n";
  for (const DocMember& member : members) {
    out << "- `" << member.signature << "`";
    if (!member.doc.empty()) {
      std::string doc = member.doc;
      std::replace(doc.begin(), doc.end(), '\n', ' ');
      out << " — " << doc;
    }
    out << "\n";
  }
  out << "\n";
}

void renderTypeItem(std::ostringstream& out, const DocItem& item) {
  out << "### `" << item.name << "`\n\n";
  out << "```a\n" << item.signature << "\n```\n\n";
  renderDoc(out, item.doc);
  renderMembers(out, "Fields", item.fields);
  renderMembers(out, "Variants", item.variants);

  for (const DocItem& method : item.methods) {
    out << "#### `" << method.name << "`\n\n";
    out << "```a\n" << method.signature << "\n```\n\n";
    renderDoc(out, method.doc);
    if (!method.params.empty()) {
      out << "**Parameters**\n\n";
      for (const DocParameter& param : method.params) {
        out << "- `" << param.name << "`";
        if (!param.type.empty()) out << " (`" << param.type << "`)";
        out << "\n";
      }
      out << "\n";
    }
  }
}

std::string escapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

void renderJsonMembers(std::ostringstream& out, const std::vector<DocMember>& members) {
  out << "[";
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i) out << ",";
    out << "{\"name\":\"" << escapeJson(members[i].name) << "\",\"signature\":\""
        << escapeJson(members[i].signature) << "\",\"type\":\"" << escapeJson(members[i].type)
        << "\",\"doc\":\"" << escapeJson(members[i].doc) << "\"}";
  }
  out << "]";
}

void renderJsonItem(std::ostringstream& out, const DocItem& item) {
  out << "{\"kind\":\"" << kindName(item.kind) << "\",\"name\":\"" << escapeJson(item.name)
      << "\",\"signature\":\"" << escapeJson(item.signature) << "\",\"doc\":\""
      << escapeJson(item.doc) << "\",\"returnType\":\"" << escapeJson(item.returnType)
      << "\",\"private\":" << (item.isPrivate ? "true" : "false");

  out << ",\"params\":[";
  for (std::size_t i = 0; i < item.params.size(); ++i) {
    if (i) out << ",";
    out << "{\"name\":\"" << escapeJson(item.params[i].name) << "\",\"type\":\""
        << escapeJson(item.params[i].type) << "\"}";
  }
  out << "],\"fields\":";
  renderJsonMembers(out, item.fields);
  out << ",\"variants\":";
  renderJsonMembers(out, item.variants);
  out << ",\"methods\":[";
  for (std::size_t i = 0; i < item.methods.size(); ++i) {
    if (i) out << ",";
    renderJsonItem(out, item.methods[i]);
  }
  out << "]}";
}

}  // namespace

DocResult extractDocs(const std::string& source, DiagnosticSink& sink,
                      const DocOptions& options) {
  DocResult result;

  Lexer lexer(source, sink, 0);
  std::vector<Token> tokens = lexer.tokenize();
  std::vector<Comment> comments = lexer.takeComments();
  if (sink.hasErrors()) return result;

  Parser parser(std::move(tokens), sink);
  std::unique_ptr<Program> program = parser.parseProgram();
  if (!program || sink.hasErrors()) return result;

  SourceText sourceView(source);
  DocMap docs = extractDocMap(comments);

  result.module.name = options.moduleName.empty() ? moduleStem(options.sourceName)
                                                  : options.moduleName;
  result.module.path = options.sourceName;

  int firstDeclarationLine = 0;
  for (const auto& statement : program->statements) {
    if (statement && statement->span.valid()) {
      firstDeclarationLine = statement->span.line;
      break;
    }
  }
  for (const auto& entry : docs) {
    if (firstDeclarationLine == 0 || entry.first < firstDeclarationLine) {
      if (!result.module.doc.empty()) result.module.doc += "\n\n";
      result.module.doc += entry.second;
    }
  }

  for (const auto& statement : program->statements) {
    if (!statement) continue;
    switch (statement->kind) {
      case StmtKind::FnDecl:
        result.module.items.push_back(
            functionItem(static_cast<const FnDecl&>(*statement), docs));
        break;
      case StmtKind::StructDecl:
        result.module.items.push_back(
            structItem(static_cast<const StructDecl&>(*statement), docs, sourceView));
        break;
      case StmtKind::EnumDecl:
        result.module.items.push_back(enumItem(static_cast<const EnumDecl&>(*statement), docs));
        break;
      case StmtKind::TypeAlias: {
        const auto& alias = static_cast<const TypeAliasDecl&>(*statement);
        DocItem item;
        item.kind = DocItem::Kind::TypeAlias;
        item.name = alias.name;
        item.signature = "type " + alias.name + " = " + typeText(alias.type.get());
        item.doc = docFor(docs, alias.span);
        result.module.items.push_back(std::move(item));
        break;
      }
      case StmtKind::Let:
        result.module.items.push_back(
            letItem(static_cast<const LetStmt&>(*statement), docs, sourceView, true));
        break;
      case StmtKind::Expr: {
        const auto& expr = static_cast<const ExprStmt&>(*statement);
        if (sourceView.slice(statement->span) == "pass") break;
        (void)expr;
        break;
      }
      default:
        break;
    }
  }

  result.ok = true;
  return result;
}

std::string renderMarkdown(const DocModule& module) {
  std::ostringstream out;
  out << "# " << module.name << "\n\n";
  if (!module.path.empty()) {
    out << "> Generated from `" << module.path << "` by `a doc`.\n\n";
  }
  renderDoc(out, module.doc);

  std::vector<const DocItem*> functions;
  std::vector<const DocItem*> types;
  std::vector<const DocItem*> values;
  for (const DocItem& item : module.items) {
    switch (item.kind) {
      case DocItem::Kind::Function: functions.push_back(&item); break;
      case DocItem::Kind::Class:
      case DocItem::Kind::Struct:
      case DocItem::Kind::Metaclass:
      case DocItem::Kind::Enum:
      case DocItem::Kind::TypeAlias: types.push_back(&item); break;
      default: values.push_back(&item); break;
    }
  }

  if (!functions.empty() || !types.empty() || !values.empty()) {
    out << "## Contents\n\n";
    if (!functions.empty()) out << "- [Functions](#functions)\n";
    if (!types.empty()) out << "- [Types](#types)\n";
    if (!values.empty()) out << "- [Constants and variables](#constants-and-variables)\n";
    out << "\n";
  }

  if (!functions.empty()) {
    out << "## Functions\n\n";
    for (const DocItem* item : functions) {
      renderFunction(out, *item, 3);
    }
  }
  if (!types.empty()) {
    out << "## Types\n\n";
    for (const DocItem* item : types) renderTypeItem(out, *item);
  }
  if (!values.empty()) {
    out << "## Constants and variables\n\n";
    for (const DocItem* item : values) {
      out << "### `" << item->name << "`\n\n";
      out << "```a\n" << item->signature << "\n```\n\n";
      renderDoc(out, item->doc);
    }
  }

  std::string text = out.str();
  while (!text.empty() && text.back() == '\n') text.pop_back();
  if (!text.empty()) text += '\n';
  return text;
}

std::string renderJson(const DocModule& module) {
  std::ostringstream out;
  out << "{\"module\":\"" << escapeJson(module.name) << "\",\"path\":\""
      << escapeJson(module.path) << "\",\"doc\":\"" << escapeJson(module.doc)
      << "\",\"items\":[";
  for (std::size_t i = 0; i < module.items.size(); ++i) {
    if (i) out << ",";
    renderJsonItem(out, module.items[i]);
  }
  out << "]}\n";
  return out.str();
}

}  // namespace a::tooling
