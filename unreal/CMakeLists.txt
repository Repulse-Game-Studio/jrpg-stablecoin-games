# Builds the C++ core (ScfgCore), its test binary, the integration driver, the compile-only checks (poisoned,
# unity) and the export check (the core as a hidden-visibility shared library).
cmake_minimum_required(VERSION 3.25)
project(scfg_unreal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every target in this directory (core, tests, driver) gets the same flags: no exceptions, no RTTI, as UBT.
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wundef -Wnon-virtual-dtor -Werror -fno-exceptions -fno-rtti)

# Ruling R13: newer Clang (22) reports SCFG_TRY's __COUNTER__ as a C2y extension under -Wpedantic. Every compiler
# Unreal supports has __COUNTER__, so silence that one warning. Only on Clang, and only where the flag exists: Clang 18
# refuses an unknown -Wno- name under -Werror.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-Wc2y-extensions SCFG_HAS_WC2Y_EXTENSIONS)
  if(SCFG_HAS_WC2Y_EXTENSIONS)
    add_compile_options(-Wno-c2y-extensions)
  endif()
endif()

# Recursive, as UBT compiles a module and as the lints walk it: a source in any subfolder of the module is compiled
# by every target below, and a header in any subfolder of Public gets its own header TU.
set(SCFG_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Scfg/Source/ScfgCore)
file(GLOB_RECURSE SCFG_CORE_SOURCES CONFIGURE_DEPENDS ${SCFG_CORE_DIR}/*.cpp)
# The module's registration (IMPLEMENT_MODULE) is the one Unreal file of the module: only UBT compiles it.
list(FILTER SCFG_CORE_SOURCES EXCLUDE REGEX "/Private/ScfgCoreModule\\.cpp$")
file(GLOB_RECURSE SCFG_CORE_HEADERS CONFIGURE_DEPENDS ${SCFG_CORE_DIR}/Public/*.h)

add_library(scfg_core STATIC ${SCFG_CORE_SOURCES})
target_include_directories(scfg_core PUBLIC ${SCFG_CORE_DIR}/Public)

# The core as Unreal sees it: every source and every public header on its own (one generated TU each), with the
# Unreal/Windows macros that break plain C++ defined to poison tokens first.
# Each TU mirrors its header's path (header-tus/Scfg/Json.h.cpp), so two headers never share a TU (N2: a flattened
# name such as MAKE_C_IDENTIFIER's Scfg_Json_h would give Scfg/A_B.h and Scfg/A/B.h the same one).
set(SCFG_HEADER_TUS)
foreach(header IN LISTS SCFG_CORE_HEADERS)
  file(RELATIVE_PATH included ${SCFG_CORE_DIR}/Public ${header})  # Scfg/Json.h
  set(tu ${CMAKE_CURRENT_BINARY_DIR}/header-tus/${included}.cpp)
  file(CONFIGURE OUTPUT ${tu} CONTENT "#include \"${included}\"\n")
  list(APPEND SCFG_HEADER_TUS ${tu})
endforeach()
add_library(scfg_core_poisoned OBJECT ${SCFG_CORE_SOURCES} ${SCFG_HEADER_TUS})
target_include_directories(scfg_core_poisoned PRIVATE ${SCFG_CORE_DIR}/Public)
target_compile_options(scfg_core_poisoned PRIVATE
  -include ${CMAKE_CURRENT_SOURCE_DIR}/core-tests/support/UnrealMacroPoison.h)

# The core as one translation unit, as UBT's unity build compiles it: file-local names must not collide.
add_library(scfg_core_unity OBJECT ${SCFG_CORE_SOURCES})
target_include_directories(scfg_core_unity PRIVATE ${SCFG_CORE_DIR}/Public)
set_target_properties(scfg_core_unity PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)

# Ruling R2-3: the core as a shared library that exports only what carries SCFGCORE_API, as an Unreal module's .so
# does (-fvisibility=hidden is stricter than UBT's -fvisibility-ms-compat: it hides classes too), and a program that
# links against it and calls into several of its files. A declaration that lost its export fails this link.
add_library(scfg_core_shared SHARED ${SCFG_CORE_SOURCES})
target_include_directories(scfg_core_shared PUBLIC ${SCFG_CORE_DIR}/Public)
target_compile_definitions(scfg_core_shared PUBLIC "SCFGCORE_API=__attribute__((visibility(\"default\")))")
set_target_properties(scfg_core_shared PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
add_executable(scfg_core_exports ${CMAKE_CURRENT_SOURCE_DIR}/core-exports/Main.cpp)
target_link_libraries(scfg_core_exports PRIVATE scfg_core_shared)
set_target_properties(scfg_core_exports PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

file(GLOB_RECURSE SCFG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core-tests/*.cpp)
add_executable(scfg_core_tests ${SCFG_TEST_SOURCES})
target_link_libraries(scfg_core_tests PRIVATE scfg_core)
target_include_directories(scfg_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core-tests)

# The JSON-lines driver tests/integration/cpp_driver.py runs against local chains: the core, the Tier 2 encoders it
# reuses (support/Tier2, which does not need the test runner, ruling R6) and a blocking POSIX HTTP transport. Test-only
# code, built by default so every build covers it.
add_executable(scfg_core_driver
  ${CMAKE_CURRENT_SOURCE_DIR}/core-driver/Main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core-driver/PosixHttpTransport.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core-tests/support/Tier2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/core-tests/support/JsonEq.cpp)
target_link_libraries(scfg_core_driver PRIVATE scfg_core)
target_include_directories(scfg_core_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core-tests)

# The JSON-lines driver of tests/server_security's cpp_server half (server_handles.py's DriverServer protocol): the
# core's local_server::CheckoutServer on non-blocking POSIX sockets bound to 127.0.0.1. Test-only, built by default.
add_executable(scfg_server_driver ${CMAKE_CURRENT_SOURCE_DIR}/core-driver/ServerMain.cpp)
target_link_libraries(scfg_server_driver PRIVATE scfg_core)

# One ctest test per group. A new test group is added here: RunnerTests.EveryGroupIsRegistered fails if the binary
# has a group that is not in SCFG_TEST_GROUPS, or SCFG_TEST_GROUPS one the binary does not have.
set(SCFG_TEST_GROUPS LintTests RunnerTests ErrTests BigIntTests BytesTests JsonTests PyTests PyFloatTests IntsTests
  LocaleTests Tier1Tests DecTests AmountsTests Sha256Tests KeccakTests EvmTests
  Base58Tests Ed25519Tests ChainsTests ConfigTests PaymentsTests ReceiptsTests SessionsTests EntropyTests
  PdaTests SolanaTxTests QrTests PaylinksTests CoTests CoioTests RpcTests SolanaRpcTests Tier2Tests EngineTests
  SolanaBackendTests ReverifyTests CheckoutTests CheckoutStepsTests CheckoutSolanaTests CheckoutAbandonedStartTests
  RestoreFlowTests LocalServerTests Tier1 Tier2 Tier2Deferred)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH SCFG_REPO_ROOT)
string(JOIN "," SCFG_TEST_GROUPS_CSV ${SCFG_TEST_GROUPS})
target_compile_definitions(scfg_core_tests PRIVATE
  SCFG_REPO_ROOT="${SCFG_REPO_ROOT}"
  SCFG_CTEST_GROUPS="${SCFG_TEST_GROUPS_CSV}")

enable_testing()
foreach(group IN LISTS SCFG_TEST_GROUPS)
  add_test(NAME ${group} COMMAND scfg_core_tests --group ${group})
endforeach()
add_test(NAME SharedLibraryExports COMMAND scfg_core_exports)
