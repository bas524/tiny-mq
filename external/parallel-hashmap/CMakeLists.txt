list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
find_package(phmap REQUIRED)

if (phmap_FOUND)
  set_target_properties(phmap::phmap
                        PROPERTIES IMPORTED_GLOBAL TRUE)
else ()
  message(STATUS "Try to set phmap_DIR, for example cmake -Dphmap_DIR=/usr/local")
endif ()
