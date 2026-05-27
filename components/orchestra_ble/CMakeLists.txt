# PlatformIO/IDF auto-discovers and builds every directory under components/,
# regardless of who REQUIRES it. So to keep this component out of the kivsee
# (BLE-disabled) build we must gate its OWN registration: when CONFIG_BT_ENABLED
# is off, register an empty component (no sources, no `bt` dependency) so the
# NimBLE headers are never pulled in.
if(CONFIG_BT_ENABLED)
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
else()
    # BLE disabled (kivsee variant): compile nothing.
    idf_component_register()
endif()
