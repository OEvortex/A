#include "name-resolution/module_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "lexer/lexer.hpp"
#include "name-resolution/native_modules.hpp"
#include "parser/parser.hpp"

namespace a {
namespace fs = std::filesystem;
namespace {

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::vector<std::string> environmentPaths() {
  std::vector<std::string> paths;
  const char* value = std::getenv("A_PATH");
  if (!value) return paths;
  std::string list = value;
  std::size_t start = 0;
  while (start <= list.size()) {
    std::size_t end = list.find(':', start);
    if (end == std::string::npos) end = list.size();
    if (end > start) paths.push_back(list.substr(start, end - start));
    start = end + 1;
  }
  return paths;
}

std::string stemOf(const std::string& path) {
  std::string stem = fs::path(path).stem().string();
  return stem.empty() ? "module" : stem;
}

}  // namespace

std::string defaultStdlibRoot() {
  if (const char* env = std::getenv("A_STDLIB"); env && *env) return env;

  std::vector<fs::path> candidates;
  std::error_code error;
#if defined(__linux__)
  fs::path executable = fs::read_symlink("/proc/self/exe", error);
#else
  fs::path executable;
#endif
  if (!error && !executable.empty()) {
    fs::path dir = executable.parent_path();
    candidates.push_back(dir / "stdlib");
    candidates.push_back(dir / ".." / "stdlib");
    candidates.push_back(dir / ".." / ".." / "stdlib");
    candidates.push_back(dir / ".." / "share" / "a" / "stdlib");
  }
  candidates.push_back(fs::current_path(error) / "stdlib");

  for (const auto& candidate : candidates) {
    std::error_code existsError;
    if (fs::exists(candidate, existsError) && !existsError) return candidate.string();
  }
  return {};
}

ModuleLoader::ModuleLoader(ModuleLoaderOptions options, DiagnosticSink& sink)
    : options_(std::move(options)), sink_(sink) {}

std::string ModuleLoader::canonicalKey(const std::string& path) const {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

std::string ModuleLoader::uniqueModuleName(const std::string& stem) const {
  std::string name = stem.empty() ? "module" : stem;
  bool clash = false;
  for (const auto& module : graph_.modules) {
    if (module.name == name) {
      clash = true;
      break;
    }
  }
  if (!clash) return name;
  for (int suffix = 2;; ++suffix) {
    std::string candidate = name + "_" + std::to_string(suffix);
    bool taken = false;
    for (const auto& module : graph_.modules) {
      if (module.name == candidate) {
        taken = true;
        break;
      }
    }
    if (!taken) return candidate;
  }
}

ModuleGraph ModuleLoader::load(const std::string& entryPath, std::string entrySource) {
  if (options_.stdlibRoot.empty()) options_.stdlibRoot = defaultStdlibRoot();

  int entry = loadModule(entryPath, entryPath, &entrySource);
  graph_.entry = entry;
  return std::move(graph_);
}

int ModuleLoader::loadModule(const std::string& resolvedPath, const std::string& displayPath,
                             const std::string* preloaded) {
  const std::string key = canonicalKey(resolvedPath);
  auto existing = byPath_.find(key);
  if (existing != byPath_.end()) {
    if (existing->second.state == State::Loading) return existing->second.id;
    return existing->second.id;
  }

  std::string source = preloaded ? *preloaded : readFile(resolvedPath);
  if (!preloaded && source.empty()) {
    std::error_code error;
    if (!fs::exists(resolvedPath, error)) return -1;
  }

  Module module;
  module.id = static_cast<int>(graph_.modules.size());
  module.name = uniqueModuleName(stemOf(displayPath));
  module.path = displayPath;

  auto sourceFile = std::make_shared<SourceFile>(displayPath, std::move(source));
  FileId fileId = sink_.addFile(sourceFile);

  const int errorsBefore = sink_.errorCount();
  Lexer lexer(sourceFile->text(), sink_, fileId);
  Parser parser(lexer.tokenize(), sink_);
  module.program = parser.parseProgram();

  graph_.modules.push_back(std::move(module));
  byPath_[key] = Entry{graph_.modules.back().id, State::Loading};

  Module& stored = graph_.modules.back();
  if (sink_.errorCount() == errorsBefore) loadImports(stored);

  byPath_[key].state = State::Loaded;
  graph_.order.push_back(stored.id);
  return stored.id;
}

void ModuleLoader::loadImports(Module& module) {
  const fs::path fromDir = fs::path(module.path).parent_path();

  auto resolve = [&](const std::string& path, const std::string& bareName, bool quoted,
                     Span span, bool& nativeOut) -> int {
    nativeOut = false;
    if (quoted) return resolveQuoted(path, fromDir.string(), span);
    bool found = false;
    int id = resolveBare(bareName, span, nativeOut, found);
    if (!found) {
      sink_.error(span, "Cannot find module '" + bareName + "'.",
                  "Native modules are math, io, fs, strings, collections, core, and time. "
                  "A-file modules are searched in the stdlib directory.");
    }
    return id;
  };

  for (auto& statement : module.program->statements) {
    if (auto* import = dynamic_cast<ImportStmt*>(statement.get())) {
      bool native = false;
      int id = resolve(import->path, import->bareName, import->quoted, import->targetSpan, native);
      import->nativeModule = native;
      import->moduleId = id;
      continue;
    }
    if (auto* from = dynamic_cast<FromImportStmt*>(statement.get())) {
      bool native = false;
      int id = resolve(from->path, from->bareName, from->quoted, from->targetSpan, native);
      from->nativeModule = native;
      from->moduleId = id;
    }
  }
}

int ModuleLoader::resolveQuoted(const std::string& rawPath, const std::string& fromDir,
                                Span span) {
  std::vector<fs::path> candidates;
  fs::path requested(rawPath);
  if (requested.is_absolute()) {
    candidates.push_back(requested);
  } else {
    candidates.push_back(fs::path(fromDir) / requested);
    for (const auto& dir : options_.modulePaths) candidates.push_back(fs::path(dir) / requested);
    for (const auto& dir : environmentPaths()) candidates.push_back(fs::path(dir) / requested);
    if (!options_.stdlibRoot.empty()) {
      candidates.push_back(fs::path(options_.stdlibRoot) / requested);
    }
  }

  for (const auto& candidate : candidates) {
    std::error_code error;
    if (!fs::is_regular_file(candidate, error) || error) continue;
    const std::string path = candidate.string();
    const std::string key = canonicalKey(path);
    auto existing = byPath_.find(key);
    if (existing != byPath_.end() && existing->second.state == State::Loading) {
      sink_.error(span, "Import cycle detected while loading '" + path + "'.",
                  "Modules cannot import each other in a cycle.");
      return -1;
    }
    return loadModule(path, path, nullptr);
  }

  sink_.error(span, "Cannot find module \"" + rawPath + "\".",
              "Imports are resolved relative to the importing file, then -I paths, A_PATH, "
              "and the stdlib directory.");
  return -1;
}

int ModuleLoader::resolveBare(const std::string& name, Span span, bool& nativeOut, bool& found) {
  found = false;
  nativeOut = false;
  if (isNativeModule(name)) {
    nativeOut = true;
    found = true;
    return -1;
  }

  std::vector<fs::path> candidates;
  if (!options_.stdlibRoot.empty()) {
    candidates.push_back(fs::path(options_.stdlibRoot) / (name + ".a"));
    candidates.push_back(fs::path(options_.stdlibRoot) / name / (name + ".a"));
  }
  for (const auto& dir : options_.modulePaths) {
    candidates.push_back(fs::path(dir) / (name + ".a"));
    candidates.push_back(fs::path(dir) / name / (name + ".a"));
  }

  for (const auto& candidate : candidates) {
    std::error_code error;
    if (!fs::is_regular_file(candidate, error) || error) continue;
    const std::string path = candidate.string();
    const std::string key = canonicalKey(path);
    auto existing = byPath_.find(key);
    if (existing != byPath_.end() && existing->second.state == State::Loading) {
      sink_.error(span, "Import cycle detected while loading '" + path + "'.",
                  "Modules cannot import each other in a cycle.");
      return -1;
    }
    found = true;
    return loadModule(path, path, nullptr);
  }
  return -1;
}

}  // namespace a
