cmake_minimum_required(VERSION 3.20)
project(dpa_asm_demo LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_TARGET_NAME "dpa_asm_demo")
set(DPA_APP_NAME "dpa_asm_demo_dpa_app")

set(DOCA_RUNTIME_PACKAGES
  doca-common
  doca-dpa
  libflexio
)

set(HOST_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dpa_asm_demo.cpp"
)
set(HOST_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/host"
)

set(DEVICE_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_asm_demo_dev.c"
)
set(DEVICE_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/dev"
  "${CMAKE_CURRENT_LIST_DIR}/host"
)
set(DEVICE_ATTR_YAML
  "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_asm_demo_attributes.yaml"
)

# build the asm separately, then link the device library and host stub
set(DPA_ASM_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpaasm.a")
set(DPA_ASM_HOST_LIB "${CMAKE_CURRENT_BINARY_DIR}/libdpaasm_host.a")
add_custom_command(
  OUTPUT "${DPA_ASM_LIB}" "${DPA_ASM_HOST_LIB}"
  COMMAND "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
          "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_asm_funcs.c"
          "${DPA_ASM_LIB}" dpaasm
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/dev/dpa_asm_funcs.c"
          "${CMAKE_CURRENT_LIST_DIR}/tools/build_dpa_asm_lib.sh"
  COMMENT "building dpaasm"
  VERBATIM
)
add_custom_target(dpa_asm_lib DEPENDS "${DPA_ASM_LIB}" "${DPA_ASM_HOST_LIB}")

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
    "-L${CMAKE_CURRENT_BINARY_DIR}" "-ldpaasm"
    "-L${DOCA_DPA_LIBDIR}" "-ldoca_dpa_dev" "-ldoca_dpa_dev_comm" "-ldoca_dpa_dev_verbs"
  DEPENDS dpa_asm_lib
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
)
add_dependencies(${APP_TARGET_NAME} dpa_asm_lib)
target_compile_options(${APP_TARGET_NAME} PRIVATE
  -Wno-deprecated-declarations
  -Wall
  -Wextra
  -DFLEXIO_ALLOW_EXPERIMENTAL_API
)
