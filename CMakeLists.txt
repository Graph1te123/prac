cmake_minimum_required(VERSION 3.20)

project(prac_tray_app LANGUAGES CXX)

add_executable(prac-tray WIN32
    src/main.cpp
)

target_compile_features(prac-tray PRIVATE cxx_std_17)
target_compile_definitions(prac-tray PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(prac-tray PRIVATE
    advapi32
    shell32
    user32
)

install(TARGETS prac-tray RUNTIME DESTINATION .)
