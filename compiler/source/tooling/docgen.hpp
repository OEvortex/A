#pragma once

#include <string>
#include <vector>

#include "diagnostics/diagnostics.hpp"

namespace a::tooling {

struct DocParameter {
  std::string name;
  std::string type;
  std::string defaultValue;
};

struct DocMember {
  std::string name;
  std::string signature;
  std::string doc;
  std::string type;
};

struct DocItem {
  enum class Kind { Function, Class, Struct, Metaclass, Enum, TypeAlias, Constant, Variable };

  Kind kind = Kind::Function;
  std::string name;
  std::string signature;
  std::string doc;
  std::string returnType;
  bool isPrivate = false;
  std::vector<DocParameter> params;
  std::vector<DocMember> fields;
  std::vector<DocMember> variants;
  std::vector<DocItem> methods;
};

struct DocModule {
  std::string name;
  std::string path;
  std::string doc;
  std::vector<DocItem> items;
};

struct DocOptions {
  std::string moduleName;  // defaults to the file stem
  std::string sourceName;  // name shown in the generated header
};

// Parses A source and extracts documentation from `##` comments attached to
// declarations. Diagnostics go through `sink`; `ok` is false on parse errors.
struct DocResult {
  bool ok = false;
  DocModule module;
};

DocResult extractDocs(const std::string& source, DiagnosticSink& sink,
                      const DocOptions& options = {});

std::string renderMarkdown(const DocModule& module);
std::string renderJson(const DocModule& module);

}  // namespace a::tooling
