# PlatformIO extra script: include/ and src/ go first on the include path of yOS's own sources.
# They can't be in build_flags (with custom_sdkconfig those reach ESP-IDF's sources, where "os/os.h"
# and "version.h" are other files), and build_src_flags would put them after the framework's paths,
# behind NimBLE's os/os.h.
Import("projenv")

projenv.Prepend(CPPPATH=["$PROJECT_INCLUDE_DIR", "$PROJECT_SRC_DIR",
                         "$PROJECT_PACKAGES_DIR/framework-arduinoespressif32/libraries/SD/src"])
