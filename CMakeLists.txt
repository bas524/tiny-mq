cmake_minimum_required(VERSION 3.12...3.13)

project(tiny_mq
        VERSION 0.1
        DESCRIPTION "C++ MQ-tiny_mq")


option(ENABLE_TESTS "Enable tests" ON)

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
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Wextra -Wshadow")
endif ()

set(CMAKE_CXX_STANDARD 14)
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
               BytesMessage.h PocoAnyVisitor.h)
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
                 tests/SimpleTest.cpp
                 tests/SimpleTest.h
                 tests/TextMessageTest.cpp
                 tests/TextMessageTest.h
                 tests/BytesMessageTest.cpp
                 tests/BytesMessageTest.h)
  set(TEST_LIBS
      GTest::GTest
      GTest::Main)
endif ()

target_link_libraries(tiny_mq PRIVATE ${CMAKE_DL_LIBS} ${POCO_LIBS} ${TEST_LIBS} phmap nonstd::span-lite)
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