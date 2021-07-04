cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(TinyLeakCheck)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo" CACHE STRING "" FORCE)

add_library(TinyLeakCheck
	"tinyleakcheck/tiniestleakcheck.hpp"
	"tinyleakcheck/tinyleakcheck.hpp"
	"tinyleakcheck/tinyleakcheck.cpp"
)
set_target_properties(TinyLeakCheck PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(TinyLeakCheck PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(example "example/main.cpp")
set_target_properties(example PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(example TinyLeakCheck)

set_property( DIRECTORY PROPERTY VS_STARTUP_PROJECT example )
