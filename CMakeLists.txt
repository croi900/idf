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

find_package(Protobuf REQUIRED)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
    tokens.proto
    tokens_processed.proto
)


find_library(TREE_SITTER_LIB tree-sitter REQUIRED)
find_library(TREE_SITTER_C_LIB tree-sitter-c REQUIRED)
set(TREE_SITTER_CPP_LIB /usr/lib/libtree-sitter-cpp.so)
find_path(TREE_SITTER_INCLUDE tree_sitter/api.h REQUIRED)


add_executable(${PROJECT_NAME} 
    main.cpp 
    src/dirtree.hpp 
    src/extensions.hpp
    src/tokenizer.hpp
    src/sharding.hpp
    src/db_manager.hpp
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    src/file_processor.hpp
    src/search.h
        src/index.h
        src/actions.h
        src/cli.h
        src/gui.h
        src/parser.h
        src/parser_queries.h
)

target_compile_options(${PROJECT_NAME} PRIVATE 
    -O3 -march=native
)

find_package(HPX REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(DuckDB CONFIG REQUIRED)
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
    protobuf::libprotobuf
    absl::log_internal_check_op
    absl::log_internal_message
    absl::flat_hash_map
    absl::hash
    duckdb
    ${TREE_SITTER_LIB}
    ${TREE_SITTER_C_LIB}
    ${TREE_SITTER_CPP_LIB}

)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    <hpx/hpx.hpp>
    <vector>
    <string>
    <filesystem>
    <iostream>
    <liburing.h>
    <google/protobuf/io/coded_stream.h>
    <google/protobuf/io/zero_copy_stream_impl.h>
    <google/protobuf/arena.h>
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${URING_INCLUDE_DIRS}
    ${JEMALLOC_INCLUDE_DIRS}
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/gui
        ${CMAKE_CURRENT_BINARY_DIR}/gui
    COMMENT "Copying GUI Frontend into Build Directory"
)
