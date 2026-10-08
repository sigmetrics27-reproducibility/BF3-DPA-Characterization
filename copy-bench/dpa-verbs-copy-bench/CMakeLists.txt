cmake_minimum_required(VERSION 3.20)
project(dpa_verbs_copy_bench LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)

set(APP_TARGET_NAME "dpa_verbs_copy_bench")
set(DPA_APP_NAME "dpa_verbs_copy_bench_dpa_app")

set(DOCA_RUNTIME_PACKAGES
  doca-common
  doca-dpa
  doca-verbs
  libflexio
)

set(HOST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dpa_verbs_copy_bench.cpp"
)
set(HOST_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/host"
)

set(DEVICE_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_verbs_copy_bench_dev.c"
)
set(DEVICE_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/dev"
  "${CMAKE_CURRENT_LIST_DIR}/host"
)
set(DEVICE_ATTR_YAML
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_verbs_copy_bench_attributes.yaml"
)

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
  OUT_LIB_TARGET DPA_DEVICE_LIB
)

target_link_libraries(${APP_TARGET_NAME} PRIVATE ${DPA_DEVICE_LIB})
target_link_libraries(${APP_TARGET_NAME} PRIVATE PkgConfig::IBVERBS)
target_compile_options(${APP_TARGET_NAME} PRIVATE
  -Wno-deprecated-declarations
  -Wall
  -Wextra
  -DFLEXIO_ALLOW_EXPERIMENTAL_API
)
