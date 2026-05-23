cmake_minimum_required(VERSION 3.20)

project(prac_tray_app LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
endif()

add_executable(prac-tray WIN32
    src/main.cpp
)

target_compile_features(prac-tray PRIVATE cxx_std_17)

target_include_directories(prac-tray PRIVATE
    src
)

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

add_executable(prac-service
    src/service.cpp
)

target_compile_features(prac-service PRIVATE cxx_std_17)

target_include_directories(prac-service PRIVATE
    src
)

target_compile_definitions(prac-service PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(prac-service PRIVATE
    advapi32
    userenv
    wtsapi32
)

install(TARGETS prac-tray prac-service RUNTIME DESTINATION .)