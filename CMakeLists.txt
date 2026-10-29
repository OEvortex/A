cmake_minimum_required(VERSION 3.20)
project(Alang VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(A_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(A_ENABLE_LTO "Enable link-time optimization in Release builds" ON)

if(A_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT a_ipo_supported OUTPUT a_ipo_error)
  if(a_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(STATUS "LTO unavailable: ${a_ipo_error}")
  endif()
endif()

# ---------------------------------------------------------------- interface

add_library(a_project_options INTERFACE)
target_include_directories(a_project_options INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compiler/source>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime>
  $<INSTALL_INTERFACE:include/a/compiler>
  $<INSTALL_INTERFACE:include/a/runtime>)

if(MSVC)
  target_compile_options(a_project_options INTERFACE /W4)
  if(A_WARNINGS_AS_ERRORS)
    target_compile_options(a_project_options INTERFACE /WX)
  endif()
else()
  target_compile_options(a_project_options INTERFACE -Wall -Wextra -Wpedantic)
  if(A_WARNINGS_AS_ERRORS)
    target_compile_options(a_project_options INTERFACE -Werror)
  endif()
endif()

# ------------------------------------------------------------------ modules

# SourceFile lives with diagnostics because DiagnosticSink stores source
# files to render context. Keeping it here avoids a dependency cycle between
# a_diagnostics and a_lexer (the lexer consumes diagnostics for errors).
add_library(a_diagnostics STATIC
  compiler/source/diagnostics/diagnostics.cpp
  compiler/source/lexer/source.cpp
)
target_link_libraries(a_diagnostics PUBLIC a_project_options)

add_library(a_lexer STATIC
  compiler/source/lexer/lexer.cpp
)
target_link_libraries(a_lexer PUBLIC a_diagnostics)

add_library(a_parser STATIC
  compiler/source/parser/parser.cpp
)
target_link_libraries(a_parser PUBLIC a_lexer)

add_library(a_name_resolution STATIC
  compiler/source/name-resolution/module_loader.cpp
)
target_link_libraries(a_name_resolution PUBLIC a_parser)

add_library(a_type_checker STATIC
  compiler/source/type-checker/sema.cpp
)
target_link_libraries(a_type_checker PUBLIC a_name_resolution)

add_library(a_ownership STATIC
  compiler/source/ownership/ownership.cpp
)
target_link_libraries(a_ownership PUBLIC a_type_checker)

add_library(a_ir STATIC
  compiler/source/ir/ir.cpp
)
target_link_libraries(a_ir PUBLIC a_type_checker a_runtime)

add_library(a_optimization STATIC
  compiler/source/optimization/optimizer.cpp
)
target_link_libraries(a_optimization PUBLIC a_ir)

add_library(a_runtime STATIC
  runtime/strings/utf8.cpp
  runtime/strings/format.cpp
  runtime/io/file_io.cpp
)
target_link_libraries(a_runtime PUBLIC a_project_options)

add_library(a_backend STATIC
  compiler/source/backend/interpreter.cpp
  compiler/source/backend/stdlib/builtins.cpp
  compiler/source/backend/native/native_codegen.cpp
  compiler/source/backend/llvm/llvm_backend.cpp
)
target_link_libraries(a_backend PUBLIC a_type_checker a_runtime)

# TLS for net.tls_connect / https is optional: enable it when OpenSSL is present.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(a_backend PRIVATE A_HAVE_OPENSSL=1)
  target_link_libraries(a_backend PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  message(STATUS "OpenSSL not found: net TLS support will report as unavailable")
endif()

add_library(a_tooling STATIC
  compiler/source/tooling/source_text.cpp
  compiler/source/tooling/ast_text.cpp
  compiler/source/tooling/formatter.cpp
  compiler/source/tooling/docgen.cpp
)
target_link_libraries(a_tooling PUBLIC a_parser)

add_library(a_driver STATIC
  compiler/source/driver/driver.cpp
)
target_link_libraries(a_driver PUBLIC a_backend a_ownership a_ir a_optimization a_tooling)

# Namespaced aliases keep embedding projects decoupled from target names.
add_library(a::diagnostics ALIAS a_diagnostics)
add_library(a::lexer ALIAS a_lexer)
add_library(a::parser ALIAS a_parser)
add_library(a::name_resolution ALIAS a_name_resolution)
add_library(a::type_checker ALIAS a_type_checker)
add_library(a::ownership ALIAS a_ownership)
add_library(a::ir ALIAS a_ir)
add_library(a::optimization ALIAS a_optimization)
add_library(a::runtime ALIAS a_runtime)
add_library(a::backend ALIAS a_backend)
add_library(a::tooling ALIAS a_tooling)
add_library(a::driver ALIAS a_driver)

# ---------------------------------------------------------------------- cli

add_executable(a compiler/source/cli/main.cpp)
target_link_libraries(a PRIVATE a_driver)

# ------------------------------------------------------------ language server

add_executable(a-lsp
  tools/language-server/server.cpp
  tools/language-server/analysis.cpp
  tools/language-server/json.cpp
)
target_link_libraries(a-lsp PRIVATE a_type_checker a_tooling)

# ---------------------------------------------------------------- packaging

include(GNUInstallDirs)
install(TARGETS a RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS a-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY compiler/source/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/compiler
  FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY runtime/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/runtime
  FILES_MATCHING PATTERN "*.hpp")

# --------------------------------------------------------------------- tests

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh")
  add_test(NAME language-suite
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh $<TARGET_FILE:a>)
endif()

add_executable(a-tooling-tests tests/tooling/tooling_tests.cpp)
target_link_libraries(a-tooling-tests PRIVATE a_tooling)
add_test(NAME tooling-tests COMMAND a-tooling-tests)

add_executable(a-ir-tests tests/ir/ir_tests.cpp)
target_link_libraries(a-ir-tests PRIVATE a_ir a_optimization)
add_test(NAME ir-tests COMMAND a-ir-tests)
add_test(NAME tooling-cli
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/run_cli_tests.sh $<TARGET_FILE:a>)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME language-server-smoke
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/language-server/smoke_test.py
      $<TARGET_FILE:a-lsp>)
endif()
