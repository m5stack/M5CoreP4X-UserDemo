SET(SOURCES screens/ui_startup.c
    screens/ui_mian.c
    screens/ui_touchtest.c
    screens/ui_info.c
    screens/ui_function.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_corep4x_ui_startup_png.c
    images/ui_img_corep4x_ui_info_png.c
    images/ui_img_corep4x_ui_function_png.c)

add_library(ui ${SOURCES})
