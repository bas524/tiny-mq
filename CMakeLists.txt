cmake_minimum_required(VERSION 3.13)

project(tiny_mq
        VERSION 0.1
        DESCRIPTION "C++ tiny message queue")


option(ENABLE_TESTS "Enable tests" ON)
option(ENABLE_CLANG_TIDY "Check C++ test style with clang-tidy in Debug builds" ON)
# Force CMake to prefer CONFIG packages (vcpkg) over Find*.cmake

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON)

if (POCO_ROOT_DIR)
  list(APPEND CMAKE_MODULE_PATH "${POCO_ROOT_DIR}/lib/cmake/Poco")
  list(APPEND CMAKE_PREFIX_PATH "${POCO_ROOT_DIR}")
endif ()

if (PROTOBUF_ROOT_DIR)
  list(APPEND CMAKE_MODULE_PATH "${PROTOBUF_ROOT_DIR}/cmake")
  list(APPEND CMAKE_PREFIX_PATH "${PROTOBUF_ROOT_DIR}")
endif ()

if (GTEST_ROOT_DIR)
  list(APPEND CMAKE_MODULE_PATH "${GTEST_ROOT_DIR}/lib/cmake/GTest")
  list(APPEND CMAKE_PREFIX_PATH "${GTEST_ROOT_DIR}")
endif ()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(MSVC_COMPILE_FLAGS "/MP")
if (MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MSVC_COMPILE_FLAGS}")
  if (MSVC_TOOLSET_VERSION GREATER 140)
    #        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX /W4")
  endif ()
endif ()
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -Wextra -Wshadow -Wno-deprecated-builtins")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Set a default build type if none was specified
set(default_build_type "Release")

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif ()

