set(SRC_FILES main.c machine_registry.c machine_stub.c)

idf_component_register(SRCS ${SRC_FILES}
        INCLUDE_DIRS "."
        REQUIRES ui machine audio machine_sampler2 machine_sampler3 machine_looper machine_slicer machine_granular machine_glitch machine_drumsampler machine_freesound machine_deck machine_dualdeck machine_tracker machine_radio machine_synth machine_instsampler machine_tape machine_editor menu)
