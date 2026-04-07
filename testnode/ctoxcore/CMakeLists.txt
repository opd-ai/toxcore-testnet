cmake_minimum_required(VERSION 3.16)
project(c-testnode C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Find toxcore ────────────────────────────────────────────────────────────
# The build script (scripts/build-ctoxcore.sh) installs c-toxcore into
# ${CMAKE_PREFIX_PATH} before invoking cmake, so pkg-config should find it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TOXCORE REQUIRED toxcore)

# ── Target ──────────────────────────────────────────────────────────────────
add_executable(c-testnode main.c)

target_include_directories(c-testnode PRIVATE ${TOXCORE_INCLUDE_DIRS})
target_link_directories(c-testnode PRIVATE ${TOXCORE_LIBRARY_DIRS})

# Prefer static linking: look for the .a archive first, fall back to the
# pkg-config libraries if the static archive is not present.
find_library(TOXCORE_STATIC_LIB
    NAMES libtoxcore.a
    PATHS ${TOXCORE_LIBRARY_DIRS}
    NO_DEFAULT_PATH
)
if(TOXCORE_STATIC_LIB)
    # Static link: toxcore + its transitive dependencies (libsodium, pthreads)
    target_link_libraries(c-testnode PRIVATE
        ${TOXCORE_STATIC_LIB}
        sodium
        pthread
    )
    message(STATUS "Linking c-testnode statically against ${TOXCORE_STATIC_LIB}")
else()
    target_link_libraries(c-testnode PRIVATE ${TOXCORE_LIBRARIES})
    message(STATUS "Linking c-testnode dynamically against ${TOXCORE_LIBRARIES}")
endif()

target_compile_options(c-testnode PRIVATE ${TOXCORE_CFLAGS_OTHER} -Wall -Wextra)

# Set RPATH — only relevant when dynamically linked, but harmless otherwise.
set_target_properties(c-testnode PROPERTIES
    BUILD_RPATH "${TOXCORE_LIBRARY_DIRS}"
    INSTALL_RPATH "$ORIGIN/lib"
)

# ── Install ─────────────────────────────────────────────────────────────────
install(TARGETS c-testnode RUNTIME DESTINATION bin)
