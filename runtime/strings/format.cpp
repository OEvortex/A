#include "strings/format.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <variant>

#include "strings/utf8.hpp"

namespace a {
namespace {

std::string quoteString(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

std::string formatDouble(double value) {
  if (std::isnan(value)) return "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  std::string text(buffer, result.ptr);
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

}  // namespace

std::string formatValue(const Value& value, bool quoteStrings) {
  if (std::holds_alternative<std::monostate>(value.data)) return "void";
  if (auto* b = std::get_if<bool>(&value.data)) return *b ? "true" : "false";
  if (auto* i = std::get_if<std::int64_t>(&value.data)) {
    char buffer[32];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), *i);
    return std::string(buffer, result.ptr);
  }
  if (auto* d = std::get_if<double>(&value.data)) return formatDouble(*d);
  if (auto* c = std::get_if<char32_t>(&value.data)) return utf8Encode(*c);
  if (auto* s = std::get_if<std::string>(&value.data)) {
    return quoteStrings ? quoteString(*s) : *s;
  }
  if (auto* array = std::get_if<ArrayPtr>(&value.data)) {
    std::string out = "[";
    for (std::size_t i = 0; i < (*array)->size(); ++i) {
      if (i) out += ", ";
      out += formatValue((**array)[i], true);
    }
    out += "]";
    return out;
  }
  if (auto* view = std::get_if<ArrayViewPtr>(&value.data)) {
    const auto& storage = *(*view)->storage;
    std::string out = "[";
    for (std::size_t i = 0; i < (*view)->length; ++i) {
      if (i) out += ", ";
      out += formatValue(storage[(*view)->offset + i], true);
    }
    out += "]";
    return out;
  }
  if (auto* map = std::get_if<MapPtr>(&value.data)) {
    std::string out = "{";
    for (std::size_t i = 0; i < (*map)->entries.size(); ++i) {
      if (i) out += ", ";
      out += formatValue((*map)->entries[i].first, true);
      out += ": ";
      out += formatValue((*map)->entries[i].second, true);
    }
    out += "}";
    return out;
  }
  if (auto* set = std::get_if<SetPtr>(&value.data)) {
    if ((*set)->elements.empty()) return "set()";
    std::string out = "{";
    for (std::size_t i = 0; i < (*set)->elements.size(); ++i) {
      if (i) out += ", ";
      out += formatValue((*set)->elements[i], true);
    }
    out += "}";
    return out;
  }
  if (auto* object = std::get_if<StructPtr>(&value.data)) {
    const std::string& name = (*object)->typeName;
    std::size_t sep = name.rfind("::");
    std::string shortName = sep == std::string::npos ? name : name.substr(sep + 2);
    std::string out = shortName + "(";
    for (std::size_t i = 0; i < (*object)->fields.size(); ++i) {
      if (i) out += ", ";
      out += (*object)->fields[i].first + "=" + formatValue((*object)->fields[i].second, true);
    }
    out += ")";
    return out;
  }
  if (auto* range = std::get_if<RangeValue>(&value.data)) {
    std::string out = std::to_string(range->start) + ".." + std::to_string(range->end);
    if (range->step != 1) out += ".." + std::to_string(range->step);
    return out;
  }
  if (auto* optional = std::get_if<OptionalPtr>(&value.data)) {
    if (!(*optional)->hasValue) return "None";
    return "Some(" + formatValue((*optional)->value, true) + ")";
  }
  if (auto* result = std::get_if<ResultPtr>(&value.data)) {
    if ((*result)->ok) return "Ok(" + formatValue((*result)->value, true) + ")";
    return "Err(" + formatValue((*result)->value, true) + ")";
  }
  if (auto* tuple = std::get_if<TuplePtr>(&value.data)) {
    std::string out = "(";
    for (std::size_t i = 0; i < (*tuple)->items.size(); ++i) {
      if (i) out += ", ";
      out += formatValue((*tuple)->items[i], true);
    }
    out += ")";
    return out;
  }
  if (auto* allocator = std::get_if<AllocatorPtr>(&value.data)) {
    const char* kind = "arena";
    if ((*allocator)->kind == AllocatorObj::Kind::Pool) kind = "pool";
    if ((*allocator)->kind == AllocatorObj::Kind::Region) kind = "region";
    return "<" + std::string(kind) + " " + std::to_string((*allocator)->used) + "/" +
           std::to_string((*allocator)->capacity) + ">";
  }
  if (auto* pointer = std::get_if<PointerPtr>(&value.data)) {
    return "<pointer " + std::to_string((*pointer)->index) + ">";
  }
  if (std::holds_alternative<ClosurePtr>(value.data)) return "<function>";
  if (auto* module = std::get_if<ModulePtr>(&value.data)) {
    if (!(*module)->nativeName.empty()) return "<module " + (*module)->nativeName + ">";
    return "<module>";
  }
  if (std::holds_alternative<NativeFunctionPtr>(value.data)) return "<native function>";
  if (auto* en = std::get_if<EnumPtr>(&value.data)) {
    const std::string& name = (*en)->name;
    std::size_t sep = name.rfind("::");
    return "<enum " + (sep == std::string::npos ? name : name.substr(sep + 2)) + ">";
  }
  return "void";
}

}  // namespace a
