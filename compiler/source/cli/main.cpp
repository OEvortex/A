#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define A_HAVE_ISATTY 1
#endif

#include "a/driver/driver.hpp"
#include "a/version.hpp"

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
     << "  check <file.a>   type-check a program without running it\n"
     << "  version          print the toolchain version\n"
     << "  help             show this message\n"
     << "\n"
     << "Options:\n"
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
  bool useColor = false;
};

bool parseOptions(int argc, char** argv, Options& options, std::ostream& err) {
  std::vector<std::string> positional;
  bool colorDisabled = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--no-color") {
      colorDisabled = true;
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

  static const std::vector<std::string> kCommands = {"run", "check", "version", "help"};
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

  return a::runFile(options.path, config, std::cin, std::cout, std::cerr);
}
