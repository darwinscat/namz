# SPDX-License-Identifier: MIT
# Runs the CLI once and requires all of: its exit code, a message on stderr, and — when OUT is given — that
# no file named OUT exists afterwards. A refusal that still writes the file, or exits with the wrong code,
# or says something else, fails. (CTest's own PASS_REGULAR_EXPRESSION ignores the exit code, and WILL_FAIL
# passes on any failure at all, an "unknown option" included.)
#
#   cmake -DCLI=<namz> -DEXIT=<code> -DSTDERR=<regex> [-DOUT=<file>] [-DSTDOUT_FILE=<file>]
#         -P cli-expect.cmake -- <cli args...>
#
# OUT is removed before the run, so a file left by an earlier run cannot pass for one this run wrote.
# STDOUT_FILE holds the exact stdout expected, byte for byte but for line endings and trailing whitespace
# (Windows prints a CRLF, a checkout may hold one).

foreach(_v CLI EXIT STDERR)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "cli-expect: -D${_v}=... is required")
  endif()
endforeach()

set(_args "")
set(_after FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_after)
    list(APPEND _args "${CMAKE_ARGV${_i}}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_after TRUE)
  endif()
endforeach()
if(NOT _after)
  message(FATAL_ERROR "cli-expect: the CLI's arguments follow a --")
endif()

if(DEFINED OUT)
  file(REMOVE "${OUT}")
endif()

execute_process(COMMAND "${CLI}" ${_args} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)

set(_bad "")
if(NOT "${_rc}" STREQUAL "${EXIT}")
  string(APPEND _bad "\n  exit code ${_rc}, expected ${EXIT}")
endif()
if(NOT "${_err}" MATCHES "${STDERR}")
  string(APPEND _bad "\n  stderr does not match \"${STDERR}\"")
endif()
if(DEFINED OUT AND EXISTS "${OUT}")
  string(APPEND _bad "\n  ${OUT} was written")
endif()
if(DEFINED STDOUT_FILE)
  file(READ "${STDOUT_FILE}" _want)
  foreach(_s _want _out)
    string(REPLACE "\r\n" "\n" ${_s} "${${_s}}")
    string(REGEX REPLACE "[ \t\r\n]+$" "" ${_s} "${${_s}}")
  endforeach()
  if(NOT "${_out}" STREQUAL "${_want}")
    string(APPEND _bad "\n  stdout is not ${STDOUT_FILE}:\n  got  [${_out}]\n  want [${_want}]")
  endif()
endif()
if(_bad)
  message(FATAL_ERROR "namz ${_args}:${_bad}\n  stderr: ${_err}")
endif()
message(STATUS "namz ${_args}: exit ${_rc}, stderr matches")
