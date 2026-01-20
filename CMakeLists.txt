cmake_minimum_required(VERSION 3.20)
project(quadrature VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_UI "Build GTK4 user interface" OFF)
option(BUILD_TESTS "Build test suite" OFF)
option(BUILD_INDEXER "Build indexer daemon" OFF)
option(BUILD_MUSICBRAINZ "Build MusicBrainz enrichment support" OFF)
option(BUILD_PRODUCTION "Production build with install targets" OFF)

# Build mode: DEBUG (local only) or BROADCAST (full features)
set(QUADRATURE_BUILD_MODE "DEBUG" CACHE STRING "Build mode: DEBUG or BROADCAST")
set_property(CACHE QUADRATURE_BUILD_MODE PROPERTY STRINGS "DEBUG" "BROADCAST")

# Set compile definitions based on build mode
if(QUADRATURE_BUILD_MODE STREQUAL "BROADCAST")
    add_definitions(-DQUADRATURE_BUILD_BROADCAST)
    message(STATUS "Build mode: BROADCAST (full features)")
else()
    add_definitions(-DQUADRATURE_BUILD_DEBUG)
    message(STATUS "Build mode: DEBUG (local only)")
endif()

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    endif()
endif()

# Linux-only: enable GNU extensions
add_definitions(-D_GNU_SOURCE)

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswresample libswscale libavfilter)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(FFTW3 REQUIRED fftw3)
pkg_check_modules(VIPS REQUIRED vips)
pkg_check_modules(RUBBERBAND REQUIRED rubberband>=3.0)

# MusicBrainz enrichment dependencies (optional)
if(BUILD_MUSICBRAINZ)
    pkg_check_modules(SOUP REQUIRED libsoup-3.0)
    pkg_check_modules(CHROMAPRINT REQUIRED libchromaprint)
    pkg_check_modules(TAGLIB REQUIRED taglib)
    pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
endif()

# Include dirs
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/cava
    ${GLIB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${VIPS_INCLUDE_DIRS}
    ${RUBBERBAND_INCLUDE_DIRS}
)

# MusicBrainz include dirs (optional)
if(BUILD_MUSICBRAINZ)
    include_directories(
        ${SOUP_INCLUDE_DIRS}
        ${CHROMAPRINT_INCLUDE_DIRS}
        ${TAGLIB_INCLUDE_DIRS}
        ${JSON_GLIB_INCLUDE_DIRS}
    )
endif()

# Core sources (audio pipeline with PipeWire + FFmpeg)
set(CORE_SOURCES
    src/audio/audio_pipeline.c
    src/audio/audio_buffer_store.c
    src/audio/audio_scrub.c
    src/audio/audio_spectrum.c
    src/audio/audio_devices.c
    src/core/perf_dashboard.c
    third_party/cava/cavacore.c
)

# Database sources (replaces library)
set(DATABASE_SOURCES
    src/database/db.c
    src/database/db_read.c
    src/database/db_write.c
)

# Library sources
set(LIBRARY_SOURCES
    src/library/library_source.c
)

# Indexer core sources (shared between daemon and embedded)
set(INDEXER_CORE_SOURCES
    src/indexer/indexer.c
    src/indexer/utils.c
    src/indexer/artwork.c
    src/indexer/folder_album.c
)

# MusicBrainz enrichment sources (optional)
if(BUILD_MUSICBRAINZ)
    set(MUSICBRAINZ_SOURCES
        src/musicbrainz/mb_tags.c
        src/musicbrainz/mb_client.c
        src/musicbrainz/mb_fingerprint.c
        src/musicbrainz/mb_acoustid.c
        src/musicbrainz/mb_parser.c
        src/musicbrainz/mb_tagger.c
        src/musicbrainz/mb_artwork.c
        src/musicbrainz/mb_enrichment.c
    )
endif()

