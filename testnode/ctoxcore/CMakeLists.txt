cmake_minimum_required(VERSION 3.16)
project(c-testnode C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Find toxcore ─────────────────────────────────────────────────────────────
# The build script (scripts/build-ctoxcore.sh) installs c-toxcore into
# ${CMAKE_PREFIX_PATH} before invoking cmake, so pkg-config should find it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TOXCORE REQUIRED toxcore)

# ── Target ────────────────────────────────────────────────────────────────────
add_executable(c-testnode main.c)

target_include_directories(c-testnode PRIVATE ${TOXCORE_INCLUDE_DIRS})
# Use TOXCORE_LINK_LIBRARIES for proper library path handling
target_link_directories(c-testnode PRIVATE ${TOXCORE_LIBRARY_DIRS})
target_link_libraries(c-testnode PRIVATE ${TOXCORE_LIBRARIES})
target_compile_options(c-testnode PRIVATE ${TOXCORE_CFLAGS_OTHER} -Wall -Wextra)

# Set RPATH so the binary can find libtoxcore at runtime.
# BUILD_RPATH uses the absolute path so the binary works in the build tree.
# INSTALL_RPATH uses $ORIGIN/lib so the installed binary looks for shared
# libraries in a lib/ directory next to itself — this makes the artifact
# self-contained and portable across CI jobs.
set_target_properties(c-testnode PROPERTIES
    BUILD_RPATH "${TOXCORE_LIBRARY_DIRS}"
    INSTALL_RPATH "$ORIGIN/lib"
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS c-testnode RUNTIME DESTINATION bin)
