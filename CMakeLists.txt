cmake_minimum_required(VERSION 3.20)
project(TAREEK-Vis VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent Network)
find_package(ZLIB REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/core/types.cpp
    src/core/config.cpp
    src/core/transit_types.cpp
    src/core/video_recorder.cpp
    src/core/logger.cpp
    src/parsers/network_parser.cpp
    src/parsers/event_parser.cpp
    src/parsers/preprocessor.cpp
    src/parsers/transit_schedule_parser.cpp
    src/parsers/counts_parser.cpp
    src/data/network_index.cpp
    src/data/vehicle_index.cpp
    src/analysis/nkdv_network.cpp
    src/analysis/nkde_scatter.cpp
    src/analysis/heatmap_cache.cpp
    src/renderer/gl_renderer.cpp
    src/renderer/network_renderer.cpp
    src/renderer/vehicle_renderer.cpp
    src/renderer/transit_route_renderer.cpp
    src/renderer/counts_renderer.cpp
    src/renderer/person_route_renderer.cpp
    src/renderer/halo_renderer.cpp
    src/renderer/tile_renderer.cpp
    src/renderer/heatmap_renderer.cpp
    src/ui/main_window.cpp
    src/ui/map_widget.cpp
    src/ui/info_panel.cpp
    src/ui/hourly_volume_chart.cpp
    src/ui/timeline_widget.cpp
    src/ui/progress_dialog.cpp
    src/ui/video_settings_dialog.cpp
    src/ui/counts_chart_dialog.cpp
    src/ui/screenshot_exporter.cpp
    third_party/pugixml.cpp
)

set(HEADERS
    src/core/types.h
    src/core/config.h
    src/core/transit_types.h
    src/core/video_recorder.h
    src/core/logger.h
    src/parsers/network_parser.h
    src/parsers/event_parser.h
    src/parsers/preprocessor.h
    src/parsers/transit_schedule_parser.h
    src/parsers/counts_parser.h
    src/data/network_index.h
    src/data/vehicle_index.h
    src/analysis/nkdv_network.h
    src/analysis/nkde_scatter.h
    src/analysis/heatmap_cache.h
    src/renderer/gl_renderer.h
    src/renderer/network_renderer.h
    src/renderer/vehicle_renderer.h
    src/renderer/transit_route_renderer.h
    src/renderer/counts_renderer.h
    src/renderer/person_route_renderer.h
    src/renderer/halo_renderer.h
    src/renderer/tile_renderer.h
    src/renderer/heatmap_renderer.h
    src/renderer/heatmap_palette.h
    src/core/crs_transform.h
    src/ui/main_window.h
    src/ui/map_widget.h
    src/ui/info_panel.h
    src/ui/hourly_volume_chart.h
    src/ui/panel_style.h
    src/ui/timeline_widget.h
    src/ui/progress_dialog.h
    src/ui/video_settings_dialog.h
    src/ui/counts_chart_dialog.h
    src/ui/screenshot_exporter.h
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# The version lives only in the project() call above. Anything that shows it to
# a user reads it from here, so a release cannot ship mismatched numbers.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    TAREEK_VIS_VERSION="${PROJECT_VERSION}"
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
    Qt6::Network
    ZLIB::ZLIB
)

# Platform-specific settings
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE opengl32)
    # Deploy Qt DLLs on Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# ---------------------------------------------------------------------------
# tareek-bench: headless benchmark driver
#
# The desktop app needs a display and a GPU, which a compute node has neither
# of. This second target exposes the same parse / index / density pipeline as a
# batch program, so the paper's measurements can run on a headless machine and
# be repeated exactly.
#
# It links Qt6::Core only. Every stage it drives already depends on nothing
# more, and keeping it that way is the point: if this target ever needs Widgets
# or OpenGL, logic has leaked into the wrong layer.
# ---------------------------------------------------------------------------
set(BENCH_SOURCES
    src/tools/benchmark_main.cpp
    src/core/types.cpp
    src/core/transit_types.cpp
    src/core/logger.cpp
    src/parsers/network_parser.cpp
    src/parsers/event_parser.cpp
    src/parsers/preprocessor.cpp
    src/parsers/transit_schedule_parser.cpp
    src/data/network_index.cpp
    src/data/vehicle_index.cpp
    src/analysis/nkdv_network.cpp
    src/analysis/nkde_scatter.cpp
    # The gather-form baseline is measured by tareek-bench only; the
    # application ships the scatter form alone.
    src/analysis/nkde_gather.cpp
    src/analysis/heatmap_cache.cpp
    third_party/pugixml.cpp
)

add_executable(tareek-bench ${BENCH_SOURCES})

target_compile_definitions(tareek-bench PRIVATE
    TAREEK_VIS_VERSION="${PROJECT_VERSION}"
)

target_include_directories(tareek-bench PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(tareek-bench PRIVATE
    Qt6::Core
    ZLIB::ZLIB
)

# Copy sample data for testing, if present locally (not part of the repo)
if(EXISTS ${CMAKE_SOURCE_DIR}/inputs)
    file(COPY ${CMAKE_SOURCE_DIR}/inputs DESTINATION ${CMAKE_BINARY_DIR})
endif()

# Copy FFmpeg to build directory if it exists
if(EXISTS ${CMAKE_SOURCE_DIR}/ffmpeg)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/ffmpeg $<TARGET_FILE_DIR:${PROJECT_NAME}>/ffmpeg
        COMMENT "Copying ffmpeg directory to build output"
    )
    message(STATUS "FFmpeg directory found - will be copied to build output")
else()
    message(STATUS "FFmpeg directory not found - video recording will require system FFmpeg")
endif()