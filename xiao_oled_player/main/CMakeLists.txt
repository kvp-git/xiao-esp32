idf_component_register(
    SRCS "main.c" "display_ssd1351.c" "player_task.c" "usb_cli.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer pngdec jpegdec
)

# PNGdec and JPEGDEC expose C++ APIs. Set this after registration, outside IDF's
# script-mode dependency scan.
set_source_files_properties(player_task.c PROPERTIES LANGUAGE CXX)
