#
# This module detects if span is installed and determines where the
# include files and libraries are.
#
# This code sets the following variables:
#
# span_INCLUDE_DIRS   = include dir to be used when using the parallel-hashmap library
# span_FOUND          = set to true if parallel-hashmap was found successfully
#
# span_DIR
#   setting this enables search for parallel-hashmap libraries / headers in this location

find_path(span_INCLUDE_DIRS "nonstd/span.hpp")

if (span_INCLUDE_DIRS)
  message(STATUS "span include dir: ${span_INCLUDE_DIRS}")
endif (span_INCLUDE_DIRS)

# -----------------------------------------------------
# handle the QUIETLY and REQUIRED arguments and set span_FOUND to TRUE if
# all listed variables are TRUE
# -----------------------------------------------------
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(span DEFAULT_MSG span_INCLUDE_DIRS)

add_library(span::span INTERFACE IMPORTED)
set_property(TARGET span::span PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${span_INCLUDE_DIRS}")

mark_as_advanced(span_INCLUDE_DIRS)
