set(SYMBOL_FILE DeclFilter.exports)

set (CLANG_LIBS
  clang
)

add_clang_plugin(DeclFilter DeclFilter.cpp)

set_target_properties(DeclFilter PROPERTIES
  LINKER_LANGUAGE CXX
  PREFIX "")
target_link_libraries(DeclFilter sqlite3)