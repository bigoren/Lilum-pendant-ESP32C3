idf_component_register(
    SRCS
        "button_service.cpp"
    INCLUDE_DIRS
        "include"
        "../../include"
        "../../lib/led_engine"
        # app_mode.h is consumed only when LILUM_KIVSEE is defined (kivsee
        # env only). The header path is harmless to expose to the standalone
        # env's compile too — its button_service.cpp never #includes it.
        "../../lib/app_mode"
)
