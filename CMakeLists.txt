cmake_minimum_required(VERSION 3.20)

project(codex_cpp VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

option(CODEX_CPP_BUILD_EXE "Build the codex-cpp executable" ON)
option(CODEX_CPP_BUILD_STATIC_LIB "Build the codex-cpp static library" OFF)
option(CODEX_CPP_BUILD_SHARED_LIB "Build the codex-cpp shared library" OFF)

if(NOT CODEX_CPP_BUILD_EXE AND NOT CODEX_CPP_BUILD_STATIC_LIB AND NOT CODEX_CPP_BUILD_SHARED_LIB)
    message(FATAL_ERROR "Enable at least one of CODEX_CPP_BUILD_EXE, CODEX_CPP_BUILD_STATIC_LIB, or CODEX_CPP_BUILD_SHARED_LIB.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

set(CPR_BUILD_TESTS OFF CACHE INTERNAL "")
set(CPR_USE_SYSTEM_CURL OFF CACHE INTERNAL "")
set(CPR_CURL_USE_LIBPSL OFF CACHE INTERNAL "")
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG f091b2c061b307ee89b164c39976fc9202a1c79d
)
FetchContent_MakeAvailable(cpr)

set(CODEX_CPP_LIBRARY_SOURCES
    src/app.cpp
    src/codex_cpp_c_api.cpp
)

function(configure_codex_cpp_target target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_23)
    target_include_directories(${target_name}
        PUBLIC
            include
        PRIVATE
            src
    )

    target_compile_definitions(${target_name} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    target_link_libraries(${target_name} PUBLIC
        cpr::cpr
        nlohmann_json::nlohmann_json
        crypt32
        shell32
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /permissive-
            /EHsc
            /utf-8
            /Zc:__cplusplus
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/GL>
        )
        target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/LTCG>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-flto>
            $<$<CONFIG:Release>:-march=native>
        )
        target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:-flto>)
    endif()
endfunction()

if(CODEX_CPP_BUILD_STATIC_LIB)
    add_library(codex-cpp-static STATIC ${CODEX_CPP_LIBRARY_SOURCES})
    set_target_properties(codex-cpp-static PROPERTIES OUTPUT_NAME codex-cpp-static)
    configure_codex_cpp_target(codex-cpp-static)
endif()

if(CODEX_CPP_BUILD_SHARED_LIB)
    add_library(codex-cpp-shared SHARED ${CODEX_CPP_LIBRARY_SOURCES})
    set_target_properties(codex-cpp-shared PROPERTIES OUTPUT_NAME codex-cpp)
    target_compile_definitions(codex-cpp-shared PRIVATE CODEX_CPP_SHARED CODEX_CPP_BUILDING_SHARED)
    target_compile_definitions(codex-cpp-shared INTERFACE CODEX_CPP_SHARED)
    configure_codex_cpp_target(codex-cpp-shared)
endif()

if(CODEX_CPP_BUILD_EXE)
    add_executable(codex-cpp src/main.cpp)

    if(TARGET codex-cpp-static)
        target_link_libraries(codex-cpp PRIVATE codex-cpp-static)
    elseif(TARGET codex-cpp-shared)
        target_link_libraries(codex-cpp PRIVATE codex-cpp-shared)
    else()
        target_sources(codex-cpp PRIVATE ${CODEX_CPP_LIBRARY_SOURCES})
        configure_codex_cpp_target(codex-cpp)
    endif()

    if(TARGET codex-cpp-static OR TARGET codex-cpp-shared)
        target_compile_features(codex-cpp PRIVATE cxx_std_23)
        target_include_directories(codex-cpp PRIVATE src)
        if(MSVC)
            target_compile_options(codex-cpp PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
        else()
            target_compile_options(codex-cpp PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endif()
