file(GLOB SRC_FILES *.c)

idf_component_register(SRCS ${SRC_FILES}
        INCLUDE_DIRS "./include"
        REQUIRES vfs fatfs util json drivers machine audio esp_http_server)

target_include_directories(${COMPONENT_LIB} PRIVATE ../ui/include ../menu/include)
