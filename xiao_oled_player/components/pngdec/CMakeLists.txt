# Upstream PNGdec 1.1.6, built through its portable (non-Arduino) API.
file(GLOB pngdec_c_sources "upstream/src/*.c")
idf_component_register(
    SRCS "upstream/src/PNGdec.cpp" ${pngdec_c_sources}
    INCLUDE_DIRS "upstream/src"
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    __MCUXPRESSO
    PNG_MAX_BUFFERED_PIXELS=8194
)
