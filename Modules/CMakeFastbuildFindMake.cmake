# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.


find_program(CMAKE_MAKE_PROGRAM
  NAMES fbuild
  NAMES_PER_DIR
  DOC "Program used to build from fbuild.bff files.")
mark_as_advanced(CMAKE_MAKE_PROGRAM)
