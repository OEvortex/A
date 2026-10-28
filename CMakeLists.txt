cmake_minimum_required(VERSION 3.20)
project(Alang VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(A_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------- interface

add_library(a_project_options INTERFACE)
target_include_directories(a_project_options INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

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

add_library(a_common STATIC
  src/common/source.cpp
  src/common/diagnostics.cpp
)
target_link_libraries(a_common PUBLIC a_project_options)

add_library(a_frontend STATIC
  src/frontend/lexer.cpp
  src/frontend/parser.cpp
)
target_link_libraries(a_frontend PUBLIC a_common)

add_library(a_sema STATIC
  src/sema/sema.cpp
)
target_link_libraries(a_sema PUBLIC a_frontend)

add_library(a_runtime STATIC
  src/runtime/interpreter.cpp
)
target_link_libraries(a_runtime PUBLIC a_sema)

add_library(a_driver STATIC
  src/driver/driver.cpp
)
target_link_libraries(a_driver PUBLIC a_runtime)

# Namespaced aliases keep embedding projects decoupled from target names.
add_library(a::common ALIAS a_common)
add_library(a::frontend ALIAS a_frontend)
add_library(a::sema ALIAS a_sema)
add_library(a::runtime ALIAS a_runtime)
add_library(a::driver ALIAS a_driver)

# ---------------------------------------------------------------------- cli

add_executable(a src/cli/main.cpp)
target_link_libraries(a PRIVATE a_driver)

# ---------------------------------------------------------------- packaging

include(GNUInstallDirs)
install(TARGETS a RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/a DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# --------------------------------------------------------------------- tests

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh")
  add_test(NAME language-suite
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:a>)
endif()
