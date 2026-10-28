#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lexer/span.hpp"

namespace a {

enum class TypeKind {
  Error,
  Void,
  Bool,
  Int,
  Float,
  Char,
  String,
  Array,
  Range,
  Optional,
  Tuple,
  Struct,
  Enum,
  Function,
  Module,
  Var,
};

enum class Constraint { None, Integer, Floating, Numeric };

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
  TypeKind kind = TypeKind::Error;
  int bits = 0;
  bool isSigned = true;
  TypePtr elem;
  std::string structName;
  int moduleId = -1;
  std::vector<TypePtr> params;
  TypePtr ret;

  // Used when kind == Var.
  int varId = 0;
  Constraint constraint = Constraint::None;
  bool allowDefault = false;
  bool allowUnresolved = false;  // e.g. the element type of a bare `None`
  std::string varDescription;
  Span varSpan;
  TypePtr link;
};

inline TypePtr resolveType(TypePtr type) {
  while (type && type->kind == TypeKind::Var && type->link) type = type->link;
  return type;
}

inline TypePtr makeType(TypeKind kind) {
  auto type = std::make_shared<Type>();
  type->kind = kind;
  return type;
}

inline TypePtr typeError() { return makeType(TypeKind::Error); }
inline TypePtr typeVoid() { return makeType(TypeKind::Void); }
inline TypePtr typeBool() { return makeType(TypeKind::Bool); }
inline TypePtr typeChar() { return makeType(TypeKind::Char); }
inline TypePtr typeString() { return makeType(TypeKind::String); }

inline TypePtr typeInt(int bits, bool isSigned) {
  auto type = makeType(TypeKind::Int);
  type->bits = bits;
  type->isSigned = isSigned;
  return type;
}

inline TypePtr typeFloat(int bits) {
  auto type = makeType(TypeKind::Float);
  type->bits = bits;
  return type;
}

inline TypePtr typeArray(TypePtr elem) {
  auto type = makeType(TypeKind::Array);
  type->elem = std::move(elem);
  return type;
}

inline TypePtr typeRange(TypePtr elem) {
  auto type = makeType(TypeKind::Range);
  type->elem = std::move(elem);
  return type;
}

inline TypePtr typeOptional(TypePtr elem) {
  auto type = makeType(TypeKind::Optional);
  type->elem = std::move(elem);
  return type;
}

inline TypePtr typeTuple(std::vector<TypePtr> elements) {
  auto type = makeType(TypeKind::Tuple);
  type->params = std::move(elements);
  return type;
}

inline TypePtr typeStruct(const std::string& name) {
  auto type = makeType(TypeKind::Struct);
  type->structName = name;
  return type;
}

inline TypePtr typeModule(int moduleId) {
  auto type = makeType(TypeKind::Module);
  type->moduleId = moduleId;
  return type;
}

inline TypePtr typeEnum(const std::string& name) {
  auto type = makeType(TypeKind::Enum);
  type->structName = name;
  return type;
}

inline TypePtr typeNativeModule(const std::string& name) {
  auto type = makeType(TypeKind::Module);
  type->structName = name;
  return type;
}

inline std::string shortName(const std::string& qualified) {
  std::size_t pos = qualified.rfind("::");
  return pos == std::string::npos ? qualified : qualified.substr(pos + 2);
}

inline TypePtr typeFunction(std::vector<TypePtr> params, TypePtr ret) {
  auto type = makeType(TypeKind::Function);
  type->params = std::move(params);
  type->ret = std::move(ret);
  return type;
}

inline bool isIntegerType(const TypePtr& type) {
  TypePtr t = resolveType(type);
  return t && t->kind == TypeKind::Int;
}

inline bool isFloatType(const TypePtr& type) {
  TypePtr t = resolveType(type);
  return t && t->kind == TypeKind::Float;
}

inline bool isNumericType(const TypePtr& type) {
  TypePtr t = resolveType(type);
  return t && (t->kind == TypeKind::Int || t->kind == TypeKind::Float);
}

inline TypePtr builtinTypeByName(const std::string& name) {
  if (name == "bool") return typeBool();
  if (name == "char") return typeChar();
  if (name == "String") return typeString();
  if (name == "void") return typeVoid();
  if (name == "i8") return typeInt(8, true);
  if (name == "i16") return typeInt(16, true);
  if (name == "i32") return typeInt(32, true);
  if (name == "i64") return typeInt(64, true);
  if (name == "u8") return typeInt(8, false);
  if (name == "u16") return typeInt(16, false);
  if (name == "u32") return typeInt(32, false);
  if (name == "u64") return typeInt(64, false);
  if (name == "f32") return typeFloat(32);
  if (name == "f64") return typeFloat(64);
  return nullptr;
}

inline std::string typeName(const TypePtr& type) {
  TypePtr t = resolveType(type);
  if (!t) return "unknown";
  switch (t->kind) {
    case TypeKind::Error: return "error";
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Char: return "char";
    case TypeKind::String: return "String";
    case TypeKind::Int:
      return (t->isSigned ? "i" : "u") + std::to_string(t->bits);
    case TypeKind::Float: return "f" + std::to_string(t->bits);
    case TypeKind::Array: return "[" + typeName(t->elem) + "]";
    case TypeKind::Range: return "range<" + typeName(t->elem) + ">";
    case TypeKind::Optional: return typeName(t->elem) + "?";
    case TypeKind::Tuple: {
      std::string s = "(";
      for (std::size_t i = 0; i < t->params.size(); ++i) {
        if (i) s += ", ";
        s += typeName(t->params[i]);
      }
      s += ")";
      return s;
    }
    case TypeKind::Struct: return shortName(t->structName);
    case TypeKind::Enum: return shortName(t->structName);
    case TypeKind::Module:
      return t->moduleId >= 0 ? "<module>" : "<native module " + t->structName + ">";
    case TypeKind::Function: {
      std::string s = "fn(";
      for (std::size_t i = 0; i < t->params.size(); ++i) {
        if (i) s += ", ";
        s += typeName(t->params[i]);
      }
      s += ") -> " + typeName(t->ret);
      return s;
    }
    case TypeKind::Var:
      switch (t->constraint) {
        case Constraint::Integer: return "an integer type";
        case Constraint::Floating: return "a float type";
        case Constraint::Numeric: return "a numeric type";
        default: return "an unknown type";
      }
  }
  return "unknown";
}

}  // namespace a
