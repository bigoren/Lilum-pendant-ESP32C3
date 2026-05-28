idf_component_register(
    SRCS
        "button_service.cpp"
    INCLUDE_DIRS
        "include"
        "../../include"
        "../../lib/led_engine"
        "../../lib/app_mode"  # only #include'd when LILUM_KIVSEE is defined
)
