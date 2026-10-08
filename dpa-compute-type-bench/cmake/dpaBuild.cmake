include_guard(GLOBAL)


if(DEFINED DOCA_ROOT AND NOT "${DOCA_ROOT}" STREQUAL "")
  set(_dpa_default_doca_root "${DOCA_ROOT}")
else()
  set(_dpa_default_doca_root "/opt/mellanox/doca")
endif()
set(DPA_DOCA_ROOT "${_dpa_default_doca_root}" CACHE PATH "Root path of NVIDIA DOCA installation")
unset(_dpa_default_doca_root)

# asm loops are built separately, leave LTO off for the wrappers
option(DPA_ENABLE_LTO "Enable -flto for the dpacc device compile" OFF)


function(dpa_add_device_lib)
  set(oneValueArgs
    APP_NAME
    DOCA_DPA_LIBDIR
    DEVICE_ATTR_YAML
    DPACC_MCPU_FLAG
    OUT_LIB_TARGET
    OUTPUT_LIB_NAME
    OUTPUT_DIR
    DPACC_EXECUTABLE
    ATTR2BLOB_EXECUTABLE
    HOST_COMPILER
  )
  set(multiValueArgs
    DEVICE_SOURCES
    DEVICE_INCLUDE_DIRS
    HOST_CC_FLAGS
    DEVICE_CC_FLAGS
    DEVICE_LIBS
    DEPENDS
  )
  cmake_parse_arguments(DPA "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})


  if("${DPA_APP_NAME}" STREQUAL "")
    message(FATAL_ERROR "APP_NAME is required")
  endif()
  if(NOT "${DPA_APP_NAME}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR
      "APP_NAME must be a valid C identifier for dpacc-generated symbols: ${DPA_APP_NAME}")
  endif()
  if("${DPA_DEVICE_SOURCES}" STREQUAL "")
    message(FATAL_ERROR "DEVICE_SOURCES is required")
  endif()


  if("${DPA_DOCA_DPA_LIBDIR}" STREQUAL "")
    if(NOT "${DOCA_DPA_LIBDIR}" STREQUAL "")
      set(DPA_DOCA_DPA_LIBDIR "${DOCA_DPA_LIBDIR}")
    else()
      message(FATAL_ERROR "DOCA_DPA_LIBDIR is required (or call doca_find_packages() first)")
    endif()
  endif()

  if(NOT DEFINED DPA_DPACC_MCPU_FLAG OR "${DPA_DPACC_MCPU_FLAG}" STREQUAL "")
    set(DPA_DPACC_MCPU_FLAG "nv-dpa-bf3,nv-dpa-cx7,nv-dpa-cx8,nv-dpa-cx9")
  endif()

  if(NOT DEFINED DPA_HOST_COMPILER OR "${DPA_HOST_COMPILER}" STREQUAL "")
    set(DPA_HOST_COMPILER "gcc")
  endif()

  if(DPA_HOST_CC_FLAGS)
    string(JOIN " " _host_cc_flags ${DPA_HOST_CC_FLAGS})
  else()
    set(_host_cc_flags "-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_ALLOW_EXPERIMENTAL_API")
  endif()

  if(DPA_DEVICE_CC_FLAGS)
    string(JOIN " " _device_cc_flags ${DPA_DEVICE_CC_FLAGS})
  else()
    set(_device_cc_flags "-Wno-deprecated-declarations -Werror -Wall -Wextra -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API -O2")
  endif()

  if("${DPA_OUTPUT_LIB_NAME}" STREQUAL "")
    set(DPA_OUTPUT_LIB_NAME "${DPA_APP_NAME}_program")
  endif()

  if("${DPA_OUTPUT_DIR}" STREQUAL "")
    set(DPA_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${DPA_APP_NAME}/device/build_dpacc")
  endif()


  foreach(_src IN LISTS DPA_DEVICE_SOURCES)
    if(NOT EXISTS "${_src}")
      message(FATAL_ERROR "Missing DPA device source: ${_src}")
    endif()
  endforeach()

  set(_device_include_dirs ${DPA_DEVICE_INCLUDE_DIRS})
  list(APPEND _device_include_dirs "${DPA_DOCA_ROOT}/include")
  list(REMOVE_DUPLICATES _device_include_dirs)

  foreach(_inc IN LISTS _device_include_dirs)
    if(NOT EXISTS "${_inc}")
      message(FATAL_ERROR "Missing DPA include directory: ${_inc}")
    endif()
  endforeach()

  if(DPA_DPACC_EXECUTABLE)
    set(_dpacc "${DPA_DPACC_EXECUTABLE}")
  else()

    find_program(_dpacc NAMES dpacc HINTS "${DPA_DOCA_ROOT}/tools" REQUIRED)
  endif()

  if(DPA_ATTR2BLOB_EXECUTABLE)
    set(_attr2blob "${DPA_ATTR2BLOB_EXECUTABLE}")
  else()
    find_program(_attr2blob NAMES dpa-app-attributes2blob HINTS "${DPA_DOCA_ROOT}/tools")
  endif()

  set(_program_name "${DPA_OUTPUT_LIB_NAME}")
  set(_program_lib "${DPA_OUTPUT_DIR}/${_program_name}.a")

  set(_dpacc_include_args)
  foreach(_inc IN LISTS _device_include_dirs)
    list(APPEND _dpacc_include_args "-I${_inc}")
  endforeach()

  if(DPA_DEVICE_LIBS)
    string(JOIN " " _device_libs ${DPA_DEVICE_LIBS})
  else()

    set(_device_libs "-L${DPA_DOCA_DPA_LIBDIR} -ldoca_dpa_dev -ldoca_dpa_dev_comm -ldoca_dpa_dev_verbs")
  endif()

  set(_dpacc_optional_args)
  set(_dpacc_extra_depends)

  if(DPA_ENABLE_LTO)
    set(_dpacc_lto_flag "-flto")
  else()
    set(_dpacc_lto_flag "")
  endif()

  if(NOT "${DPA_DEVICE_ATTR_YAML}" STREQUAL "")
    if(NOT EXISTS "${DPA_DEVICE_ATTR_YAML}")
      message(FATAL_ERROR "Missing DPA attributes YAML: ${DPA_DEVICE_ATTR_YAML}")
    endif()
    if("${_attr2blob}" STREQUAL "_attr2blob-NOTFOUND")
      message(FATAL_ERROR "dpa-app-attributes2blob not found. Set ATTR2BLOB_EXECUTABLE or install DOCA tools")
    endif()

    set(_attributes_blob "${DPA_OUTPUT_DIR}/${DPA_APP_NAME}_attributes.blob")

    add_custom_command(
      OUTPUT "${_attributes_blob}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${DPA_OUTPUT_DIR}"
      COMMAND "${_attr2blob}" "${DPA_DEVICE_ATTR_YAML}" "${_attributes_blob}"
      DEPENDS "${DPA_DEVICE_ATTR_YAML}"
      COMMENT "generating attributes for ${DPA_APP_NAME}"
      VERBATIM
    )

    list(APPEND _dpacc_optional_args "--dpa-proc-attr=${_attributes_blob}")
    list(APPEND _dpacc_extra_depends "${_attributes_blob}")
  endif()


  add_custom_command(
    OUTPUT "${_program_lib}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DPA_OUTPUT_DIR}"
    COMMAND "${_dpacc}"
            ${DPA_DEVICE_SOURCES}
            -o "${_program_lib}"
            "-mcpu=${DPA_DPACC_MCPU_FLAG}"
            "-hostcc=${DPA_HOST_COMPILER}"
            "-hostcc-options=${_host_cc_flags}"
            "--devicecc-options=${_device_cc_flags}"
            "--app-name=${DPA_APP_NAME}"
            "-device-libs=${_device_libs}"
            ${_dpacc_lto_flag}
            ${_dpacc_include_args}
            ${_dpacc_optional_args}
    DEPENDS ${DPA_DEVICE_SOURCES} ${_dpacc_extra_depends} ${DPA_DEPENDS}
    COMMENT "building ${DPA_APP_NAME} with dpacc"
    VERBATIM
  )

  set(_program_target "dpa_program_lib_${DPA_APP_NAME}")
  add_custom_target(${_program_target} DEPENDS "${_program_lib}")


  add_library(${_program_name} STATIC IMPORTED GLOBAL)
  set_target_properties(${_program_name} PROPERTIES IMPORTED_LOCATION "${_program_lib}")
  add_dependencies(${_program_name} ${_program_target})

  if(NOT "${DPA_OUT_LIB_TARGET}" STREQUAL "")
    set(${DPA_OUT_LIB_TARGET} "${_program_name}" PARENT_SCOPE)
  endif()
endfunction()
