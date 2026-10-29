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
      {"json", makeSpec({{"parse", "json_parse"},
                         {"stringify", "json_stringify"},
                         {"pretty", "json_pretty"},
                         {"is_valid", "json_is_valid"},
                         {"get", "json_get"},
                         {"at", "json_at"},
                         {"len", "json_len"},
                         {"has", "json_has"},
                         {"keys", "json_keys"},
                         {"kind", "json_type"},
                         {"as_str", "json_as_str"},
                         {"as_int", "json_as_int"},
                         {"as_float", "json_as_float"},
                         {"as_bool", "json_as_bool"},
                         {"as_array", "json_as_array"},
                         {"as_map", "json_as_map"}},
                        {})},
      {"binary", makeSpec({{"encode", "binary_encode"}, {"decode", "binary_decode"}}, {})},
      {"testing", makeSpec({{"seed", "testing_seed"},
                            {"for_all_int", "testing_for_all_int"},
                            {"for_all_float", "testing_for_all_float"},
                            {"for_all_bool", "testing_for_all_bool"},
                            {"for_all_string", "testing_for_all_string"},
                            {"for_all_elem", "testing_for_all_elem"}},
                           {})},
      {"log", makeSpec({{"set_level", "log_set_level"},
                        {"set_json", "log_set_json"},
                        {"debug", "log_debug"},
                        {"info", "log_info"},
                        {"warn", "log_warn"},
                        {"error", "log_error"}},
                       {})},
      {"metrics", makeSpec({{"counter", "metrics_counter"},
                            {"gauge", "metrics_gauge"},
                            {"histogram", "metrics_histogram"},
                            {"render", "metrics_render"},
                            {"reset", "metrics_reset"}},
                           {})},
      {"trace", makeSpec({{"start", "trace_start"},
                          {"end", "trace_end"},
                          {"current", "trace_current"},
                          {"set_attribute", "trace_set_attribute"},
                          {"add_event", "trace_add_event"},
                          {"inject", "trace_inject"},
                          {"extract", "trace_extract"},
                          {"spans", "trace_spans"},
                          {"reset", "trace_reset"}},
                         {})},
      {"net", makeSpec({{"resolve", "net_resolve"},
                        {"tcp_listen", "net_tcp_listen"},
                        {"tcp_connect", "net_tcp_connect"},
                        {"accept", "net_accept"},
                        {"send", "net_send"},
                        {"recv", "net_recv"},
                        {"close", "net_close"},
                        {"local_port", "net_local_port"},
                        {"set_timeout", "net_set_timeout"},
                        {"set_nonblocking", "net_set_nonblocking"},
                        {"poll", "net_poll"},
                        {"udp_socket", "net_udp_socket"},
                        {"udp_bind", "net_udp_bind"},
                        {"udp_send", "net_udp_send"},
                        {"udp_recv", "net_udp_recv"},
                        {"tls_connect", "net_tls_connect"},
                        {"tls_available", "net_tls_available"}},
                       {})},
      {"http", makeSpec({{"get", "http_get"},
                         {"post", "http_post"},
                         {"request", "http_request"},
                         {"serve", "http_serve"},
                         {"serve_once", "http_serve_once"}},
                        {})},
  };
  return modules;
}

// Builtins implemented by the standard-library layer in the interpreter. The
// LLVM and native backends cannot lower these and fall back to the interpreter.
inline bool isStdlibBuiltin(const std::string& name) {
  static const char* prefixes[] = {"json_",   "binary_", "testing_", "log_",
                                   "metrics_", "trace_",  "net_",    "http_"};
  for (const char* prefix : prefixes) {
    const std::string_view p(prefix);
    if (name.size() >= p.size() && name.compare(0, p.size(), p) == 0) return true;
  }
  return false;
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
