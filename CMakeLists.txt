cmake_minimum_required(VERSION 3.16)

project(cookieos-customizer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ───────────────────────────────
# ŹRÓDŁA
# ───────────────────────────────
add_executable(cookieos-customizer
    main.c
)

# ───────────────────────────────
# LIBS (X11 + Cairo + system libs)
# ───────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(CAIRO REQUIRED cairo cairo-xlib)

include_directories(
    ${X11_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
)

target_link_libraries(cookieos-customizer
    ${X11_LIBRARIES}
    ${CAIRO_LIBRARIES}
    m
)

# ───────────────────────────────
# INSTALL
# ───────────────────────────────
install(TARGETS cookieos-customizer
    RUNTIME DESTINATION bin
)

# ───────────────────────────────
# CPack (DEB GENERATOR)
# ───────────────────────────────
set(CPACK_GENERATOR "DEB")

set(CPACK_PACKAGE_NAME "cookieos-customizer")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_CONTACT "you@example.com")
set(CPACK_PACKAGE_DESCRIPTION "CookieOS system customization tool")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "CookieOS Team")

# zależności systemowe
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libx11-6, libcairo2, libglib2.0-0"
)

# architektura
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

include(CPack)
