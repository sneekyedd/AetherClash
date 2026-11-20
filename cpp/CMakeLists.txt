cmake_minimum_required(VERSION 3.16)
project(AetherClash C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The release workflow writes version.txt and server.txt at the repository root; both are baked into the game.
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(AETHER_VERSION "dev")
set(AETHER_SERVER "")
if(EXISTS ${ROOT}/version.txt)
    file(READ ${ROOT}/version.txt AETHER_VERSION)
    string(STRIP "${AETHER_VERSION}" AETHER_VERSION)
endif()
if(EXISTS ${ROOT}/server.txt)
    file(READ ${ROOT}/server.txt AETHER_SERVER)
    string(STRIP "${AETHER_SERVER}" AETHER_SERVER)
endif()
message(STATUS "Aether Clash version: ${AETHER_VERSION}   lobby server: '${AETHER_SERVER}'")

set(ASSET_CPP ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp)
set(ASSET_INPUTS "ASSET_FONT=${CMAKE_CURRENT_SOURCE_DIR}/assets/freesansbold.ttf|ASSET_GROUND=${CMAKE_CURRENT_SOURCE_DIR}/assets/ui_ground.png")
add_custom_command(
    OUTPUT ${ASSET_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${ASSET_CPP} "-DINPUTS=${ASSET_INPUTS}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake ${CMAKE_CURRENT_SOURCE_DIR}/assets/freesansbold.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui_ground.png
    VERBATIM)

find_package(SDL2 REQUIRED)

set(SOURCES src/gfx.cpp src/roster.cpp src/sim.cpp src/control.cpp src/art_pose.cpp src/art_fighter.cpp
            src/art_scene.cpp src/net.cpp src/annihilation.cpp src/game.cpp src/screens.cpp src/main.cpp ${ASSET_CPP})
if(WIN32)
    list(APPEND SOURCES win/aether.rc)
endif()

add_executable(AetherClash ${SOURCES})
target_include_directories(AetherClash PRIVATE src third_party)
target_compile_definitions(AetherClash PRIVATE AETHER_VERSION="${AETHER_VERSION}" AETHER_SERVER="${AETHER_SERVER}" SDL_MAIN_HANDLED)

if(TARGET SDL2::SDL2-static AND AETHER_STATIC)
    target_link_libraries(AetherClash PRIVATE SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    target_link_libraries(AetherClash PRIVATE SDL2::SDL2)
else()
    target_include_directories(AetherClash PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(AetherClash PRIVATE ${SDL2_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(AetherClash PRIVATE ws2_32 winhttp)
    set_target_properties(AetherClash PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MINGW)
        target_link_options(AetherClash PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(AetherClash PRIVATE Threads::Threads)
endif()
