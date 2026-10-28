#pragma once

// A small, dependency-free JSON value used by the A language server. It only
// implements what LSP needs: parsing a complete document, looking up object
// members, building requests and responses, and serializing to UTF-8.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace jsonlite {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<Value>;
  using Object = std::vector<std::pair<std::string, Value>>;

  Value();
  Value(std::nullptr_t);
  Value(bool value);
  Value(int value);
  Value(long long value);
  Value(double value);
  Value(const char* value);
  Value(std::string value);
  Value(Array value);
  Value(Object value);

  static Value null() { return Value(); }
  static Value array() { return Value(Array{}); }
  static Value object() { return Value(Object{}); }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  bool asBool(bool fallback = false) const;
  double asNumber(double fallback = 0.0) const;
  long long asInt(long long fallback = 0) const;
  const std::string& asString() const;
  const Array& asArray() const;
  const Object& asObject() const;
  Array& asArray();
  Object& asObject();

  // Object access. operator[] inserts a null member when the key is missing.
  Value& operator[](const std::string& key);
  const Value* find(const std::string& key) const;
  bool contains(const std::string& key) const { return find(key) != nullptr; }

  void push(Value value);
  void set(std::string key, Value value);
  std::size_t size() const;

  std::string dump() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

// Parses one JSON document. On failure returns a null value and stores a
// human-readable message in `error` when provided.
Value parse(const std::string& text, std::string* error = nullptr);

}  // namespace jsonlite
