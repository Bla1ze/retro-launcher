cmake_minimum_required(VERSION 3.5)

# Retro Launcher: a Neon-themed console launcher with its own libretro player.
# Builds with the AtGames External Applications SDK toolchain (see README.md).
# Plays ROMs from the app folder with the firmware's emulator cores (or cores
# shipped in cores/) on the playfield, backglass or DMD, chosen per game.
# The SDK toolchain sets no optimisation level, so builds were -O0. The panel
# compositing, audio resampler and frame sampling all need a real -O2.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
project(retro-launcher LANGUAGES CXX)

set(ATGAMES_APP_NAME "retro-launcher" CACHE STRING "External Application folder/output name")

find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATHS /usr/aarch64-linux-gnu/include/SDL2 NO_DEFAULT_PATH)
find_library(SDL2_LIBRARY NAMES SDL2 PATHS /usr/aarch64-linux-gnu/lib NO_DEFAULT_PATH)
if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(FATAL_ERROR "SDL2 was not found. Rebuild the SDK Docker image.")
endif()

# Embed Noto Sans (body) and Bebas Neue (display) in the ELF, as Jukebox does.
set(FONT_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(FONT_OBJECTS "")
foreach(font NotoSans-Regular BebasNeue-Regular)
    set(obj "${CMAKE_CURRENT_BINARY_DIR}/${font}.o")
    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT_ASSET_DIR}/${font}.ttf" "${CMAKE_CURRENT_BINARY_DIR}/${font}.ttf"
        COMMAND aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 -B aarch64 ${font}.ttf ${font}.o
        DEPENDS "${FONT_ASSET_DIR}/${font}.ttf"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Embedding ${font}")
    list(APPEND FONT_OBJECTS "${obj}")
endforeach()
set_source_files_properties(${FONT_OBJECTS} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_executable(${ATGAMES_APP_NAME}
    ${FONT_OBJECTS}
    src/controls/Controls.cpp
    src/Ambient.cpp
    src/AppFont.cpp
    src/Arcade.cpp
    src/GamePanels.cpp
    src/Gfx.cpp
    src/PanelFont.cpp
    src/Launcher.cpp
    src/Library.cpp
    src/Player.cpp
    src/main.cpp
)
set_property(TARGET ${ATGAMES_APP_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${ATGAMES_APP_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
target_include_directories(${ATGAMES_APP_NAME} PRIVATE
    ${SDL2_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)
target_link_libraries(${ATGAMES_APP_NAME} PRIVATE ${SDL2_LIBRARY} pthread dl m)

set(DIST_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/external/${ATGAMES_APP_NAME}")
set(METADATA_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/metadata")
set_target_properties(${ATGAMES_APP_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DIST_APP_DIR}" SUFFIX ".elf")
add_custom_command(TARGET ${ATGAMES_APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${METADATA_ASSET_DIR}/${ATGAMES_APP_NAME}.png" "${DIST_APP_DIR}/${ATGAMES_APP_NAME}.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${METADATA_ASSET_DIR}/${ATGAMES_APP_NAME}.xml" "${DIST_APP_DIR}/${ATGAMES_APP_NAME}.xml")
