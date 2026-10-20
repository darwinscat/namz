# SPDX-License-Identifier: MIT
# The documents name the release the code is. Fails unless all of:
#   - README.md's FetchContent example fetches the tag v<VERSION>, the version CMake and namz.h state;
#   - CHANGELOG.md's newest versioned entry is <VERSION> (an "Unreleased" entry, or a ports-only "[ports X.Y.Z]"
#     one, may sit among them);
#   - every versioned CHANGELOG entry has its link: [X.Y.Z]: https://github.com/darwinscat/namz/releases/tag/vX.Y.Z
#
#   cmake -DVERSION=<x.y.z> -DROOT=<repo root> -P docs-version.cmake

foreach(_v VERSION ROOT)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "docs-version: -D${_v}=... is required")
  endif()
endforeach()

set(_bad "")

file(READ "${ROOT}/README.md" _readme)
string(REGEX MATCHALL "GIT_REPOSITORY https://github.com/darwinscat/namz.git GIT_TAG [^ )\r\n]+" _fetches "${_readme}")
if(NOT _fetches)
  string(APPEND _bad "\n  README.md: no FetchContent example for darwinscat/namz")
endif()
foreach(_f IN LISTS _fetches)
  string(REGEX REPLACE ".* GIT_TAG " "" _tag "${_f}")
  if(NOT _tag STREQUAL "v${VERSION}")
    string(APPEND _bad "\n  README.md fetches ${_tag}, the code is v${VERSION}")
  endif()
endforeach()

file(STRINGS "${ROOT}/CHANGELOG.md" _headings REGEX "^## \\[")
file(READ "${ROOT}/CHANGELOG.md" _changelog)
set(_newest "")
foreach(_h IN LISTS _headings)
  if(_h MATCHES "^## \\[Unreleased\\]")
    continue()
  endif()
  # A ports-only release ([ports X.Y.Z]): the js and python ports at X.Y.Z, the C++ reference unchanged — no reference
  # version of its own, so it neither sets the newest entry nor needs a reference release link.
  if(_h MATCHES "^## \\[ports [0-9]+\\.[0-9]+\\.[0-9]+\\]")
    continue()
  endif()
  if(NOT _h MATCHES "^## \\[([0-9]+\\.[0-9]+\\.[0-9]+)\\]")
    string(APPEND _bad "\n  CHANGELOG.md: a heading that names no version: ${_h}")
    continue()
  endif()
  set(_ver "${CMAKE_MATCH_1}")
  if(_newest STREQUAL "")
    set(_newest "${_ver}")
  endif()
  string(FIND "${_changelog}" "\n[${_ver}]: https://github.com/darwinscat/namz/releases/tag/v${_ver}" _at)
  if(_at EQUAL -1)
    string(APPEND _bad "\n  CHANGELOG.md: [${_ver}] has no link to its release")
  endif()
endforeach()
if(NOT _newest STREQUAL VERSION)
  string(APPEND _bad "\n  CHANGELOG.md's newest entry is [${_newest}], the code is ${VERSION}")
endif()

if(_bad)
  message(FATAL_ERROR "docs-version:${_bad}")
endif()
message(STATUS "docs-version: README.md and CHANGELOG.md name ${VERSION}")
