bleach_add_llvm_based_lib(
  mctomir-lifter
  LLVM_COMPONENTS
  support
  mcdisassembler
  target
  mirparser
  irreader
  SOURCES
  elf-loader.cpp
  symbols.cpp
  mctomir-transform.cpp)

target_link_libraries(mctomir-lifter PUBLIC yaml-cpp::yaml-cpp)
if(MCTOMIR_ENABLE_TMS320)
  # PUBLIC: the option also controls fields in the lifter's public headers.
  target_compile_definitions(mctomir-lifter PUBLIC MCTOMIR_ENABLE_TMS320)
endif()
target_include_directories(
  mctomir-lifter PUBLIC ${LLVM_BLEACH_INCLUDE_DIR})
