cmake_minimum_required(VERSION 3.20)

project(basic09c LANGUAGES C CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(HandleLLVMOptions)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(basic09c-bin
  src/Basic09AST.cpp
  src/Basic09IR.cpp
  src/Basic09Lexer.cpp
  src/Basic09Parser.cpp
  src/Basic09Semantic.cpp
  src/Basic09Symbols.cpp
  src/Basic09Token.cpp
  src/basic09c.cpp
  )
set_target_properties(basic09c-bin PROPERTIES OUTPUT_NAME basic09c)

target_compile_features(basic09c-bin PRIVATE cxx_std_17)
target_include_directories(basic09c-bin PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  ${LLVM_INCLUDE_DIRS}
  )
target_compile_definitions(basic09c-bin PRIVATE ${LLVM_DEFINITIONS})
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(basic09c-bin PRIVATE -fno-rtti)
endif()

llvm_map_components_to_libnames(LLVM_LIBS
  Core
  Support
  TargetParser
  )
target_link_libraries(basic09c-bin PRIVATE ${LLVM_LIBS})

find_program(LLVM_LIT
  NAMES llvm-lit lit
  HINTS "${LLVM_TOOLS_BINARY_DIR}"
  DOC "Path to llvm-lit or lit"
  )

if(LLVM_LIT)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test/lit.site.cfg.py.in"
    "${CMAKE_CURRENT_BINARY_DIR}/test/lit.site.cfg.py"
    @ONLY
    )

  add_custom_target(check-basic09c
    COMMAND "${LLVM_LIT}" -sv "${CMAKE_CURRENT_BINARY_DIR}/test"
    DEPENDS basic09c-bin
    USES_TERMINAL
    )
else()
  message(WARNING "llvm-lit not found; check-basic09c target will not be available")
endif()

install(TARGETS basic09c-bin RUNTIME DESTINATION bin)
