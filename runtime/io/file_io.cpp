#include "io/file_io.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <ios>
#include <sstream>

namespace a {
namespace {

std::string osError(int code) {
  return std::strerror(code == 0 ? EIO : code);
}

}  // namespace

FileResult readFile(const std::string& path) {
  errno = 0;
  std::ifstream file(path, std::ios::binary);
  if (!file) return {false, {}, "Cannot read file \"" + path + "\": " + osError(errno)};
  std::ostringstream contents;
  contents << file.rdbuf();
  if (!file.good() && !file.eof()) {
    return {false, {}, "Failed while reading file \"" + path + "\"."};
  }
  return {true, contents.str(), {}};
}

FileResult writeFile(const std::string& path, const std::string& contents, bool append) {
  std::ios::openmode mode = std::ios::binary;
  mode |= append ? std::ios::app : std::ios::trunc;
  errno = 0;
  std::ofstream file(path, mode);
  if (!file) return {false, {}, "Cannot write file \"" + path + "\": " + osError(errno)};
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!file) return {false, {}, "Failed while writing file \"" + path + "\"."};
  return {true, {}, {}};
}

bool fileExists(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return static_cast<bool>(file);
}

}  // namespace a
