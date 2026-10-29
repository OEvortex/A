#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "collections/transparent_map.hpp"

namespace a {

// Native modules are thin namespaces over interpreter builtins. Each member
// name maps to the builtin function or constant that implements it.
struct NativeModuleSpec {
  StringMap<std::string> functions;
  StringMap<std::string> constants;
};

namespace native_modules_detail {

using Entries = std::initializer_list<std::pair<const char*, const char*>>;

inline NativeModuleSpec makeSpec(Entries functions, Entries constants) {
  NativeModuleSpec spec;
  for (const auto& entry : functions) spec.functions.emplace(entry.first, entry.second);
  for (const auto& entry : constants) spec.constants.emplace(entry.first, entry.second);
  return spec;
}

}  // namespace native_modules_detail

inline const StringMap<NativeModuleSpec>& nativeModules() {
  using native_modules_detail::makeSpec;
  static const StringMap<NativeModuleSpec> modules = {
      {"math", makeSpec({{"sqrt", "sqrt"},   {"abs", "abs"},       {"min", "min"},
                         {"max", "max"},     {"pow", "pow"},       {"sin", "sin"},
                         {"cos", "cos"},     {"tan", "tan"},       {"asin", "asin"},
                         {"acos", "acos"},   {"atan", "atan"},     {"atan2", "atan2"},
                         {"exp", "exp"},     {"log", "log"},       {"log2", "log2"},
                         {"log10", "log10"}, {"floor", "floor"},   {"ceil", "ceil"},
                         {"round", "round"}, {"trunc", "trunc"},   {"random", "random"},
                         {"random_int", "random_int"}},
                        {{"PI", "PI"}, {"E", "E"}})},
      {"io",
       makeSpec({{"print", "print"}, {"eprint", "eprint"}, {"input", "input"}, {"flush", "flush"}},
                {})},
      {"fs", makeSpec({{"read_file", "read_file"},
                       {"write_file", "write_file"},
                       {"append_file", "append_file"},
                       {"file_exists", "file_exists"},
                       {"list_dir", "list_dir"},
                       {"remove_file", "remove_file"},
                       {"create_dir", "create_dir"},
                       {"remove_dir", "remove_dir"},
                       {"rename", "rename"},
                       {"current_dir", "current_dir"}},
                      {})},
      {"strings", makeSpec({{"len", "len"}, {"str", "str"}, {"join", "join"}}, {})},
      {"collections", makeSpec({{"len", "len"},
                                {"map", "map"},
                                {"filter", "filter"},
                                {"reduce", "reduce"},
                                {"any", "any"},
                                {"all", "all"},
                                {"sum", "sum"},
                                {"join", "join"},
                                {"set", "set"}},
                               {})},
      {"unsafe", makeSpec({{"get", "unsafe_get"},
                           {"set", "unsafe_set"},
                           {"address_of", "unsafe_address_of"},
                           {"load", "unsafe_load"},
                           {"store", "unsafe_store"},
                           {"offset", "unsafe_offset"}},
                          {})},
      {"arena", makeSpec({{"new", "arena_new"},
                          {"pool", "arena_pool"},
                          {"region", "arena_region"},
                          {"alloc", "arena_alloc"},
                          {"release", "arena_release"},
                          {"reset", "arena_reset"},
                          {"used", "arena_used"},
                          {"total", "arena_total"}},
                         {})},
      {"core",
       makeSpec({{"panic", "panic"}, {"assert", "assert"}, {"assert_eq", "assert_eq"}}, {})},
      {"time", makeSpec({{"now", "now"}, {"sleep", "sleep"}}, {})},
  };
  return modules;
}

inline const NativeModuleSpec* nativeModuleSpec(const std::string& name) {
  auto found = nativeModules().find(name);
  return found == nativeModules().end() ? nullptr : &found->second;
}

inline bool isNativeModule(const std::string& name) { return nativeModuleSpec(name) != nullptr; }

inline const std::string* nativeFunction(const std::string& module, const std::string& member) {
  const NativeModuleSpec* spec = nativeModuleSpec(module);
  if (!spec) return nullptr;
  auto found = spec->functions.find(member);
  return found == spec->functions.end() ? nullptr : &found->second;
}

inline const std::string* nativeConstant(const std::string& module, const std::string& member) {
  const NativeModuleSpec* spec = nativeModuleSpec(module);
  if (!spec) return nullptr;
  auto found = spec->constants.find(member);
  return found == spec->constants.end() ? nullptr : &found->second;
}

}  // namespace a
