cmake_minimum_required(VERSION 3.20)
project(note4c_weather_preview LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LV_BUILD_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl"
                 "${CMAKE_CURRENT_BINARY_DIR}/lvgl")

set(FW "${CMAKE_CURRENT_SOURCE_DIR}/../../main")
set(FONT_COMPONENT "${FW}/components/78__xiaozhi-fonts")
set(FONTS "${FONT_COMPONENT}/src")

add_executable(weather_renderer_preview
    main.cc
    "${FW}/ui/renderers/rawdraw/weather_renderer.cc"
    "${FW}/rawdraw/rawdraw.cc"
    "${FW}/rawdraw/layout_utils.cc"
    "${FW}/rawdraw/theme.cc"
    "${FONTS}/SourceHanSansSC_Regular_slim.c"
    "${FONTS}/SourceHanSansSC_Medium_slim.c"
    "${FONTS}/font_zectrix_16_1.c"
    "${FONTS}/weather_icons/weather_icons_16.c"
    "${FONTS}/weather_icons/weather_icons_48.c"
)

target_link_libraries(weather_renderer_preview PRIVATE lvgl::lvgl)
target_include_directories(weather_renderer_preview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${FW}"
    "${FW}/common"
    "${FW}/rawdraw"
    "${FW}/ui/renderers/rawdraw"
    "${FONT_COMPONENT}/include"
    "${FONTS}/weather_icons"
)
target_compile_definitions(weather_renderer_preview PRIVATE LV_CONF_INCLUDE_SIMPLE)
if(MINGW)
    target_link_options(weather_renderer_preview PRIVATE -static)
endif()
