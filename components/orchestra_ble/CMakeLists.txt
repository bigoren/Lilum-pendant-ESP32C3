# PlatformIO/IDF auto-discovers and builds every directory under components/,
# regardless of who REQUIRES it. So to keep this component out of the kivsee
# build we must gate its OWN registration: in the kivsee variant register an
# empty component (no sources, no `bt` dependency) so the NimBLE headers are
# never pulled in. Gate on the LILUM_VARIANT CMake variable (passed via
# board_build.cmake_extra_args) — $ENV{PIOENV} and CONFIG_BT_ENABLED both proved
# unreliable at component-registration time; an explicit -D CMake var is not.
if(LILUM_VARIANT STREQUAL "kivsee")
    # Kivsee variant: BLE disabled — compile nothing.
    idf_component_register()
else()
    idf_component_register(
        SRCS
            "simple_BLE.cpp"
            "orchestra_payload.cpp"
        INCLUDE_DIRS
            "include"
            "../../include"
            "../../lib/led_engine"
        REQUIRES
            bt
    )
endif()
