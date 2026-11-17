# Builds the C++ core (ScfgCore), its test binary, the integration driver and the compile-only checks (poisoned,
# unity).
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

set(SCFG_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Scfg/Source/ScfgCore)
file(GLOB SCFG_CORE_SOURCES CONFIGURE_DEPENDS ${SCFG_CORE_DIR}/Private/*.cpp)
file(GLOB SCFG_CORE_HEADERS CONFIGURE_DEPENDS ${SCFG_CORE_DIR}/Public/Scfg/*.h)

add_library(scfg_core STATIC ${SCFG_CORE_SOURCES})
target_include_directories(scfg_core PUBLIC ${SCFG_CORE_DIR}/Public)

# The core as Unreal sees it: every source and every public header on its own (one generated TU each), with the
# Unreal/Windows macros that break plain C++ defined to poison tokens first.
set(SCFG_HEADER_TUS)
foreach(header IN LISTS SCFG_CORE_HEADERS)
  get_filename_component(stem ${header} NAME_WE)
  set(tu ${CMAKE_CURRENT_BINARY_DIR}/header-tus/${stem}_h.cpp)
  file(CONFIGURE OUTPUT ${tu} CONTENT "#include \"Scfg/${stem}.h\"\n")
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

# One ctest test per group. Each task that adds a test group appends it here; RunnerTests.EveryGroupIsRegistered
# fails if the binary has a group that is not in SCFG_TEST_GROUPS. SCFG_PENDING_GROUPS held the vector groups that
# existed before every topic was ported (ruling R1); it is empty now, and EveryGroupIsRegistered requires it to stay
# empty.
set(SCFG_TEST_GROUPS LintTests RunnerTests ErrTests BigIntTests BytesTests JsonTests PyTests PyFloatTests IntsTests
  LocaleTests Tier1Tests DecTests AmountsTests Sha256Tests KeccakTests EvmTests
  Base58Tests Ed25519Tests ChainsTests ConfigTests PaymentsTests ReceiptsTests SessionsTests EntropyTests
  PdaTests SolanaTxTests QrTests PaylinksTests CoTests CoioTests RpcTests SolanaRpcTests Tier2Tests EngineTests
  SolanaBackendTests ReverifyTests Tier1 Tier2 Tier2Deferred)
set(SCFG_PENDING_GROUPS)

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH SCFG_REPO_ROOT)
string(JOIN "," SCFG_TEST_GROUPS_CSV ${SCFG_TEST_GROUPS})
string(JOIN "," SCFG_PENDING_GROUPS_CSV ${SCFG_PENDING_GROUPS})
target_compile_definitions(scfg_core_tests PRIVATE
  SCFG_REPO_ROOT="${SCFG_REPO_ROOT}"
  SCFG_CTEST_GROUPS="${SCFG_TEST_GROUPS_CSV}"
  SCFG_PENDING_GROUPS="${SCFG_PENDING_GROUPS_CSV}")

enable_testing()
foreach(group IN LISTS SCFG_TEST_GROUPS)
  add_test(NAME ${group} COMMAND scfg_core_tests --group ${group})
endforeach()
