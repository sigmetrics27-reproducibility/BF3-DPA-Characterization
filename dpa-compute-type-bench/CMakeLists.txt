cmake_minimum_required(VERSION 3.20)
project(dpa_compute_type_bench LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_TARGET_NAME "dpa_compute_type_bench")
set(DPA_APP_NAME "dpa_compute_type_bench_dpa_app")
set(TPUT_SAMECORE_TARGET_NAME "dpa_compute_tput_samecore_bench")
set(TPUT_SAMECORE_DPA_APP_NAME "dpa_compute_tput_samecore_dpa_app")

set(DOCA_RUNTIME_PACKAGES
  doca-common
  doca-dpa
  libflexio
)

set(HOST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dpa_compute_type_bench.cpp"
)
set(HOST_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/host"
)

set(DEVICE_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_bench_dev.c"
)
set(DEVICE_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/dev"
  "${CMAKE_CURRENT_LIST_DIR}/host"
)
set(DEVICE_ATTR_YAML
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_type_bench_attributes.yaml"
)

set(DPA_ASM_SRC "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_bench_asm.S")
set(DPA_ASM_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpabenchasm.a")
set(DPA_ASM_HOST_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpabenchasm_host.a")
add_custom_command(
  OUTPUT "${DPA_ASM_LIB}" "${DPA_ASM_HOST_LIB}"
  COMMAND "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
          "${DPA_ASM_SRC}" "${DPA_ASM_LIB}" dpabenchasm
  DEPENDS "${DPA_ASM_SRC}"
          "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
  COMMENT "building dpabenchasm"
  VERBATIM
)
add_custom_target(dpa_compute_asm_lib DEPENDS "${DPA_ASM_LIB}" "${DPA_ASM_HOST_LIB}")

# keep the soft-float loops in their own library
set(DPA_FP_ASM_SRC "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_bench_fp_asm.S")
set(DPA_FP_ASM_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpabenchfp.a")
set(DPA_FP_ASM_HOST_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpabenchfp_host.a")
add_custom_command(
  OUTPUT "${DPA_FP_ASM_LIB}" "${DPA_FP_ASM_HOST_LIB}"
  COMMAND "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
          "${DPA_FP_ASM_SRC}" "${DPA_FP_ASM_LIB}" dpabenchfp
  DEPENDS "${DPA_FP_ASM_SRC}"
          "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
  COMMENT "building dpabenchfp"
  VERBATIM
)
add_custom_target(dpa_compute_fp_asm_lib DEPENDS "${DPA_FP_ASM_LIB}" "${DPA_FP_ASM_HOST_LIB}")

set(DPA_SAMECORE_ASM_SRC "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_tput_samecore_asm.S")
set(DPA_SAMECORE_ASM_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpasamecoreasm.a")
set(DPA_SAMECORE_ASM_HOST_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpasamecoreasm_host.a")
add_custom_command(
  OUTPUT "${DPA_SAMECORE_ASM_LIB}" "${DPA_SAMECORE_ASM_HOST_LIB}"
  COMMAND "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
          "${DPA_SAMECORE_ASM_SRC}" "${DPA_SAMECORE_ASM_LIB}" dpasamecoreasm
  DEPENDS "${DPA_SAMECORE_ASM_SRC}"
          "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
  COMMENT "building dpasamecoreasm"
  VERBATIM
)
add_custom_target(dpa_compute_samecore_asm_lib
  DEPENDS "${DPA_SAMECORE_ASM_LIB}" "${DPA_SAMECORE_ASM_HOST_LIB}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(docaDeps)
include(dpaBuild)

doca_create_runtime_target(
  TARGET_NAME doca::runtime
  PACKAGES ${DOCA_RUNTIME_PACKAGES}
)

add_executable(${APP_TARGET_NAME} ${HOST_SOURCES})
target_include_directories(${APP_TARGET_NAME} PRIVATE ${HOST_INCLUDE_DIRS})
doca_apply_to_target(${APP_TARGET_NAME} RUNTIME_TARGET doca::runtime)
target_compile_definitions(${APP_TARGET_NAME} PRIVATE DPA_APP_NAME=${DPA_APP_NAME})

dpa_add_device_lib(
  APP_NAME "${DPA_APP_NAME}"
  DOCA_DPA_LIBDIR "${DOCA_DPA_LIBDIR}"
  DEVICE_SOURCES ${DEVICE_SOURCES}
  DEVICE_INCLUDE_DIRS ${DEVICE_INCLUDE_DIRS}
  DEVICE_ATTR_YAML "${DEVICE_ATTR_YAML}"
  DEVICE_LIBS
    "-L${CMAKE_CURRENT_BINARY_DIR}" "-ldpabenchasm" "-ldpabenchfp"
    "-L${DOCA_DPA_LIBDIR}" "-ldoca_dpa_dev" "-ldoca_dpa_dev_comm" "-ldoca_dpa_dev_verbs"
  DEPENDS
    dpa_compute_asm_lib
    dpa_compute_fp_asm_lib
    "${DPA_ASM_LIB}"
    "${DPA_FP_ASM_LIB}"
    "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_bench_dev.c"
    "${CMAKE_CURRENT_LIST_DIR}/host/dpa_compute_type_common.h"
  DEVICE_CC_FLAGS
    -Wno-deprecated-declarations
    -Werror
    -Wall
    -Wextra
    -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API
    -O2
  OUT_LIB_TARGET DPA_DEVICE_LIB
)

target_link_libraries(${APP_TARGET_NAME} PRIVATE
  ${DPA_DEVICE_LIB}
  "${DPA_ASM_HOST_LIB}"
  "${DPA_FP_ASM_HOST_LIB}"
)
add_dependencies(${APP_TARGET_NAME} dpa_compute_asm_lib dpa_compute_fp_asm_lib)
target_compile_options(${APP_TARGET_NAME} PRIVATE
  -Wno-deprecated-declarations
  -Wall
  -Wextra
  -DFLEXIO_ALLOW_EXPERIMENTAL_API
)

add_executable(${TPUT_SAMECORE_TARGET_NAME}
  "${CMAKE_CURRENT_LIST_DIR}/dpa_compute_tput_samecore_bench.cpp"
)
target_include_directories(${TPUT_SAMECORE_TARGET_NAME} PRIVATE ${HOST_INCLUDE_DIRS})
doca_apply_to_target(${TPUT_SAMECORE_TARGET_NAME} RUNTIME_TARGET doca::runtime)
target_compile_definitions(${TPUT_SAMECORE_TARGET_NAME} PRIVATE DPA_APP_NAME=${TPUT_SAMECORE_DPA_APP_NAME})

dpa_add_device_lib(
  APP_NAME "${TPUT_SAMECORE_DPA_APP_NAME}"
  DOCA_DPA_LIBDIR "${DOCA_DPA_LIBDIR}"
  DEVICE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_tput_samecore_dev.c"
  DEVICE_INCLUDE_DIRS ${DEVICE_INCLUDE_DIRS}
  DEVICE_ATTR_YAML "${DEVICE_ATTR_YAML}"
  DEVICE_LIBS
    "-L${CMAKE_CURRENT_BINARY_DIR}" "-ldpasamecoreasm"
    "-L${DOCA_DPA_LIBDIR}" "-ldoca_dpa_dev" "-ldoca_dpa_dev_comm" "-ldoca_dpa_dev_verbs"
  DEPENDS
    dpa_compute_samecore_asm_lib
    "${DPA_SAMECORE_ASM_LIB}"
    "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_compute_tput_samecore_dev.c"
    "${CMAKE_CURRENT_LIST_DIR}/host/dpa_compute_tput_samecore_common.h"
  DEVICE_CC_FLAGS
    -Wno-deprecated-declarations
    -Werror
    -Wall
    -Wextra
    -DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API
    -O2
  OUT_LIB_TARGET TPUT_SAMECORE_DPA_DEVICE_LIB
)

target_link_libraries(${TPUT_SAMECORE_TARGET_NAME} PRIVATE
  ${TPUT_SAMECORE_DPA_DEVICE_LIB}
  "${DPA_SAMECORE_ASM_HOST_LIB}"
)
add_dependencies(${TPUT_SAMECORE_TARGET_NAME} dpa_compute_samecore_asm_lib)
target_compile_options(${TPUT_SAMECORE_TARGET_NAME} PRIVATE
  -Wno-deprecated-declarations
  -Wall
  -Wextra
  -DFLEXIO_ALLOW_EXPERIMENTAL_API
)

add_executable(dpa_compute_tput_multicore_bench
  "${CMAKE_CURRENT_LIST_DIR}/dpa_compute_tput_multicore_bench.cpp"
)
target_include_directories(dpa_compute_tput_multicore_bench PRIVATE ${HOST_INCLUDE_DIRS})
doca_apply_to_target(dpa_compute_tput_multicore_bench RUNTIME_TARGET doca::runtime)
target_compile_definitions(dpa_compute_tput_multicore_bench PRIVATE
  DPA_APP_NAME=${TPUT_SAMECORE_DPA_APP_NAME})
target_link_libraries(dpa_compute_tput_multicore_bench PRIVATE
  ${TPUT_SAMECORE_DPA_DEVICE_LIB}
  "${DPA_SAMECORE_ASM_HOST_LIB}"
)
add_dependencies(dpa_compute_tput_multicore_bench dpa_compute_samecore_asm_lib)
target_compile_options(dpa_compute_tput_multicore_bench PRIVATE
  -Wno-deprecated-declarations
  -Wall
  -Wextra
  -DFLEXIO_ALLOW_EXPERIMENTAL_API
)
