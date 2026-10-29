#include "tooling/ast_text.hpp"

#include <sstream>

namespace a::tooling {
namespace {

std::string typeParamsTextImpl(const std::vector<std::string>& typeParams) {
  if (typeParams.empty()) return {};
  std::string text = "<";
  for (std::size_t i = 0; i < typeParams.size(); ++i) {
    if (i) text += ", ";
    text += typeParams[i];
  }
  text += ">";
  return text;
}

}  // namespace

std::string typeText(const TypeExpr* type) {
  if (!type) return {};
  if (const auto* named = dynamic_cast<const NamedTypeExpr*>(type)) {
    std::string text;
    for (const std::string& qualifier : named->qualifiers) {
      if (!text.empty()) text += ".";
      text += qualifier;
    }
    if (!text.empty()) text += ".";
    text += named->name;
    if (!named->typeArgs.empty()) {
      text += "<";
      for (std::size_t i = 0; i < named->typeArgs.size(); ++i) {
        if (i) text += ", ";
        text += typeText(named->typeArgs[i].get());
      }
      text += ">";
    }
    return text;
  }
  if (const auto* array = dynamic_cast<const ArrayTypeExpr*>(type)) {
    if (array->length < 0) {
      return "[" + typeText(array->element.get()) + "]";
    }
    // Fixed dimensions render as suffixes in source order: `i32[2][3]`.
    std::string dims;
    const TypeExpr* current = type;
    while (const auto* fixed = dynamic_cast<const ArrayTypeExpr*>(current)) {
      if (fixed->length < 0) break;
      dims += "[" + std::to_string(fixed->length) + "]";
      current = fixed->element.get();
    }
    return typeText(current) + dims;
  }
  if (const auto* optional = dynamic_cast<const OptionalTypeExpr*>(type)) {
    const std::string inner = typeText(optional->element.get());
    if (dynamic_cast<const OptionalTypeExpr*>(optional->element.get()) != nullptr) {
      return "(" + inner + ")?";
    }
    return inner + "?";
  }
  if (const auto* tuple = dynamic_cast<const TupleTypeExpr*>(type)) {
    std::string text = "(";
    for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
      if (i) text += ", ";
      text += typeText(tuple->elements[i].get());
    }
    return text + ")";
  }
  if (const auto* map = dynamic_cast<const MapTypeExpr*>(type)) {
    return "{" + typeText(map->key.get()) + ": " + typeText(map->value.get()) + "}";
  }
  if (const auto* set = dynamic_cast<const SetTypeExpr*>(type)) {
    return "{" + typeText(set->element.get()) + "}";
  }
  if (const auto* fn = dynamic_cast<const FunctionTypeExpr*>(type)) {
    std::string text = "fn(";
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
      if (i) text += ", ";
      text += typeText(fn->params[i].get());
    }
    text += ")";
    if (fn->returnType) text += " -> " + typeText(fn->returnType.get());
    return text;
  }
  return "?";
}

std::string paramText(const ParamDecl& param) {
  std::string text = param.name;
  if (param.type) text += ": " + typeText(param.type.get());
  return text;
}

std::string paramsText(const std::vector<ParamDecl>& params) {
  std::string text = "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i) text += ", ";
    text += paramText(params[i]);
  }
  text += ")";
  return text;
}

std::string typeParamsText(const std::vector<std::string>& typeParams) {
  return typeParamsTextImpl(typeParams);
}

std::string signatureText(const FnDecl& decl) {
  std::string text = "fn " + decl.name + typeParamsText(decl.typeParams) + paramsText(decl.params);
  if (decl.returnType) text += " -> " + typeText(decl.returnType.get());
  return text;
}

std::string declarationKeyword(const SourceText& source, const Span& declaration,
                               const Span& name) {
  const std::string prefix = source.between(declaration, name);
  std::istringstream stream(prefix);
  std::string first;
  stream >> first;
  if (first == "var" || first == "const" || first == "pvt" || first == "class" ||
      first == "struct" || first == "metaclass") {
    return first;
  }
  if (first == "Class") return "class";
  if (first == "Metaclass") return "metaclass";
  return {};
}

}  // namespace a::tooling
