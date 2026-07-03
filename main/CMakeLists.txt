set(SRC_FILES main.c machine_registry.c)

idf_component_register(SRCS ${SRC_FILES}
        INCLUDE_DIRS "."
        REQUIRES ui machine audio machine_sampler menu)
