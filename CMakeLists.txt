cmake_minimum_required(VERSION 4.2.2)

project(idf VERSION 1.0 LANGUAGES CXX)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-march=native")
check_cxx_source_compiles("
#ifndef __AVX512BW__
#  error No AVX512BW
#endif
int main() { return 0; }
" HAVE_AVX512BW)
unset(CMAKE_REQUIRED_FLAGS)
if(HAVE_AVX512BW)
    message(STATUS "[idf] AVX-512BW detected — dual AVX2/AVX-512 tokenizer enabled")
else()
    message(STATUS "[idf] AVX-512BW not available — AVX2-only tokenizer")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED lmdb)

include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG main
)
FetchContent_MakeAvailable(ftxui)

find_library(TREE_SITTER_LIB tree-sitter REQUIRED)
find_library(TREE_SITTER_BASH_LIB tree-sitter-bash REQUIRED)
find_library(TREE_SITTER_C_LIB tree-sitter-c REQUIRED)
find_library(TREE_SITTER_CPP_LIB tree-sitter-cpp REQUIRED)
find_library(TREE_SITTER_JAVASCRIPT_LIB tree-sitter-javascript REQUIRED)
find_library(TREE_SITTER_LUA_LIB tree-sitter-lua REQUIRED)
find_library(TREE_SITTER_MARKDOWN_LIB tree-sitter-markdown REQUIRED)
find_library(TREE_SITTER_PYTHON_LIB tree-sitter-python REQUIRED)
find_library(TREE_SITTER_QUERY_LIB tree-sitter-query REQUIRED)
find_library(TREE_SITTER_RUST_LIB tree-sitter-rust REQUIRED)
find_library(TREE_SITTER_VIM_LIB tree-sitter-vim REQUIRED)
find_library(TREE_SITTER_VIMDOC_LIB tree-sitter-vimdoc REQUIRED)
find_path(TREE_SITTER_INCLUDE tree_sitter/api.h REQUIRED)


add_executable(${PROJECT_NAME} 
    main.cpp 
    src/dirtree.hpp 
    src/extensions.hpp
    src/tokenizer.hpp
    src/db_manager.hpp
    src/file_processor.hpp
    src/search.h
    src/index.h
    src/actions.h
    src/cli.h
    src/parser.h
    src/parser_queries.h
    src/query_parser.h
    src/history.hpp
)

target_compile_options(${PROJECT_NAME} PRIVATE 
    -O3 -march=native
)

find_package(HPX REQUIRED)
find_package(absl REQUIRED)

pkg_check_modules(URING REQUIRED liburing)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)


target_link_libraries(${PROJECT_NAME}
    PRIVATE
    HPX::hpx
    HPX::wrap_main
    HPX::iostreams_component
    ${URING_LIBRARIES}
    ${JEMALLOC_LIBRARIES}
    ${LMDB_LIBRARIES}
    absl::log_internal_check_op
    absl::log_internal_message
    absl::flat_hash_map
    absl::hash
    ${TREE_SITTER_LIB}
    ${TREE_SITTER_BASH_LIB}
    ${TREE_SITTER_C_LIB}
    ${TREE_SITTER_CPP_LIB}
    ${TREE_SITTER_JAVASCRIPT_LIB}
    ${TREE_SITTER_LUA_LIB}
    ${TREE_SITTER_MARKDOWN_LIB}
    ${TREE_SITTER_PYTHON_LIB}
    ${TREE_SITTER_QUERY_LIB}
    ${TREE_SITTER_RUST_LIB}
    ${TREE_SITTER_VIM_LIB}
    ${TREE_SITTER_VIMDOC_LIB}
    ftxui::screen
    ftxui::dom
    ftxui::component
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    <hpx/hpx.hpp>
    <vector>
    <string>
    <filesystem>
    <iostream>
    <liburing.h>
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${URING_INCLUDE_DIRS}
    ${JEMALLOC_INCLUDE_DIRS}
    ${LMDB_INCLUDE_DIRS}
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/gui
        ${CMAKE_CURRENT_BINARY_DIR}/gui
    COMMENT "Copying GUI Frontend into Build Directory"
)
