idf_component_register(SRCS "uvc_web_server.c" "uvc_app_task.c"
                       REQUIRES esp_driver_gpio spiffs esp_http_server usb_host_uvc
                       INCLUDE_DIRS ".")

set(STORAGE_WEB_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/webspiffs")
set(STORAGE_IMAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/storage_image")

file(GLOB_RECURSE STORAGE_WEB_ASSETS
     CONFIGURE_DEPENDS
     LIST_DIRECTORIES false
     "${STORAGE_WEB_SOURCE_DIR}/*")

set(STORAGE_EXTERNAL_AUDIO_SOURCES)
set(STORAGE_EXTERNAL_AUDIO_COPY_COMMANDS)
set(STORAGE_EXTERNAL_AUDIO_DESTINATIONS)

# Stage every configured external audio file whose runtime path belongs to this
# SPIFFS mount. Other backends (for example SD card paths) are provisioned by
# their respective storage integration.
if(DEFINED OTOOL_AUDIO_EXTERNAL_FILES)
    foreach(external_config IN LISTS OTOOL_AUDIO_EXTERNAL_FILES)
        string(REPLACE "|" ";" external_fields "${external_config}")
        list(LENGTH external_fields external_field_count)
        if(NOT external_field_count EQUAL 3)
            message(FATAL_ERROR
                "Invalid OTOOL_AUDIO_EXTERNAL_FILES entry '${external_config}'")
        endif()

        list(GET external_fields 1 external_source)
        list(GET external_fields 2 external_runtime_path)
        string(STRIP "${external_source}" external_source)
        string(STRIP "${external_runtime_path}" external_runtime_path)

        if(external_runtime_path MATCHES "^/webspiffs/(.+)$")
            set(storage_relative_path "${CMAKE_MATCH_1}")
            list(FIND STORAGE_EXTERNAL_AUDIO_DESTINATIONS
                 "${storage_relative_path}" destination_index)
            if(NOT destination_index EQUAL -1)
                message(FATAL_ERROR
                    "Multiple external audio files target "
                    "'${external_runtime_path}'")
            endif()

            get_filename_component(storage_relative_dir
                                   "${storage_relative_path}" DIRECTORY)
            set(storage_destination
                "${STORAGE_IMAGE_DIR}/${storage_relative_path}")
            if(storage_relative_dir STREQUAL "")
                set(storage_destination_dir "${STORAGE_IMAGE_DIR}")
            else()
                set(storage_destination_dir
                    "${STORAGE_IMAGE_DIR}/${storage_relative_dir}")
            endif()

            list(APPEND STORAGE_EXTERNAL_AUDIO_DESTINATIONS
                 "${storage_relative_path}")
            list(APPEND STORAGE_EXTERNAL_AUDIO_SOURCES "${external_source}")
            list(APPEND STORAGE_EXTERNAL_AUDIO_COPY_COMMANDS
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                        "${storage_destination_dir}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${external_source}" "${storage_destination}")
        endif()
    endforeach()
endif()

# Build one storage image containing the web assets plus all project-configured
# external audio files assigned to /webspiffs.
add_custom_target(storage_image_files
    COMMAND "${CMAKE_COMMAND}" -E remove_directory "${STORAGE_IMAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${STORAGE_WEB_SOURCE_DIR}" "${STORAGE_IMAGE_DIR}"
    ${STORAGE_EXTERNAL_AUDIO_COPY_COMMANDS}
    DEPENDS ${STORAGE_WEB_ASSETS} ${STORAGE_EXTERNAL_AUDIO_SOURCES}
    COMMENT "Staging web assets and configured external audio files"
    VERBATIM)

spiffs_create_partition_image(storage "${STORAGE_IMAGE_DIR}"
                              FLASH_IN_PROJECT
                              DEPENDS storage_image_files)
