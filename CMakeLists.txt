cmake_minimum_required(VERSION 3.27)

project(hypr-oled-saver
    DESCRIPTION "An OLED-friendly Hyprland screensaver plugin"
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    cairo
    libdrm
    pixman-1
    wayland-server
    xkbcommon
)

add_library(hypr-oled-saver SHARED
    src/main.cpp
)

target_link_libraries(hypr-oled-saver PRIVATE rt PkgConfig::deps)

install(TARGETS hypr-oled-saver)
