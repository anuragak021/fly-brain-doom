# doom-esp32 build glue (not an engine patch): replaces retro-go's CMakeLists, which used the
# legacy register_component() and retro-go's rg_setup_compile_options(). Same flags as upstream,
# plus DOOM_ESP32 (enables the marked patches in d_main.c, r_things.c and r_main.c). See NOTES.md, "Phase 2".
file(GLOB PRBOOM_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")
idf_component_register(SRCS ${PRBOOM_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES doom_platform)
# Third party code is left as is, so its warnings must not fail the build (IDF adds
# -Werror=all to every component). Our own components keep -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-error=address
    -Wno-misleading-indentation
    -Wno-format-overflow
    -Wno-char-subscripts
    -Wno-missing-field-initializers
    -DHAVE_CONFIG_H
    -DDOOM_ESP32
    -O2)
