find_program(CLANG_COMPILER clang HINTS ${BLEACH_CLANG_COMPILER_PATH})
if(NOT CLANG_COMPILER)
  message(
    FATAL_ERROR
      "No clang compiler found for building symtab.cpp. Try specifying BLEACH_CLANG_COMPILER_PATH"
  )
endif()

function(make_ir_file SOURCE_FILE)
  set(single_value_args IN_HEADER)
  cmake_parse_arguments(ARG "" "${single_value_args}" "" ${ARGN})
  get_filename_component(HEADERNAME_ONLY "${ARG_IN_HEADER}" NAME_WLE)
  get_filename_component(HEADERNAME_WE "${ARG_IN_HEADER}" NAME_WE)
  configure_file(${ARG_IN_HEADER}
                 "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.1" COPYONLY)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}"
    COMMAND ${CLANG_COMPILER} "-xc++" "-std=c++23" "-S" "-emit-llvm" "-o"
            "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.2" "${SOURCE_FILE}"
    COMMAND ${CMAKE_COMMAND} -E "echo" "\")\\\";}}\"" >
            "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.3"
    COMMAND
      ${CMAKE_COMMAND} -E "cat"
      "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.1"
      "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.2"
      "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}.3" >
      "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}"
    COMMAND
      ${CMAKE_COMMAND}
      -DINPUT_FILE_PATH="${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}"
      -DOUTPUT_FILE_PATH="${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}" -P
      ${CMAKE_CURRENT_SOURCE_DIR}/replace_ir_target.cmake
    COMMENT "Pre-compiling ${SOURCE_FILE} into IR module"
    DEPENDS "${SOURCE_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/${IN_HEADER}"
            ${CMAKE_CURRENT_SOURCE_DIR}/replace_ir_target.cmake)
  add_custom_target(produce_ir_header_${HEADERNAME_WE} ALL
                    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${HEADERNAME_ONLY}")
endfunction()

make_ir_file(${CMAKE_CURRENT_SOURCE_DIR}/symtab.cpp IN_HEADER symtab-ir.h.in)
make_ir_file(${CMAKE_CURRENT_SOURCE_DIR}/section_intrinsics.cpp IN_HEADER
             section_intrinsics-ir.h.in)

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

add_dependencies(bleach-lifter produce_ir_header_symtab-ir)
add_dependencies(bleach-lifter produce_ir_header_section_intrinsics-ir)

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
