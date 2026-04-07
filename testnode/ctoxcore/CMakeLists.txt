cmake_minimum_required(VERSION 3.16)
project(c-testnode C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Find toxcore ─────────────────────────────────────────────────────────────
# The build script (scripts/build-ctoxcore.sh) installs c-toxcore into
# ${CMAKE_PREFIX_PATH} before invoking cmake, so pkg-config should find it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TOXCORE REQUIRED libtoxcore)

# ── Target ────────────────────────────────────────────────────────────────────
add_executable(c-testnode main.c)

target_include_directories(c-testnode PRIVATE ${TOXCORE_INCLUDE_DIRS})
target_link_libraries(c-testnode PRIVATE ${TOXCORE_LIBRARIES})
target_compile_options(c-testnode PRIVATE ${TOXCORE_CFLAGS_OTHER} -Wall -Wextra)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS c-testnode RUNTIME DESTINATION bin)
