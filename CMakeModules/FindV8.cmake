# Finds V8 the way distributions ship it: inside Node.js's shared library (Fedora nodejs<N>-devel, Debian/Ubuntu
# libnode-dev), headers under include/node. V8_INCLUDE_DIR / V8_LIBRARY can point anywhere else.
#
# This module defines:
# V8_FOUND
# V8_INCLUDE_DIR
# V8_LIBRARY
# V8_VERSION
# V8_DEFINITIONS - defines that change V8's object layout, plus V8_ICU_DEFAULT_DATA_DIR when node has one
# V8::V8 - imported target carrying all of the above

find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
  pkg_search_module(PC_NODE QUIET nodejs-26 nodejs-25 nodejs-24 nodejs-23 nodejs-22 nodejs-21 nodejs-20 libnode)
endif()

find_library(
  V8_LIBRARY
  NAMES node v8
  HINTS ${PC_NODE_LIBRARY_DIRS}
)

# the headers have to be the library's own: a Node.js tarball in /usr/local brings include/node but no libnode, and
# its V8 version differs from the distribution's
set(_V8_PREFIX_HINTS)
if(V8_LIBRARY)
  get_filename_component(_V8_LIBRARY_DIR "${V8_LIBRARY}" DIRECTORY)
  list(APPEND _V8_PREFIX_HINTS "${_V8_LIBRARY_DIR}/../include" "${_V8_LIBRARY_DIR}/../../include")
endif()

find_path(
  V8_INCLUDE_DIR
  NAMES v8.h
  HINTS ${PC_NODE_INCLUDE_DIRS} ${_V8_PREFIX_HINTS}
  PATH_SUFFIXES node nodejs/deps/v8/include v8
)

set(V8_DEFINITIONS)

if(V8_INCLUDE_DIR AND EXISTS "${V8_INCLUDE_DIR}/v8-version.h")
  file(STRINGS "${V8_INCLUDE_DIR}/v8-version.h" _V8_VERSION_LINES REGEX "#define V8_(MAJOR_VERSION|MINOR_VERSION|BUILD_NUMBER)")
  foreach(_part MAJOR_VERSION MINOR_VERSION BUILD_NUMBER)
    string(REGEX MATCH "V8_${_part} +([0-9]+)" _match "${_V8_VERSION_LINES}")
    set(_V8_${_part} "${CMAKE_MATCH_1}")
  endforeach()
  set(V8_VERSION "${_V8_MAJOR_VERSION}.${_V8_MINOR_VERSION}.${_V8_BUILD_NUMBER}")

  # node writes its build configuration next to the headers (config.gypi, Fedora's multilib config-<arch>.gypi);
  # these switches are what node's own common.gypi turns into defines for addons
  file(GLOB _V8_GYPI_FILES "${V8_INCLUDE_DIR}/config*.gypi")
  set(_V8_CONFIG "")
  foreach(_file ${_V8_GYPI_FILES})
    file(READ "${_file}" _content)
    string(APPEND _V8_CONFIG "${_content}")
  endforeach()

  macro(_v8_flag variable)
    string(REGEX MATCH "['\"]${variable}['\"]: *['\"]?(1|true)" _enabled "${_V8_CONFIG}")
    if(_enabled)
      list(APPEND V8_DEFINITIONS ${ARGN})
    endif()
  endmacro()

  _v8_flag(v8_enable_pointer_compression V8_COMPRESS_POINTERS)
  _v8_flag(v8_enable_pointer_compression_shared_cage V8_COMPRESS_POINTERS_IN_SHARED_CAGE)
  _v8_flag(v8_enable_31bit_smis_on_64bit_arch V8_31BIT_SMIS_ON_64BIT_ARCH)
  _v8_flag(v8_enable_sandbox V8_ENABLE_SANDBOX)

  # Fedora's node embeds a small ICU and loads the full data (nodejs<N>-full-i18n) from here
  string(REGEX MATCH "['\"]icu_default_data['\"]: *['\"]([^'\"]+)['\"]" _icu_default_data "${_V8_CONFIG}")
  if(CMAKE_MATCH_1)
    list(APPEND V8_DEFINITIONS "V8_ICU_DEFAULT_DATA_DIR=\"${CMAKE_MATCH_1}\"")
  endif()
endif()

if(V8_INCLUDE_DIR AND NOT EXISTS "${V8_INCLUDE_DIR}/libplatform/libplatform.h")
  message(STATUS "V8 headers in ${V8_INCLUDE_DIR} have no libplatform/libplatform.h")
  unset(V8_INCLUDE_DIR CACHE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  V8
  REQUIRED_VARS V8_LIBRARY V8_INCLUDE_DIR
  VERSION_VAR V8_VERSION
  REASON_FAILURE_MESSAGE
    "V8 comes from Node.js's development package: nodejs24-devel or nodejs22-devel (Fedora), libnode-dev (Debian/Ubuntu), \
the AUR's libnode (Arch). Elsewhere set V8_INCLUDE_DIR (the directory with v8.h) and V8_LIBRARY (libnode.so)."
)

if(V8_FOUND AND NOT TARGET V8::V8)
  add_library(V8::V8 UNKNOWN IMPORTED)
  set_target_properties(V8::V8 PROPERTIES
    IMPORTED_LOCATION "${V8_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${V8_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "${V8_DEFINITIONS}")
endif()

mark_as_advanced(V8_INCLUDE_DIR V8_LIBRARY)
