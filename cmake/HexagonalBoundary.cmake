# Build-time enforcement of the hexagonal architecture boundary — the C++
# analog of Python's import-linter. enforce_no_dependency() walks the transitive
# link graph of a target and fails the configure step if it reaches a forbidden
# target. This is what keeps `domain` pure: a stray `target_link_libraries(
# broker_exec_domain ... broker_exec_adapters)` becomes a hard build error.

function(_brokerexec_direct_link_targets target out_var)
  set(_acc "")
  foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_libs ${target} ${_prop})
    if(_libs)
      foreach(_lib ${_libs})
        if(TARGET ${_lib})
          list(APPEND _acc ${_lib})
        endif()
      endforeach()
    endif()
  endforeach()
  if(_acc)
    list(REMOVE_DUPLICATES _acc)
  endif()
  set(${out_var} "${_acc}" PARENT_SCOPE)
endfunction()

function(enforce_no_dependency)
  cmake_parse_arguments(A "" "TARGET" "FORBIDS" ${ARGN})
  if(NOT TARGET ${A_TARGET})
    message(FATAL_ERROR "enforce_no_dependency: unknown target '${A_TARGET}'")
  endif()

  set(_seen "")
  set(_frontier "${A_TARGET}")
  while(_frontier)
    list(POP_FRONT _frontier _cur)
    if(_cur IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen ${_cur})

    foreach(_forbidden ${A_FORBIDS})
      if(_cur STREQUAL _forbidden)
        message(FATAL_ERROR
          "Hexagonal boundary violation: '${A_TARGET}' links (transitively) the "
          "forbidden target '${_forbidden}'. The inner layers must not depend on "
          "adapters/transports.")
      endif()
    endforeach()

    _brokerexec_direct_link_targets(${_cur} _deps)
    foreach(_d ${_deps})
      if(NOT _d IN_LIST _seen)
        list(APPEND _frontier ${_d})
      endif()
    endforeach()
  endwhile()

  message(STATUS "Boundary OK: ${A_TARGET} does not link ${A_FORBIDS}")
endfunction()
