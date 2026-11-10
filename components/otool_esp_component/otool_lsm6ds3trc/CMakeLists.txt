# SPDX-FileCopyrightText: 2026 M5Stack Technology Co., Ltd
# SPDX-License-Identifier: MIT

if(NOT ENABLE_OTOOL_LSM6DS3TRC)
    return()
endif()

list(APPEND COMPILE_DEFS OTOOL_ENABLE_LSM6DS3TRC)

list(APPEND TOOLBOX_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/lsm6ds3trc_tools.cpp"
)

list(APPEND TOOLBOX_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

list(APPEND TOOLBOX_REQUIRES
    esp_timer
)
