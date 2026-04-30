cmake_minimum_required(VERSION 3.27)

project(hypr-oled-saver
    DESCRIPTION "An OLED-friendly Hyprland layer-shell screensaver"
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    gtk+-3.0
    gtk-layer-shell-0
    pangocairo
)

add_executable(hypr-oled-saver
    src/main.cpp
)

target_link_libraries(hypr-oled-saver PRIVATE PkgConfig::deps)

install(TARGETS hypr-oled-saver RUNTIME DESTINATION bin)
