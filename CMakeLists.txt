cmake_minimum_required(VERSION 3.20)
project(gtasa_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(GTASA_DEBUG_LOG "Write the compatibility debug log" OFF)
option(GTASA_QUIT_CHORD "Enable the Guide/Back + Start quit chord" ON)
option(GTASA_SDL2_SHIM "Build against the SDL3-to-system-SDL2 runtime shim" OFF)
set(GTASA_PRODUCT_NAME "Grand Theft Auto: San Andreas" CACHE STRING "Window and fatal-error product name")
set(GTASA_CONFIG_NAME "gtasa_nx.cfg" CACHE STRING "Wrapper configuration filename")
set(GTASA_APPSTATE_NAME "appstate.txt" CACHE STRING "Wrapper app-state filename")
set(GAME_BINARY_NAME "gtasa_linux" CACHE STRING "Game executable target and output name")


find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(OPENAL REQUIRED IMPORTED_TARGET openal)
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)

add_executable(${GAME_BINARY_NAME}
    source/aml_mod.c
    source/config.c
    source/error_linux.c
    source/imports.c
    source/input_linux.c
    source/jni_fake.c
    source/libc_shim.c
    source/main_linux.c
    source/overlay.c
    source/so_util.c
    source/util.c
    source/hooks/game_linux.c
    source/audio_linux.c
)

target_include_directories(${GAME_BINARY_NAME} PRIVATE source source/hooks)
target_link_libraries(${GAME_BINARY_NAME} PRIVATE
    PkgConfig::SDL3
    PkgConfig::EGL
    PkgConfig::GLES2
    PkgConfig::OPENAL
    PkgConfig::MPG123
    m dl pthread
)

target_compile_definitions(${GAME_BINARY_NAME} PRIVATE
    _GNU_SOURCE
    GTASA_PRODUCT_NAME="${GTASA_PRODUCT_NAME}"
    GTASA_CONFIG_NAME="${GTASA_CONFIG_NAME}"
    GTASA_APPSTATE_NAME="${GTASA_APPSTATE_NAME}"

)
if(GTASA_DEBUG_LOG)
    target_compile_definitions(${GAME_BINARY_NAME} PRIVATE DEBUG_LOG=1)
endif()
if(GTASA_QUIT_CHORD)
    target_compile_definitions(${GAME_BINARY_NAME} PRIVATE GTASA_QUIT_CHORD=1)
endif()
target_compile_options(${GAME_BINARY_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS ${GAME_BINARY_NAME} RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(input_linux_test tests/input_linux_test.c source/input_linux.c)
    target_include_directories(input_linux_test PRIVATE source)
    target_link_libraries(input_linux_test PRIVATE PkgConfig::SDL3 m)
    if(GTASA_QUIT_CHORD)
        target_compile_definitions(input_linux_test PRIVATE GTASA_QUIT_CHORD=1)
    endif()
    target_compile_options(input_linux_test PRIVATE -Wall -Wextra -UNDEBUG)
    add_test(NAME input_linux COMMAND input_linux_test)
    set_tests_properties(input_linux PROPERTIES TIMEOUT 10)
    if(NOT GTASA_SDL2_SHIM)
        add_executable(audio_linux_test tests/audio_linux_test.c source/audio_linux.c)
        target_include_directories(audio_linux_test PRIVATE source)
        target_link_libraries(audio_linux_test PRIVATE PkgConfig::SDL3 PkgConfig::OPENAL m pthread)
        target_compile_options(audio_linux_test PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME audio_linux COMMAND audio_linux_test)
        set_tests_properties(audio_linux PROPERTIES ENVIRONMENT "SDL_AUDIO_DRIVER=dummy" TIMEOUT 10)
    endif()
endif()
