idf_component_register(SRCS bitstream.c huffman.c l3bitstream.c l3loop.c
                            l3mdct.c l3subband.c layer3.c reservoir.c tables.c
        INCLUDE_DIRS "./include"
        PRIV_INCLUDE_DIRS "."
        REQUIRES heap)

# fixed-point encode loops want speed, not size (project default is -Os);
# -Wno-misleading-indentation: vendored style, not ours to patch
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-misleading-indentation)
