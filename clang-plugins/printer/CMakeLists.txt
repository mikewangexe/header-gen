set(SYMBOL_FILE PrintFunctionNames.exports)

set (CLANG_LIBS
  clang
  # clangFrontend
  # clangAST
  # clangAnalysis
  # clangBasic
  # clangCodeGen
  # clangDriver
  # clangFrontendTool
  # clangLex
  # clangParse
  # clangSema
  # clangSerialization
  # clangStaticAnalyzerCheckers
  # clangStaticAnalyzerCore
  # clangStaticAnalyzerFrontend
)

set (USER_LIBS
  pthread
)

add_clang_plugin(DumpDecls DumpDecls.cpp)

set_target_properties(DumpDecls PROPERTIES
  LINKER_LANGUAGE CXX
  PREFIX "")
target_link_libraries(DumpDecls sqlite3)