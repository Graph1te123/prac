cmake_minimum_required(VERSION 3.20)

project(prac_tray_app LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
endif()

set(GENERATED_RPC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RPC_HEADER "${GENERATED_RPC_DIR}/prac_service.h")
set(RPC_CLIENT_SOURCE "${GENERATED_RPC_DIR}/prac_service_c.c")
set(RPC_SERVER_SOURCE "${GENERATED_RPC_DIR}/prac_service_s.c")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIDL_ENVIRONMENT win64)
else()
    set(MIDL_ENVIRONMENT win32)
endif()

find_program(MIDL_EXECUTABLE
    NAMES midl midl.exe
    PATHS
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
        "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x86"
)

if(NOT MIDL_EXECUTABLE)
    get_filename_component(WINDOWS_KITS_ROOT
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
        ABSOLUTE
    )
    file(GLOB WINDOWS_SDK_BIN_DIRS LIST_DIRECTORIES true
        "${WINDOWS_KITS_ROOT}/bin/10.*"
    )
    list(SORT WINDOWS_SDK_BIN_DIRS COMPARE NATURAL ORDER DESCENDING)
    foreach(SDK_BIN_DIR IN LISTS WINDOWS_SDK_BIN_DIRS)
        find_program(MIDL_EXECUTABLE
            NAMES midl midl.exe
            PATHS "${SDK_BIN_DIR}/x64" "${SDK_BIN_DIR}/x86"
            NO_DEFAULT_PATH
        )
        if(MIDL_EXECUTABLE)
            break()
        endif()
    endforeach()
endif()

if(NOT MIDL_EXECUTABLE)
    message(FATAL_ERROR "midl.exe was not found. Install the Windows SDK.")
endif()

add_custom_command(
    OUTPUT "${RPC_HEADER}" "${RPC_CLIENT_SOURCE}" "${RPC_SERVER_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_RPC_DIR}"
    COMMAND "${MIDL_EXECUTABLE}" /nologo /env ${MIDL_ENVIRONMENT} /char signed
            /out "${GENERATED_RPC_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/rpc/prac_service.idl"
    MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/rpc/prac_service.idl"
    VERBATIM
)

add_executable(prac-tray WIN32
    src/main.cpp
    src/rpc_memory.cpp
    "${RPC_CLIENT_SOURCE}"
)

target_compile_features(prac-tray PRIVATE cxx_std_17)
target_include_directories(prac-tray PRIVATE
    src
    "${GENERATED_RPC_DIR}"
)
target_compile_definitions(prac-tray PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(prac-tray PRIVATE
    advapi32
    rpcrt4
    shell32
    user32
)

add_executable(prac-service
    src/service.cpp
    src/rpc_memory.cpp
    "${RPC_SERVER_SOURCE}"
)

target_compile_features(prac-service PRIVATE cxx_std_17)
target_include_directories(prac-service PRIVATE
    src
    "${GENERATED_RPC_DIR}"
)
target_compile_definitions(prac-service PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(prac-service PRIVATE
    advapi32
    rpcrt4
    userenv
    wtsapi32
)

install(TARGETS prac-tray prac-service RUNTIME DESTINATION .)
