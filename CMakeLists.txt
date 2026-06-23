cmake_minimum_required(VERSION 3.13)

project(tiny_mq
        VERSION 0.1
        DESCRIPTION "C++ tiny message queue")


option(ENABLE_TESTS "Enable tests" ON)
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
add_executable(tiny_mq
               main.cpp
               Exchange.cpp
               Exchange.h
               DestinationType.h
               Destination.cpp
               Destination.h
               Producer.cpp
               Producer.h
               Consumer.cpp
               Consumer.h
               Message.cpp
               Message.h
               LogTracer.cpp
               LogTracer.h
               DestinationHash.cpp
               DestinationHash.h
               MessageProperty.h
               MessageProperty.cpp
               TextMessage.cpp
               TextMessage.h
               BytesMessage.cpp
               BytesMessage.h
               PocoAnyVisitor.h
               MapMessage.cpp
               MapMessage.h
               StreamMessage.cpp
               StreamMessage.h
               ObjectMessage.cpp
               ObjectMessage.h
               Session.cpp
               Session.h
               Connection.cpp
               Connection.h
               ConnectionFactory.cpp
               ConnectionFactory.h
               HashHelper.h
               LinearStorage.cpp
               LinearStorage.h
               ConcurrentLinearStorage.cpp
               ConcurrentLinearStorage.h
               TransactionBuffer.cpp
               TransactionBuffer.h
               Selector.cpp
               Selector.h
               )

set(POCO_LIBS
    Poco::Foundation
    Poco::Util
    Poco::Net
    Poco::XML
    Poco::JSON
    )

if (ENABLE_TESTS)
  target_compile_definitions(tiny_mq PUBLIC ENABLE_TESTS)
  target_include_directories(tiny_mq PUBLIC tests ${CMAKE_CURRENT_LIST_DIR})
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
                 tests/DurableSubscriberTest.cpp
                 tests/DurableSubscriberTest.h
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
                 )
  set(TEST_LIBS
      GTest::gtest
      GTest::gtest_main
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
                   COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/configurations/tiny_mq.properties ${CMAKE_CURRENT_BINARY_DIR}/tiny_mq.properties
                   COMMENT "Copy tiny_mq.properties to ${CMAKE_CURRENT_BINARY_DIR}"
                   )
