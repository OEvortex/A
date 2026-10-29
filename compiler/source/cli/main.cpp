#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define A_HAVE_ISATTY 1
#endif

#include "driver/driver.hpp"
#include "driver/version.hpp"

namespace {

constexpr int kExitUsage = 64;
constexpr int kExitNoInput = 66;

void printUsage(std::ostream& os) {
  os << "A language toolchain " << a::kVersion << "\n"
     << "\n"
     << "Usage:\n"
     << "  a <command> [options] <file.a>\n"
     << "\n"
      << "Commands:\n"
      << "  run <file.a>     compile and execute a program\n"
      << "  build <file.a>   compile program to native binary (-o <bin>)\n"
      << "  check <file.a>   type-check a program without running it\n"
      << "  fmt <file.a>     format a source file (prints to stdout)\n"
      << "  doc <file.a>     generate documentation (markdown, prints to stdout)\n"
      << "  version          print the toolchain version\n"
      << "  help             show this message\n"
     << "\n"
     << "Options:\n"
     << "  -o <file>        output path (for 'build' and 'doc')\n"
     << "  -w, --write      rewrite the source file in place (for 'fmt')\n"
     << "  --check          exit nonzero if the file needs formatting (for 'fmt')\n"
     << "  --indent <n>     indentation width in spaces (for 'fmt', default 4)\n"
     << "  --json           emit structured JSON (for 'doc')\n"
     << "  --backend <name> compilation backend: 'native' (default) or 'llvm'\n"
     << "  --emit-llvm      emit textual LLVM IR (.ll) file instead of executable\n"
     << "  -I <dir>         add a module search directory (repeatable)\n"
     << "  --stdlib <dir>   override the standard library directory\n"
     << "  --no-color       disable colored diagnostics\n"
     << "  -h, --help       show this message\n"
     << "  -V, --version    print the toolchain version\n";
}

bool isTerminal() {
#ifdef A_HAVE_ISATTY
  return isatty(fileno(stderr)) != 0;
#else
  return false;
#endif
}

struct Options {
  std::string command;
  std::string path;
  std::string outputPath;
  std::string backend = "llvm";
  bool hasExplicitBackend = false;
  bool emitLLVM = false;
  std::vector<std::string> modulePaths;
  std::string stdlibRoot;
  bool useColor = false;
  bool checkOnly = false;
  bool writeInPlace = false;
  bool jsonOutput = false;
  int indentWidth = 4;
};

bool parseOptions(int argc, char** argv, Options& options, std::ostream& err) {
  std::vector<std::string> positional;
  bool colorDisabled = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--no-color") {
      colorDisabled = true;
    } else if (arg == "--emit-llvm") {
      options.emitLLVM = true;
    } else if (arg == "--check") {
      options.checkOnly = true;
    } else if (arg == "-w" || arg == "--write") {
      options.writeInPlace = true;
    } else if (arg == "--json") {
      options.jsonOutput = true;
    } else if (arg == "--indent") {
      if (i + 1 >= argc) {
        err << "error: '--indent' requires a positive number\n";
        return false;
      }
      std::string value = argv[++i];
      try {
        options.indentWidth = std::stoi(value);
      } catch (...) {
        err << "error: invalid indentation width '" << value << "'\n";
        return false;
      }
      if (options.indentWidth < 1 || options.indentWidth > 16) {
        err << "error: indentation width must be between 1 and 16\n";
        return false;
      }
    } else if (arg.rfind("--indent=", 0) == 0) {
      std::string value = arg.substr(9);
      try {
        options.indentWidth = std::stoi(value);
      } catch (...) {
        err << "error: invalid indentation width '" << value << "'\n";
        return false;
      }
      if (options.indentWidth < 1 || options.indentWidth > 16) {
        err << "error: indentation width must be between 1 and 16\n";
        return false;
      }
    } else if (arg == "--backend") {
      if (i + 1 >= argc) {
        err << "error: '--backend' requires a name ('native' or 'llvm')\n";
        return false;
      }
      options.backend = argv[++i];
      options.hasExplicitBackend = true;
    } else if (arg.rfind("--backend=", 0) == 0) {
      options.backend = arg.substr(10);
      options.hasExplicitBackend = true;
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 >= argc) {
        err << "error: '" << arg << "' requires a file path\n";
        return false;
      }
      options.outputPath = argv[++i];
    } else if (arg == "-I" || arg == "--include") {
      if (i + 1 >= argc) {
        err << "error: '" << arg << "' requires a directory\n";
        return false;
      }
      options.modulePaths.push_back(argv[++i]);
    } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
      options.modulePaths.push_back(arg.substr(2));
    } else if (arg == "--stdlib") {
      if (i + 1 >= argc) {
        err << "error: '--stdlib' requires a directory\n";
        return false;
      }
      options.stdlibRoot = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      options.command = "help";
    } else if (arg == "-V" || arg == "--version") {
      options.command = "version";
    } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
      err << "error: unknown option '" << arg << "'\n";
      err << "help: run 'a help' to see available commands\n";
      return false;
    } else {
      positional.push_back(std::move(arg));
    }
  }

  options.useColor = !colorDisabled && isTerminal();

  if (options.command == "help" || options.command == "version") return true;

  if (positional.empty()) {
    err << "error: missing command\n";
    err << "help: run 'a help' to see available commands\n";
    return false;
  }

  static const std::vector<std::string> kCommands = {"run", "build", "check", "fmt", "doc", "version", "help"};
  bool known = false;
  for (const auto& command : kCommands) {
    if (positional[0] == command) {
      known = true;
      break;
    }
  }

  if (!known) {
    // Allow `a program.a` as shorthand for `a run program.a`.
    if (positional.size() == 1) {
      options.command = "run";
      options.path = positional[0];
      return true;
    }
    err << "error: unknown command '" << positional[0] << "'\n";
    err << "help: run 'a help' to see available commands\n";
    return false;
  }

  options.command = positional[0];
  if (options.command == "version" || options.command == "help") return true;

  if (positional.size() < 2) {
    err << "error: '" << options.command << "' requires a file\n";
    err << "help: usage is 'a " << options.command << " <file.a>'\n";
    return false;
  }

  if (positional.size() > 2) {
    err << "error: unexpected argument '" << positional[2] << "'\n";
    return false;
  }

  options.path = positional[1];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options, std::cerr)) {
    std::cerr << "\n";
    printUsage(std::cerr);
    return kExitUsage;
  }

  if (options.command == "help") {
    printUsage(std::cout);
    return 0;
  }
  if (options.command == "version") {
    std::cout << "A language toolchain " << a::kVersion << "\n";
    return 0;
  }

  if (options.path == "-") {
    std::cerr << "error: reading from stdin is not supported yet\n";
    return kExitNoInput;
  }

  a::DriverConfig config;
  config.checkOnly = options.command == "check";
  config.useColor = options.useColor;
  config.backend = options.backend;
  config.modulePaths = options.modulePaths;
  config.stdlibRoot = options.stdlibRoot;

  if (options.command == "build") {
    std::string backend = options.backend;
    // Default build backend is native C++ codegen for full language coverage unless --backend=llvm is explicitly specified
    if (backend == "llvm" && !options.hasExplicitBackend) {
      backend = "native";
    }
    std::string out = options.outputPath;
    if (out.empty()) {
      out = options.path;
      if (out.size() >= 2 && out.substr(out.size() - 2) == ".a") {
        out = out.substr(0, out.size() - 2);
      } else {
        out += ".out";
      }
      if (options.emitLLVM) {
        out += ".ll";
      }
    }
    if (options.emitLLVM) {
      return a::emitLLVMFile(options.path, out, config, std::cerr);
    }
    return a::buildFile(options.path, out, config, backend, std::cerr);
  }

  if (options.command == "fmt") {
    a::FormatFileOptions formatOptions;
    formatOptions.checkOnly = options.checkOnly;
    formatOptions.writeInPlace = options.writeInPlace;
    formatOptions.indentWidth = options.indentWidth;
    formatOptions.useColor = options.useColor;
    return a::formatFile(options.path, formatOptions, std::cout, std::cerr);
  }

  if (options.command == "doc") {
    a::DocFileOptions docOptions;
    docOptions.json = options.jsonOutput;
    docOptions.useColor = options.useColor;
    docOptions.outputPath = options.outputPath;
    return a::documentFile(options.path, docOptions, std::cout, std::cerr);
  }

  if (options.command == "check") {
    int code = a::runFile(options.path, config, std::cin, std::cout, std::cerr);
    if (code == 0) {
      std::cout << options.path << ": File is valid\n";
    }
    return code;
  }

  return a::runFile(options.path, config, std::cin, std::cout, std::cerr);
}