if (ENABLE_TESTS AND ENABLE_CLANG_TIDY AND CMAKE_BUILD_TYPE STREQUAL "Debug"
    AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy
               HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
  if (CLANG_TIDY_EXECUTABLE)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    message(STATUS "C++ test clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
  else ()
    message(STATUS "C++ test clang-tidy: executable not found")
  endif ()
endif ()

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT result)
  if (result)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif ()
endif ()

set(SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/share")
set(EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external")

add_subdirectory(external)
include_directories(external/concurrent_queue)
include_directories(external/btree/include)
add_executable(tiny_mq)
add_subdirectory(server)
target_sources(tiny_mq PRIVATE
    server/src/DeliveryClock.cpp
    server/src/LocalTransactionJournal.cpp
    server/src/SessionDispatch.cpp
)

set(POCO_LIBS
    Poco::Util
    Poco::Net
    Poco::XML
    Poco::JSON
    )

# Корень проекта нужен в include-пути всегда: заголовки подключают
# <external/concurrent_queue/...> угловыми скобками. Раньше эта строка целиком
# лежала внутри if (ENABLE_TESTS), из-за чего сборка с ENABLE_TESTS=OFF падала.
target_include_directories(tiny_mq PUBLIC ${CMAKE_CURRENT_LIST_DIR})

if (ENABLE_TESTS)
  target_compile_definitions(tiny_mq PUBLIC ENABLE_TESTS)
  target_include_directories(tiny_mq PUBLIC tests)
  target_sources(tiny_mq PUBLIC
                 tests/TestHelper.h
                 tests/SimpleTest.cpp
                 tests/SimpleTest.h
                 tests/TextMessageTest.cpp
                 tests/TextMessageTest.h
                 tests/BytesMessageTest.cpp
                 tests/BytesMessageTest.h
                 tests/MapMessageTest.cpp
                 tests/MapMessageTest.h
                 tests/StreamMessageTest.cpp
                 tests/StreamMessageTest.h
                 tests/TransactionTest.cpp
                 tests/TransactionTest.h
                 tests/LinearStorageTest.cpp
                 tests/LinearStorageTest.h
                 tests/PersistentTransactionTest.cpp
                 tests/PersistentTransactionTest.h
                 tests/TopicTest.cpp
                 tests/TopicTest.h
                 tests/ClientAckTest.cpp
                 tests/ClientAckTest.h
                 tests/BenchmarkTest.cpp
                 tests/BenchmarkTest.h
                 tests/SelectorTest.cpp
                 tests/SelectorTest.h
                 tests/DeliveryContractTest.cpp
                 tests/DestinationContractTest.cpp
                 tests/DurableSubscriberTest.cpp
                 tests/DurableSubscriberTest.h
                 tests/MessageContractTest.cpp
                 tests/MessageHeadersTest.cpp
                 tests/MessageHeadersTest.h
                 tests/ObjectMessageTest.cpp
                 tests/ObjectMessageTest.h
                 tests/DisableHeadersTest.cpp
                 tests/DisableHeadersTest.h
                 tests/DupsOkAckTest.cpp
                 tests/DupsOkAckTest.h
                 tests/ConnectionLifecycleTest.cpp
                 tests/ConnectionLifecycleTest.h
                 tests/SendOptionsTest.cpp
                 tests/SendOptionsTest.h
                 tests/ExpirationTest.cpp
                 tests/ExpirationTest.h
                 tests/PriorityOrderingTest.cpp
                 tests/PriorityOrderingTest.h
                 tests/DeliveryDelayTest.cpp
                 tests/DeliveryDelayTest.h
                 tests/AnyVisitorTest.cpp
                 tests/AnyVisitorTest.h
                 tests/StorageWorkerResilienceTest.cpp
                 tests/RecoverTest.cpp
                 tests/RecoverTest.h
                 tests/DlqTest.cpp
                 tests/DlqTest.h
                 )
  if (ENABLE_CLANG_TIDY AND CLANG_TIDY_EXECUTABLE AND CMAKE_BUILD_TYPE STREQUAL "Debug"
      AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    get_target_property(tiny_mq_test_sources tiny_mq SOURCES)
    set(tiny_mq_test_headers ${tiny_mq_test_sources})
    list(FILTER tiny_mq_test_sources INCLUDE REGEX "^tests/.*[.]cpp$")
    list(FILTER tiny_mq_test_headers INCLUDE REGEX "^tests/.*[.]h$")
    foreach (tiny_mq_test_source IN LISTS tiny_mq_test_sources)
      get_filename_component(tiny_mq_test_name "${tiny_mq_test_source}" NAME_WE)
      set(tiny_mq_test_lint_stamp "${CMAKE_CURRENT_BINARY_DIR}/${tiny_mq_test_name}.clang-tidy.stamp")
      add_custom_command(
        OUTPUT "${tiny_mq_test_lint_stamp}"
        COMMAND "${CLANG_TIDY_EXECUTABLE}"
                --checks=-*,readability-braces-around-statements
                --warnings-as-errors=readability-braces-around-statements
                "--header-filter=(^|/)tests/"
                -p "${CMAKE_CURRENT_BINARY_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${tiny_mq_test_source}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${tiny_mq_test_lint_stamp}"
        DEPENDS "${tiny_mq_test_source}" ${tiny_mq_test_headers}
                "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        COMMENT "clang-tidy ${tiny_mq_test_source}"
        VERBATIM)
      list(APPEND tiny_mq_test_lint_stamps "${tiny_mq_test_lint_stamp}")
    endforeach ()
    add_custom_target(tiny_mq_test_clang_tidy DEPENDS ${tiny_mq_test_lint_stamps})
    add_dependencies(tiny_mq tiny_mq_test_clang_tidy)
  endif ()
  configure_file(tests/MessageContract.tsv MessageContract.tsv COPYONLY)
  set_source_files_properties(tests/MessageContractTest.cpp PROPERTIES
      COMPILE_DEFINITIONS "TINY_MQ_MESSAGE_CONTRACT_FIXTURE=\"${CMAKE_CURRENT_BINARY_DIR}/MessageContract.tsv\"")
  set(TEST_LIBS
      GTest::gtest
      benchmark::benchmark)
endif ()

target_link_libraries(tiny_mq PRIVATE ${CMAKE_DL_LIBS} ${POCO_LIBS} ${TEST_LIBS} phmap::phmap span::span)
#[[upmq::protocol protobuf::libprotobuf]]
if (NOT WIN32)
  target_link_libraries(tiny_mq PRIVATE Threads::Threads)
else ()
  target_link_libraries(tiny_mq PRIVATE ws2_32 mswsock iphlpapi)
endif ()

add_custom_command(TARGET tiny_mq POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/server/configurations/tiny_mq.properties ${CMAKE_CURRENT_BINARY_DIR}/tiny_mq.properties
                   COMMENT "Copy tiny_mq.properties to ${CMAKE_CURRENT_BINARY_DIR}"
                   )
