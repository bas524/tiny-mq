#
# This module detects if phmap is installed and determines where the
# include files and libraries are.
#
# This code sets the following variables:
#
# phmap_INCLUDE_DIRS   = include dir to be used when using the parallel-hashmap library
# phmap_FOUND          = set to true if parallel-hashmap was found successfully
#
# phmap_DIR
#   setting this enables search for parallel-hashmap libraries / headers in this location

find_path(phmap_INCLUDE_DIRS "parallel_hashmap/btree.h")

if (phmap_INCLUDE_DIRS)
  message(STATUS "phmap include dir: ${phmap_INCLUDE_DIRS}")
endif (phmap_INCLUDE_DIRS)

# -----------------------------------------------------
# handle the QUIETLY and REQUIRED arguments and set phmap_FOUND to TRUE if
# all listed variables are TRUE
# -----------------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(phmap DEFAULT_MSG phmap_INCLUDE_DIRS)

add_library(phmap::phmap INTERFACE IMPORTED)
set_property(TARGET phmap::phmap PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${phmap_INCLUDE_DIRS}")

mark_as_advanced(phmap_INCLUDE_DIRS)
