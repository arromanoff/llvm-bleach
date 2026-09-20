find_program(CLANG_COMPILER clang HINTS ${BLEACH_CLANG_COMPILER_PATH})
if(NOT CLANG_COMPILER)
  message(
    FATAL_ERROR
      "No clang compiler found for building symtab.cpp. Try specifying BLEACH_CLANG_COMPILER_PATH"
  )
endif()

function(make_ir_file SOURCE_FILE)
  set(single_value_args OUTPUT)
  cmake_parse_arguments(ARG "" "${single_value_args}" "" ${ARGN})
  get_filename_component(OUTPUT_FILENAME ${ARG_OUTPUT} NAME_WE)
  add_custom_command(COMMAND ${CLANG_COMPILER} "-xc++" "${SOURCE_FILE}" "-std=c++26" "-S" "-emit-llvm" "-o" "${ARG_OUTPUT}"
                     OUTPUT ${ARG_OUTPUT})
  add_custom_target(produce_ir_${OUTPUT_FILENAME} ALL DEPENDS "${ARG_OUTPUT}")
endfunction()

make_ir_file(${CMAKE_CURRENT_SOURCE_DIR}/symtab.cpp OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/symtab.ll)
make_ir_file(${CMAKE_CURRENT_SOURCE_DIR}/section_intrinsics.cpp OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/section-intrinsics.ll)

bleach_add_llvm_based_lib(
  bleach-lifter
  LLVM_COMPONENTS
  support
  mcdisassembler
  target
  linker
  mirparser
  irreader
  SOURCES
  instr-impl.cpp
  lifter.cpp)

target_compile_options(bleach-lifter PRIVATE "--embed-dir=${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(bleach-lifter produce_ir_symtab)
add_dependencies(bleach-lifter produce_ir_section-intrinsics)

target_link_libraries(bleach-lifter PUBLIC yaml-cpp::yaml-cpp mctomir-lifter)
target_sources(
  bleach-lifter
  PUBLIC FILE_SET
         public_headers
         TYPE
         HEADERS
         BASE_DIRS
         ${LLVM_BLEACH_INCLUDE_DIR}
         FILES
         ${LLVM_BLEACH_INCLUDE_DIR}/bleach/lifter/instr-impl.hpp
         ${LLVM_BLEACH_INCLUDE_DIR}/bleach/lifter/lifter.hpp)
target_include_directories(
  bleach-lifter
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
         $<BUILD_INTERFACE:${LLVM_BLEACH_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS bleach-lifter FILE_SET public_headers)
