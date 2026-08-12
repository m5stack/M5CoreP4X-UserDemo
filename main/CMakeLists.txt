idf_component_register(SRCS "main.cpp" "tools.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES m5_corep4_component m5_py32bin_tools esp_timer esp_driver_usb_serial_jtag
                    )

# Firmware metadata generated at build time.
# Format: V${APP_FW_VERSION_TEXT}.YYMMDD-HHMMSS
set(APP_FW_NAME "M5CoreP4X-UserDemo" CACHE STRING "Firmware name")
set(APP_FW_VERSION_TEXT "1" CACHE STRING "Firmware version text used after the leading V")
set(APP_FW_COMMIT "" CACHE STRING "Firmware source commit (up to 8 characters); empty uses Git")
set(APP_FW_POWERED_BY "Powered by onexs." CACHE STRING "Firmware author/powered-by text")
set(APP_FW_COMPANY "M5Stack Technology Co., Ltd" CACHE STRING "Firmware company/support text")
set(APP_FW_VERSION_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APP_FW_VERSION_HEADER "${APP_FW_VERSION_GENERATED_DIR}/app_fw_version.h")
set(APP_FW_VERSION_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generate_app_fw_version.cmake")

set(APP_FW_COMMIT_RESOLVED "${APP_FW_COMMIT}")
if(APP_FW_COMMIT_RESOLVED STREQUAL "")
    execute_process(
        COMMAND git rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE APP_FW_GIT_RESULT
        OUTPUT_VARIABLE APP_FW_COMMIT_RESOLVED
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT APP_FW_GIT_RESULT EQUAL 0 OR APP_FW_COMMIT_RESOLVED STREQUAL "")
        set(APP_FW_COMMIT_RESOLVED "unknown")
        message(WARNING "APP_FW_COMMIT could not be resolved; using non-empty fallback 'unknown'")
    endif()
endif()
string(LENGTH "${APP_FW_COMMIT_RESOLVED}" APP_FW_COMMIT_LENGTH)
if(APP_FW_COMMIT_LENGTH GREATER 8)
    string(SUBSTRING "${APP_FW_COMMIT_RESOLVED}" 0 8 APP_FW_COMMIT_RESOLVED)
endif()
message(STATUS "Firmware source commit: ${APP_FW_COMMIT_RESOLVED}")

file(MAKE_DIRECTORY "${APP_FW_VERSION_GENERATED_DIR}")
file(WRITE "${APP_FW_VERSION_SCRIPT}" [=[
if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()
if(NOT DEFINED APP_FW_NAME)
    set(APP_FW_NAME "M5CoreP4X-UserDemo")
endif()
if(NOT DEFINED APP_FW_VERSION_TEXT)
    set(APP_FW_VERSION_TEXT "1")
endif()
if(NOT DEFINED APP_FW_COMMIT OR APP_FW_COMMIT STREQUAL "")
    set(APP_FW_COMMIT "unknown")
endif()
if(NOT DEFINED APP_FW_POWERED_BY)
    set(APP_FW_POWERED_BY "Powered by onexs.")
endif()
if(NOT DEFINED APP_FW_COMPANY)
    set(APP_FW_COMPANY "M5Stack Technology Co., Ltd")
endif()

string(TIMESTAMP APP_FW_BUILD_DATE "%y%m%d")
string(TIMESTAMP APP_FW_BUILD_TIME "%H%M%S")
set(APP_FW_VERSION "V${APP_FW_VERSION_TEXT}.${APP_FW_BUILD_DATE}-${APP_FW_BUILD_TIME}")
set(APP_FW_SUPPORT_TEXT "Supported by ${APP_FW_COMPANY}")

file(WRITE "${OUTPUT_FILE}" "#pragma once\n\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_NAME \"${APP_FW_NAME}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_VERSION_TEXT \"${APP_FW_VERSION_TEXT}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_BUILD_DATE \"${APP_FW_BUILD_DATE}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_BUILD_TIME \"${APP_FW_BUILD_TIME}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_VERSION \"${APP_FW_VERSION}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_COMMIT \"${APP_FW_COMMIT}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_POWERED_BY \"${APP_FW_POWERED_BY}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_COMPANY \"${APP_FW_COMPANY}\"\n")
file(APPEND "${OUTPUT_FILE}" "#define APP_FW_SUPPORT_TEXT \"${APP_FW_SUPPORT_TEXT}\"\n")
message(STATUS "Generated firmware version: ${APP_FW_VERSION}")
]=])

add_custom_target(main_app_fw_version_header ALL
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT_FILE=${APP_FW_VERSION_HEADER}"
        "-DAPP_FW_NAME:STRING=${APP_FW_NAME}"
        "-DAPP_FW_VERSION_TEXT:STRING=${APP_FW_VERSION_TEXT}"
        "-DAPP_FW_COMMIT:STRING=${APP_FW_COMMIT_RESOLVED}"
        "-DAPP_FW_POWERED_BY:STRING=${APP_FW_POWERED_BY}"
        "-DAPP_FW_COMPANY:STRING=${APP_FW_COMPANY}"
        -P "${APP_FW_VERSION_SCRIPT}"
    BYPRODUCTS "${APP_FW_VERSION_HEADER}"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} main_app_fw_version_header)
target_include_directories(${COMPONENT_LIB} PRIVATE "${APP_FW_VERSION_GENERATED_DIR}")

idf_component_get_property(M5_COREP4_COMPONENT_LIB m5_corep4_component COMPONENT_LIB)
if(M5_COREP4_COMPONENT_LIB)
    add_dependencies(${M5_COREP4_COMPONENT_LIB} main_app_fw_version_header)
    target_include_directories(${M5_COREP4_COMPONENT_LIB} PRIVATE "${APP_FW_VERSION_GENERATED_DIR}")
endif()
