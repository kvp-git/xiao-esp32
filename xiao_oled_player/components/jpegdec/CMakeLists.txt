# Pinned upstream JPEGDEC 1.8.4; ESP_PLATFORM selects its portable API.
idf_component_register(
    SRCS "upstream/src/JPEGDEC.cpp"
    INCLUDE_DIRS "upstream/src"
)
