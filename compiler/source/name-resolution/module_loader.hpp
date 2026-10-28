#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "collections/transparent_map.hpp"
#include "diagnostics/diagnostics.hpp"
#include "ast/ast.hpp"

namespace a {

// One parsed source file in a program. Modules are stored in a stable deque;
// `id` is the index. A deque keeps references valid while imports recursively
// append more modules during loading.
struct Module {
  int id = -1;
  std::string name;  // namespace stem, unique within the program
  std::string path;  // path used in diagnostics
  std::unique_ptr<Program> program;
};

struct ModuleGraph {
  std::deque<Module> modules;
  std::vector<int> order;  // dependency order: imports before importers
  int entry = -1;

  const Module& at(int id) const { return modules[static_cast<std::size_t>(id)]; }
  Module& at(int id) { return modules[static_cast<std::size_t>(id)]; }
};

struct ModuleLoaderOptions {
  std::vector<std::string> modulePaths;  // -I directories
  std::string stdlibRoot;                // stdlib/ directory; auto-detected when empty
};

// Parses an entry file and every module it imports, resolving imports relative
// to the importing file, then -I paths, A_PATH, and the stdlib root. Reports
// missing modules and import cycles through the sink.
class ModuleLoader {
 public:
  ModuleLoader(ModuleLoaderOptions options, DiagnosticSink& sink);

  ModuleGraph load(const std::string& entryPath, std::string entrySource);

 private:
  enum class State { Loading, Loaded };

  struct Entry {
    int id = -1;
    State state = State::Loading;
  };

  int loadModule(const std::string& resolvedPath, const std::string& displayPath,
                 const std::string* preloaded);
  void loadImports(Module& module);
  int resolveQuoted(const std::string& rawPath, const std::string& fromDir, Span span);
  int resolveBare(const std::string& name, Span span, bool& nativeOut, bool& found);
  std::string canonicalKey(const std::string& path) const;
  std::string uniqueModuleName(const std::string& stem) const;

  ModuleLoaderOptions options_;
  DiagnosticSink& sink_;
  ModuleGraph graph_;
  StringMap<Entry> byPath_;
};
// Returns the default stdlib directory: $A_STDLIB, then paths derived from the
// executable location, then ./stdlib.
std::string defaultStdlibRoot();

}  // namespace a