# Shared engine library
add_library(quadrature_engine STATIC ${CORE_SOURCES} ${DATABASE_SOURCES} ${LIBRARY_SOURCES})
target_link_libraries(quadrature_engine
    ${GLIB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${FFTW3_LIBRARIES}
    ${RUBBERBAND_LIBRARIES}
    Threads::Threads
    m
)
target_link_directories(quadrature_engine PUBLIC ${GLIB_LIBRARY_DIRS} ${FFMPEG_LIBRARY_DIRS} ${SQLITE3_LIBRARY_DIRS} ${PIPEWIRE_LIBRARY_DIRS} ${FFTW3_LIBRARY_DIRS} ${RUBBERBAND_LIBRARY_DIRS})

# Note: Spectrum uses cavacore/FFTW which may conflict with Criterion signal handlers.
# If tests crash, disable spectrum in individual test targets rather than engine-wide.

# MusicBrainz enrichment library (optional)
if(BUILD_MUSICBRAINZ)
    add_library(quadrature_musicbrainz STATIC ${MUSICBRAINZ_SOURCES})
    target_include_directories(quadrature_musicbrainz PRIVATE
        ${CMAKE_SOURCE_DIR}/src/musicbrainz
        ${CMAKE_SOURCE_DIR}/src/database
    )
    target_link_libraries(quadrature_musicbrainz
        quadrature_engine
        ${SOUP_LIBRARIES}
        ${CHROMAPRINT_LIBRARIES}
        ${TAGLIB_LIBRARIES}
        ${JSON_GLIB_LIBRARIES}
    )
    target_link_directories(quadrature_musicbrainz PUBLIC
        ${SOUP_LIBRARY_DIRS}
        ${CHROMAPRINT_LIBRARY_DIRS}
        ${TAGLIB_LIBRARY_DIRS}
        ${JSON_GLIB_LIBRARY_DIRS}
    )
endif()

# Indexer core library (shared between daemon and embedded modes)
add_library(quadrature_indexer_core STATIC ${INDEXER_CORE_SOURCES})
target_include_directories(quadrature_indexer_core PRIVATE ${CMAKE_SOURCE_DIR}/src/indexer ${CMAKE_SOURCE_DIR}/src/database)
target_link_libraries(quadrature_indexer_core quadrature_engine OpenMP::OpenMP_C ${VIPS_LIBRARIES})
target_link_directories(quadrature_indexer_core PUBLIC ${VIPS_LIBRARY_DIRS})

# quadrature-indexer daemon
if(BUILD_INDEXER)
    set(INDEXER_DAEMON_SOURCES
        src/indexer/indexer_daemon.c
        src/indexer/indexer_config.c
        src/indexer/indexer_watcher.c
    )
    add_executable(quadrature-indexer bin/quadrature_indexer.c ${INDEXER_DAEMON_SOURCES})
    target_include_directories(quadrature-indexer PRIVATE ${CMAKE_SOURCE_DIR}/src/indexer)
    target_link_libraries(quadrature-indexer quadrature_indexer_core)

    # Link MusicBrainz enrichment if enabled
    if(BUILD_MUSICBRAINZ)
        target_link_libraries(quadrature-indexer quadrature_musicbrainz)
        target_compile_definitions(quadrature-indexer PRIVATE HAVE_MUSICBRAINZ=1)
    endif()
endif()

# GTK4 UI (optional)
if(BUILD_UI)
    pkg_check_modules(GTK4 REQUIRED gtk4)

    # Find glib-compile-resources for GResource compilation
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

    # GResource compilation
    set(GRESOURCE_XML ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature.gresource.xml)
    set(GRESOURCE_C ${CMAKE_BINARY_DIR}/quadrature-resources.c)
    set(GRESOURCE_DEPS
        ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature.css
        ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature_window.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/channel_strip.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/nav_bar.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/search_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_list_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/settings_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/help_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/detail_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/metadata_dialog.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_artist_row.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_album_row.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_track_row.ui
    )

    add_custom_command(
        OUTPUT ${GRESOURCE_C}
        COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_SOURCE_DIR}/src/ui/templates
            --generate-source
            ${GRESOURCE_XML}
        DEPENDS ${GRESOURCE_XML} ${GRESOURCE_DEPS}
        COMMENT "Compiling GResources"
    )

    set(UI_SOURCES
        src/ui/main.c
        src/ui/meters.c
        src/ui/channel_strip.c
        src/ui/window.c
        src/ui/metadata_dialog.c
        src/ui/indexer_controller.c
        src/ui/app_settings.c
        src/ui/row_helpers.c
        src/ui/library/cache.c
        src/ui/library/model.c
        src/ui/library/views.c
        src/ui/library/errors_view.c
        src/ui/library/artwork_manager.c
        src/ui/perf/perf_chart.c
        src/ui/perf/perf_view.c
        ${GRESOURCE_C}
    )


    add_executable(quadrature ${UI_SOURCES})
    target_include_directories(quadrature PRIVATE ${GTK4_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src/ui ${CMAKE_SOURCE_DIR}/src/ui/library ${CMAKE_SOURCE_DIR}/src/ui/perf ${CMAKE_SOURCE_DIR}/src/indexer)
    target_link_libraries(quadrature quadrature_indexer_core ${GTK4_LIBRARIES})
    target_link_directories(quadrature PRIVATE ${GTK4_LIBRARY_DIRS})
endif()

# Tests (optional)
if(BUILD_TESTS)
    enable_testing()
    pkg_check_modules(CRITERION REQUIRED criterion)

    file(GLOB TEST_SOURCES tests/unit/test_*.c)
    foreach(TEST_FILE ${TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
        if(${TEST_NAME} STREQUAL "test_database")
            add_executable(${TEST_NAME} ${TEST_FILE})
            target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/database ${CRITERION_INCLUDE_DIRS})
            target_link_libraries(${TEST_NAME} quadrature_engine ${CRITERION_LIBRARIES})
            target_link_directories(${TEST_NAME} PRIVATE ${CRITERION_LIBRARY_DIRS})
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
            continue()
        elseif(${TEST_NAME} MATCHES "test_indexer" OR ${TEST_NAME} MATCHES "test_multi_disc")
            add_executable(${TEST_NAME} ${TEST_FILE})
            target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/indexer ${CMAKE_SOURCE_DIR}/src/database)
            target_link_libraries(${TEST_NAME} quadrature_indexer_core ${CRITERION_LIBRARIES})
            target_link_directories(${TEST_NAME} PRIVATE ${CRITERION_LIBRARY_DIRS})
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
            continue()
        elseif(${TEST_NAME} MATCHES "test_audio")
            # Audio tests need access to internal headers and PipeWire
            add_executable(${TEST_NAME} ${TEST_FILE})
            target_include_directories(${TEST_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/src/audio
                ${CMAKE_SOURCE_DIR}/src/database
                ${CRITERION_INCLUDE_DIRS}
                ${PIPEWIRE_INCLUDE_DIRS}
                ${RUBBERBAND_INCLUDE_DIRS}
            )
            target_link_libraries(${TEST_NAME} quadrature_engine ${CRITERION_LIBRARIES})
            target_link_directories(${TEST_NAME} PRIVATE ${CRITERION_LIBRARY_DIRS})
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
            continue()
        else()
            add_executable(${TEST_NAME} ${TEST_FILE})
        endif()
        target_include_directories(${TEST_NAME} PRIVATE ${CRITERION_INCLUDE_DIRS})
        target_link_libraries(${TEST_NAME} quadrature_engine ${CRITERION_LIBRARIES})
        target_link_directories(${TEST_NAME} PRIVATE ${CRITERION_LIBRARY_DIRS})
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()

# Install targets
install(TARGETS quadrature_engine
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(TARGETS quadrature_indexer_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(BUILD_MUSICBRAINZ)
    install(TARGETS quadrature_musicbrainz
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )
endif()

if(BUILD_INDEXER)
    install(TARGETS quadrature-indexer RUNTIME DESTINATION bin)

    # Install systemd user service
    if(BUILD_PRODUCTION)
        configure_file(
            ${CMAKE_SOURCE_DIR}/data/quadrature-indexer.service.in
            ${CMAKE_BINARY_DIR}/quadrature-indexer.service
            @ONLY
        )
        install(FILES ${CMAKE_BINARY_DIR}/quadrature-indexer.service
                DESTINATION lib/systemd/user)
    endif()
endif()

if(BUILD_UI)
    install(TARGETS quadrature RUNTIME DESTINATION bin)
    install(FILES src/ui/templates/quadrature.css DESTINATION share/quadrature)

    if(BUILD_PRODUCTION)
        # Install desktop file
        configure_file(
            ${CMAKE_SOURCE_DIR}/data/org.quadrature.player.desktop.in
            ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop
            @ONLY
        )
        install(FILES ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop
                DESTINATION share/applications)

        # Install icons
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/icons/
                DESTINATION share/icons/hicolor
                FILES_MATCHING PATTERN "*.png" PATTERN "*.svg")
    endif()
endif()
