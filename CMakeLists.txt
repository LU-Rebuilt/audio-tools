cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(audio-tools VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "LEGO Universe FMOD audio tools")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(lu_assets
    GIT_REPOSITORY https://github.com/LU-Rebuilt/lu-assets.git
    GIT_TAG main
)
FetchContent_MakeAvailable(lu_assets)

add_executable(fsb_extract src/fsb_extract.cpp)
target_link_libraries(fsb_extract PRIVATE lu_assets)

add_executable(fev_to_fdp src/fev_to_fdp.cpp)
target_link_libraries(fev_to_fdp PRIVATE lu_assets)
target_include_directories(fev_to_fdp PRIVATE src)

add_executable(fev_project_setup src/fev_project_setup.cpp)
target_link_libraries(fev_project_setup PRIVATE lu_assets)
target_include_directories(fev_project_setup PRIVATE src)
