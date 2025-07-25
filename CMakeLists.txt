# Set the minimum required version of CMake
cmake_minimum_required(VERSION 3.15)

# Set the project name and version
project(GDriveSplitter VERSION 1.0)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies using vcpkg ---
# vcpkg makes this part incredibly simple.
# It will find the packages you installed in the previous step.
find_package(cpr REQUIRED)
find_package(nlohmann_json REQUIRED)


# --- Configure the Executable ---

# Add our source files to a list
set(SOURCES
    src/main.cpp
    src/gdrive_handler.cpp
    src/gdrive_handler.h
    src/metadata_handler.cpp
    src/metadata_handler.h
)

# Add the executable target
add_executable(filesplitter ${SOURCES})

# Link the libraries to our executable
# vcpkg creates these clean "alias" targets for us to use.
# It automatically handles linking curl, threads, ssl, etc.
target_link_libraries(filesplitter PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
)

# --- Installation (Optional but good practice) ---
# This defines what happens when you "install" the application
install(TARGETS filesplitter DESTINATION bin)
