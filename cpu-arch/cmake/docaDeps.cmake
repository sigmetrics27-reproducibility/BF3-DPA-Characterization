include_guard(GLOBAL)


set(DOCA_ROOT "/opt/mellanox/doca" CACHE PATH "Root path of NVIDIA DOCA installation")


function(_doca_pkg_to_prefix pkg_name out_prefix)
  string(TOUPPER "${pkg_name}" _prefix)
  string(REGEX REPLACE "[^A-Z0-9]" "_" _prefix "${_prefix}")
  set(${out_prefix} "DOCA_PKG_${_prefix}" PARENT_SCOPE)
endfunction()


function(doca_create_runtime_target)
  set(oneValueArgs TARGET_NAME)
  set(multiValueArgs PACKAGES)
  cmake_parse_arguments(DOCA "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if("${DOCA_TARGET_NAME}" STREQUAL "")
    set(DOCA_TARGET_NAME "doca::runtime")
  endif()

  if("${DOCA_PACKAGES}" STREQUAL "")
    set(DOCA_PACKAGES
      doca-common
      doca-dpa
      doca-rdma
      doca-argp
      libflexio
    )
  endif()

  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)


  set(_link_targets Threads::Threads)
  set(_all_include_dirs)
  set(_all_library_dirs)
  set(_all_libraries)

  foreach(_pkg IN LISTS DOCA_PACKAGES)
    _doca_pkg_to_prefix("${_pkg}" _prefix)
    pkg_check_modules(${_prefix} REQUIRED IMPORTED_TARGET "${_pkg}")

    list(APPEND _link_targets "PkgConfig::${_prefix}")
    list(APPEND _all_include_dirs ${${_prefix}_INCLUDE_DIRS})
    list(APPEND _all_library_dirs ${${_prefix}_LIBRARY_DIRS})
    list(APPEND _all_libraries ${${_prefix}_LIBRARIES})
  endforeach()

  list(REMOVE_DUPLICATES _all_include_dirs)
  list(REMOVE_DUPLICATES _all_library_dirs)
  list(REMOVE_DUPLICATES _all_libraries)

  if(TARGET "${DOCA_TARGET_NAME}")
    set_property(TARGET "${DOCA_TARGET_NAME}" PROPERTY INTERFACE_LINK_LIBRARIES "${_link_targets}")
  else()
    add_library("${DOCA_TARGET_NAME}" INTERFACE IMPORTED GLOBAL)
    set_property(TARGET "${DOCA_TARGET_NAME}" PROPERTY INTERFACE_LINK_LIBRARIES "${_link_targets}")
  endif()


  set(DOCA_RUNTIME_TARGET "${DOCA_TARGET_NAME}" PARENT_SCOPE)
  set(DOCA_INCLUDE_DIRS "${_all_include_dirs}" PARENT_SCOPE)
  set(DOCA_LIBRARY_DIRS "${_all_library_dirs}" PARENT_SCOPE)
  set(DOCA_LIBRARIES "${_all_libraries}" PARENT_SCOPE)


  list(FIND DOCA_PACKAGES "doca-dpa" _dpa_pkg_index)
  if(NOT _dpa_pkg_index EQUAL -1)
    pkg_get_variable(_doca_dpa_libdir doca-dpa libdir)
    set(DOCA_DPA_LIBDIR "${_doca_dpa_libdir}" PARENT_SCOPE)
  endif()
endfunction()


function(doca_find_packages)
  doca_create_runtime_target(
    TARGET_NAME doca::runtime
    PACKAGES
      doca-common
      doca-dpa
      doca-rdma
      doca-argp
      libflexio
  )
endfunction()


function(doca_apply_to_target target_name)
  set(oneValueArgs RUNTIME_TARGET)
  set(_runtime_target "${DOCA_RUNTIME_TARGET}")
  cmake_parse_arguments(DOCA "" "${oneValueArgs}" "" ${ARGN})

  if(NOT "${DOCA_RUNTIME_TARGET}" STREQUAL "")
    set(_runtime_target "${DOCA_RUNTIME_TARGET}")
  endif()
  if("${_runtime_target}" STREQUAL "")
    set(_runtime_target "doca::runtime")
  endif()

  if(NOT TARGET "${_runtime_target}")
    message(FATAL_ERROR
      "Runtime target '${_runtime_target}' not found. "
      "Call doca_create_runtime_target() or doca_find_packages() first.")
  endif()


  target_link_libraries(${target_name} PRIVATE "${_runtime_target}")
endfunction()
