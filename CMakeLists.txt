cmake_minimum_required(VERSION 3.20)
project(HLSSpatialVerify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#===-- LLVM --------------------------------------------------------------===#
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
if(LLVM_VERSION_MAJOR VERSION_LESS 17 OR LLVM_VERSION_MAJOR VERSION_GREATER 20)
  message(FATAL_ERROR "HLSSpatialVerify supports LLVM 17-20, found ${LLVM_PACKAGE_VERSION}")
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(HLS_SV_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${HLS_SV_LLVM_DEFINITIONS})

#===-- Z3 ----------------------------------------------------------------===#
find_package(Z3 CONFIG QUIET)
if(TARGET z3::libz3)
  set(HLS_SV_Z3_TARGET z3::libz3)
  message(STATUS "Using Z3 ${Z3_VERSION_STRING} (CMake config)")
else()
  find_path(Z3_CXX_INCLUDE_DIR z3++.h REQUIRED)
  find_library(Z3_LIBRARY NAMES z3 libz3 REQUIRED)
  add_library(hls_sv_z3 INTERFACE IMPORTED)
  set_target_properties(hls_sv_z3 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${Z3_CXX_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${Z3_LIBRARY}")
  set(HLS_SV_Z3_TARGET hls_sv_z3)
  message(STATUS "Using Z3 from ${Z3_LIBRARY}")
endif()

#===-- Pass plugin -------------------------------------------------------===#
# A loadable module for `opt -load-pass-plugin`. LLVM component libraries are
# deliberately NOT linked: opt already provides those symbols, and linking them
# again would register every cl::opt twice.
add_library(HLSSpatialVerify MODULE
  src/MemoryModel.cpp
  src/BankPredicate.cpp
  src/SMTTranslator.cpp
  src/SpatialVerificationPass.cpp)

set_target_properties(HLSSpatialVerify PROPERTIES
  PREFIX ""
  SUFFIX ".so")

target_include_directories(HLSSpatialVerify PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Z3's C++ API throws; exceptions are caught inside the plugin.
target_compile_options(HLSSpatialVerify PRIVATE -fexceptions)
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(HLSSpatialVerify PRIVATE -fno-rtti)
endif()

target_link_libraries(HLSSpatialVerify PRIVATE ${HLS_SV_Z3_TARGET})

if(APPLE)
  # Resolve LLVM symbols from the host opt at load time, as LLVM's own
  # add_llvm_pass_plugin does on Darwin.
  target_link_options(HLSSpatialVerify PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()

