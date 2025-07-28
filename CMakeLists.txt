# Set the minimum required version of CMake
cmake_minimum_required(VERSION 3.15)

# Set the project name and version
project(GDriveSplitter VERSION 1.0)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies using vcpkg ---
find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(indicators REQUIRED)
find_package(httplib REQUIRED)# Find the new progress bar library


# --- Configure the Executable ---
set(SOURCES
    src/main.cpp
    src/Shell.cpp
    src/Shell.h
    src/gdrive_handler.cpp
    src/gdrive_handler.h
)
add_executable(filesplitter ${SOURCES})

# Add this line to prevent min/max macro conflicts on Windows
target_compile_definitions(filesplitter PRIVATE NOMINMAX)

# Link the libraries to our executable
target_link_libraries(filesplitter PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
    indicators::indicators 
    httplib::httplib
    )

# --- Installation (Optional but good practice) ---
install(TARGETS filesplitter DESTINATION bin)
