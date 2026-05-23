list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
find_package(span REQUIRED)

if (span_FOUND)
  set_target_properties(span::span
                        PROPERTIES IMPORTED_GLOBAL TRUE)
else ()
  message(STATUS "Try to set span_DIR, for example cmake -Dspan_DIR=/usr/local")
endif ()
