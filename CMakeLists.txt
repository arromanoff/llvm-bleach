cmake_minimum_required(VERSION 3.23)
include(cmake/version-utils.cmake)

get_source_info(${CMAKE_CURRENT_SOURCE_DIR} LLVM_BLEACH_VERSION)
message(STATUS "LLVM_BLEACH_VERSION: " ${LLVM_BLEACH_VERSION})

project(
  llvm-bleach
  VERSION ${LLVM_BLEACH_VERSION}
  LANGUAGES C CXX)

option(INSOURCEBUILD OFF) # Prevent in-source style builds
if((${PROJECT_SOURCE_DIR} STREQUAL ${PROJECT_BINARY_DIR}) AND NOT
                                                              ${INSOURCEBUILD})
  message(
    FATAL_ERROR
      "In-source building disabled. Provide -DINSOURCEBUILD option if you are sure about it."
  )
endif()

option(SANITIZE OFF)
if(${SANITIZE})
  add_compile_options(-fsanitize=undefined,address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined,address -fno-omit-frame-pointer)
endif()

option(THREAD_SANITIZE OFF)
if(${THREAD_SANITIZE})
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
endif()

if(THREAD_SANITIZE AND SANITIZE)
  message(FATAL_ERROR "Thread and address sanitizer can't be used together")
endif()

include(cmake/functions.cmake)
include(GNUInstallDirs)

function(add_example_executable TARGET_NAME SOURCE)
  add_executable(${TARGET_NAME} ${SOURCE})
  target_enable_linter(${TARGET_NAME})
  target_compile_features(${TARGET_NAME} PUBLIC cxx_std_26)
  enable_warnings(${TARGET_NAME})
endfunction()

set(LLVM_BLEACH_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(bleach_add_library target_name)
  add_library(${target_name} ${ARGN})
  set_target_properties(
    ${target_name}
    PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

function(bleach_add_executable target_name)
  set(multi_value_args SOURCES)
  cmake_parse_arguments(ARGUMENTS "" "" "${multi_value_args}" ${ARGN})
  add_executable(${target_name} ${ARGUMENTS_SOURCES})
  set_target_properties(
    ${target_name}
    PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

function(bleach_add_llvm_based_tool target_name)
  set(options_args SHARED)
  set(multi_value_args LLVM_COMPONENTS SOURCES)
  cmake_parse_arguments(ARGUMENTS "${options_args}" "" "${multi_value_args}"
                        ${ARGN})
  bleach_add_executable(${target_name} SOURCES ${ARGUMENTS_SOURCES})
  enable_warnings(${target_name})
  llvm_map_components_to_libnames(LLVM_LIBRARIES ${ARGUMENTS_LLVM_COMPONENTS})
  target_link_libraries(${target_name} PUBLIC ${LLVM_LIBRARIES})
  target_compile_features(${target_name} PUBLIC cxx_std_26)
endfunction()

function(bleach_add_llvm_based_lib target_name)
  set(options_args SHARED)
  set(multi_value_args LLVM_COMPONENTS SOURCES)
  cmake_parse_arguments(ARGUMENTS "${options_args}" "" "${multi_value_args}"
                        ${ARGN})
  if(${ARGUMENTS_SHARED})
    bleach_add_library(${target_name} SHARED ${ARGUMENTS_SOURCES})
  else()
    bleach_add_library(${target_name} STATIC ${ARGUMENTS_SOURCES})
  endif()
  enable_warnings(${target_name})
  llvm_map_components_to_libnames(LLVM_LIBRARIES ${ARGUMENTS_LLVM_COMPONENTS})
  target_link_libraries(${target_name} PUBLIC ${LLVM_LIBRARIES})
  target_include_directories(${target_name} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_features(${target_name} PUBLIC cxx_std_26)
endfunction()

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(tools)

find_program(PANDOC_PROGRAM pandoc HINTS ${PANDOC_PATH})
if (PANDOC_PROGRAM)
  add_subdirectory(docs)
else()
  message(WARNING "pandoc executable not found in PATH or PANDOC_PATH variable. Skipping documentation generation")
endif()

string(REGEX MATCH "^([0-9]+)\.([0-9]+)\.([0-9]+)"
  LLVM_BLEACH_SEMVER_PARSE_MATCH ${LLVM_BLEACH_VERSION})

if(LLVM_BLEACH_SEMVER_PARSE_MATCH STREQUAL "")
  message(
    FATAL_ERROR
      "Invalid llvm-bleach version specified: ${LLVM_BLEACH_VERSION}, expected a semantic version"
  )
endif()

set(LLVM_BLEACH_VERSION_MAJOR ${CMAKE_MATCH_1})
set(LLVM_BLEACH_VERSION_MINOR ${CMAKE_MATCH_2})
set(LLVM_BLEACH_VERSION_PATCH ${CMAKE_MATCH_3})

message(STATUS "LLVM_BLEACH_VERSION: ${LLVM_BLEACH_VERSION}")
message(STATUS "LLVM_BLEACH_VERSION_MAJOR: ${LLVM_BLEACH_VERSION_MAJOR}")
message(STATUS "LLVM_BLEACH_VERSION_MINOR: ${LLVM_BLEACH_VERSION_MINOR}")
message(STATUS "LLVM_BLEACH_VERSION_PATCH: ${LLVM_BLEACH_VERSION_PATCH}")

include(CTest)
set(CMAKE_CTEST_ARGUMENTS "--verbose")
enable_testing()
add_subdirectory(test)

option(LLVM_BLEACH_BUILD_DOCS OFF)
if(LLVM_BLEACH_BUILD_DOCS)
  find_package(Doxygen REQUIRED OPTIONAL_COMPONENTS mscgen dia)

  doxygen_add_docs(${CMAKE_PROJECT}_docs include ALL)
endif()
