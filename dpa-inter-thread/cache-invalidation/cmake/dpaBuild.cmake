include_guard(GLOBAL)
function(dpa_build target app)
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(DOCA REQUIRED IMPORTED_TARGET doca-common doca-dpa libflexio)
  pkg_get_variable(dpa_libdir doca-dpa libdir)
  find_program(DPACC dpacc HINTS "${DOCA_ROOT}/tools" REQUIRED)
  find_program(ATTR_TOOL dpa-app-attributes2blob HINTS "${DOCA_ROOT}/tools" REQUIRED)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/device")
  set(archive "${out}/${app}.a")
  set(blob "${out}/attributes.blob")
  file(GLOB device_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/dev/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/host/*.h")
  add_custom_command(OUTPUT "${archive}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND "${ATTR_TOOL}" "${CMAKE_CURRENT_SOURCE_DIR}/dev/attributes.yaml" "${blob}"
    COMMAND "${DPACC}" "${CMAKE_CURRENT_SOURCE_DIR}/dev/bench.c"
      -o "${archive}" -mcpu=nv-dpa-bf3 -hostcc=${CMAKE_C_COMPILER}
      "-hostcc-options=-O2 -fno-lto -Wall -Wextra -Wno-deprecated-declarations -DFLEXIO_ALLOW_EXPERIMENTAL_API"
      "--devicecc-options=-std=c11 -O2 -fno-lto -Wall -Wextra -Werror -Wno-deprecated-declarations -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API"
      "--app-name=${app}" "--dpa-proc-attr=${blob}"
      "-I${DOCA_ROOT}/include" "-I${CMAKE_CURRENT_SOURCE_DIR}/host"
      "-device-libs=-L${dpa_libdir} -ldoca_dpa_dev -ldoca_dpa_dev_comm"
      --keep "--keep-dir=${out}"
    DEPENDS dev/bench.c dev/attributes.yaml ${device_headers}
    VERBATIM)
  add_custom_target(${app}_device DEPENDS "${archive}")
  add_library(${app}_archive STATIC IMPORTED)
  set_target_properties(${app}_archive PROPERTIES IMPORTED_LOCATION "${archive}")
  add_dependencies(${app}_archive ${app}_device)
  target_link_libraries(${target} PRIVATE ${app}_archive PkgConfig::DOCA Threads::Threads)
endfunction()
