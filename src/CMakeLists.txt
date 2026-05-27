# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# --- Build-variant component gating ---------------------------------------
# PlatformIO exports the active environment name as $ENV{PIOENV}. The kivsee
# variant must NOT link orchestra_ble (it pulls in the BT/NimBLE stack, which
# is disabled in sdkconfig.defaults.esp32c3_kivsee). The standalone variant
# keeps it. Components common to both go in COMMON_REQUIRES.
set(COMMON_REQUIRES
    mic_service
    IMU_service
    battery_service
)

if(LILUM_VARIANT STREQUAL "kivsee")
    message(STATUS "Lilum: building KIVSEE variant (WiFi, no BLE)")
    # kivsee animation code lives in lib/kivsee_app (a PlatformIO library), not
    # a component, so nothing extra is REQUIRED here; orchestra_ble is excluded.
    set(VARIANT_REQUIRES "")
else()
    message(STATUS "Lilum: building STANDALONE variant (BLE orchestra)")
    set(VARIANT_REQUIRES orchestra_ble)
endif()

idf_component_register(
  SRCS
    "main.cpp"
  INCLUDE_DIRS
    "."
    "../include"
  REQUIRES
    ${COMMON_REQUIRES}
    ${VARIANT_REQUIRES}
)
