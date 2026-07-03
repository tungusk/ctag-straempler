file(GLOB SRC_FILES *.c)

idf_component_register(SRCS ${SRC_FILES}
        INCLUDE_DIRS "./include"
        REQUIRES vfs fatfs util json drivers machine audio)

# ui_events.h only — a leaf header; moving it to a leaf component is M0c work.
target_include_directories(${COMPONENT_LIB} PRIVATE ../ui/include)
