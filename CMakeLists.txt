# command -  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="G:/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake_minimum_required(VERSION 3.15) 

project(gitm VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------ CPM -----------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake")
    message("-- Downloading CPM v0.42.3")
    file(
        DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.3/CPM.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
    )
    message("-- CPM Successfully fetched!")
else()
    message("-- CPM is already installed")
endif()
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# ----------- ToruNinna/toml11 ----------------------- 
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/third_party/toml.hpp") 
    message("-- Downloading ToruNiina/toml11's single header file... ") 
    file(
        DOWNLOAD
        https://raw.githubusercontent.com/ToruNiina/toml11/refs/heads/main/single_include/toml.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/toml.hpp
    )
    message("-- Fetched Single header file (ToruNinna/toml11) successfully!") 
else() 
    message("-- Header file for ToruNinna/toml11 is already there!" )
endif()

find_package(OpenSSL REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(SOURCES 
    src/main.cpp
    src/gitignore.cpp
    src/commits.cpp
    src/commit_file.cpp
    src/patchfile.cpp
    src/patch_reader.cpp
    src/gitm_apply.cpp
    src/email_sender.cpp
    src/manifest.cpp
    src/login_mgr.cpp
)
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/third_party
    incl/)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    CURL::libcurl
    libgit2::libgit2package
)
