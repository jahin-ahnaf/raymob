# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Set the minimum required version of CMake
cmake_minimum_required(VERSION 3.22.1)

# Set standards for C and C++
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Set the project name based on the name given on the gradle.properties
project("${APP_LIB_NAME}")

# Include raylib and raymob as a subdirectories
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/raylib/CMakeLists.txt")
    include(FetchContent)
    FetchContent_Declare(
      raylib
      URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.tar.gz
    )
    FetchContent_Declare(
      enet
      GIT_REPOSITORY https://github.com/lsalzman/enet.git
      GIT_TAG v1.3.18
    )
    FetchContent_MakeAvailable(raylib enet)
else()
    add_subdirectory(${CMAKE_SOURCE_DIR}/deps/raylib)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/deps/raymob)

# Fetch all source files for your project (recursively), excluding 'deps' source files
file(GLOB_RECURSE SOURCES "${CMAKE_SOURCE_DIR}/*.c" "${CMAKE_SOURCE_DIR}/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/deps/.*")

# Add headers directory for android_native_app_glue.c
include_directories(${ANDROID_NDK}/sources/android/native_app_glue/)

# Add android_native_app_glue.c to the source files
list(APPEND SOURCES ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

# Create a shared library with game source files
add_library(${APP_LIB_NAME} SHARED ${SOURCES})

# Define compiler macros for the library
target_compile_definitions(${APP_LIB_NAME} PRIVATE PLATFORM_ANDROID)

# Apply flags depending on the build type
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    target_compile_definitions(${APP_LIB_NAME} PRIVATE _DEBUG DEBUG)
endif()

# Include raymob header files
target_include_directories(${APP_LIB_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/deps/raymob")
target_include_directories(${APP_LIB_NAME} PRIVATE "${enet_SOURCE_DIR}/include")

# Link required libraries to the native application
target_link_libraries(${APP_LIB_NAME} raylib raymoblib enet)
