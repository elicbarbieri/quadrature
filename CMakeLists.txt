cmake_minimum_required(VERSION 3.20)
project(quadrature VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_UI "Build GTK4 user interface" OFF)
option(BUILD_TESTS "Build test suite" OFF)
option(BUILD_CLI "Build quadrature-cli" OFF)
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
#
# CPU baseline: x86-64-v3 (Haswell-era: AVX2, FMA, BMI1/2, ~2013+).
# Hot paths that need newer ISAs (AVX-512, etc.) use function multi-versioning
# via __attribute__((target_clones(...))) or runtime dispatch with
# __builtin_cpu_supports(). This keeps binaries portable for distribution
# (Flathub, generic .deb/.rpm) while still using modern SIMD on most hardware.
#
# For local development on a known machine, set -DQUADRATURE_NATIVE_BUILD=ON
# to opt back into -march=native -mtune=native.
option(QUADRATURE_NATIVE_BUILD "Build for the local CPU (-march=native). Off for distributable builds." OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG -ftree-vectorize)
        if(QUADRATURE_NATIVE_BUILD)
            add_compile_options(-march=native -mtune=native)
        else()
            add_compile_options(-march=x86-64-v3 -mtune=generic)
        endif()
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)  # LTO: cross-TU inlining
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
pkg_check_modules(CHROMAPRINT REQUIRED libchromaprint)
pkg_check_modules(TAGLIB REQUIRED taglib)
# PostgreSQL backend — required for talking to a self-hosted MusicBrainz
# mirror. Native (Nix/distro) builds default ON. Flatpak builds set OFF
# and the binary uses the HTTP backend exclusively (mb+http:// URI scheme).
option(QUADRATURE_USE_LIBPQ "Build the PostgreSQL MusicBrainz backend" ON)
if(QUADRATURE_USE_LIBPQ)
    pkg_check_modules(LIBPQ REQUIRED libpq)
    add_compile_definitions(QUADRATURE_USE_LIBPQ)
endif()

pkg_check_modules(LIBSOUP3 REQUIRED libsoup-3.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

# ============================================================================
# Bundled application identifiers
#
# Project-level identifiers that ship in every Quadrature binary. These name
# the application to the respective services — they are NOT per-user keys.
# Users do not need accounts or credentials to use any of these endpoints.
#
# Downstream distros / forks can swap any of these with their own at build
# time via -D, e.g. `cmake -DACOUSTID_APPLICATION_KEY="..."`. This keeps
# quota namespaces separated when a fork ships under a different brand.
# ============================================================================

if(NOT DEFINED ACOUSTID_APPLICATION_KEY)
    set(ACOUSTID_APPLICATION_KEY "N3PlLWjzbn"
        CACHE STRING "AcoustID application key — identifies Quadrature to api.acoustid.org")
endif()

if(NOT DEFINED FANART_TV_APPLICATION_KEY)
    set(FANART_TV_APPLICATION_KEY "400cfe22b47e1fb93bfeb3ca452059c8"
        CACHE STRING "fanart.tv project API key — identifies Quadrature to webservice.fanart.tv")
endif()

if(NOT DEFINED MUSICBRAINZ_USER_AGENT)
    set(MUSICBRAINZ_USER_AGENT "quadrature/${PROJECT_VERSION} ( https://github.com/elibarbieri/quadrature )"
        CACHE STRING "User-Agent string sent to musicbrainz.org (TOS-required identifier)")
endif()

add_compile_definitions(
    ACOUSTID_APPLICATION_KEY="${ACOUSTID_APPLICATION_KEY}"
    FANART_TV_APPLICATION_KEY="${FANART_TV_APPLICATION_KEY}"
    MUSICBRAINZ_USER_AGENT="${MUSICBRAINZ_USER_AGENT}"
)

# Include dirs
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${GLIB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${VIPS_INCLUDE_DIRS}
    ${RUBBERBAND_INCLUDE_DIRS}
    ${CHROMAPRINT_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
    ${LIBSOUP3_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
)

# Core sources (audio pipeline with PipeWire + FFmpeg)
set(CORE_SOURCES
    src/audio/audio_pipeline.c
    src/audio/audio_cache.c
    src/audio/audio_scrub.c
    src/audio/audio_spectrum.c
    src/audio/audio_devices.c
    src/core/perf_dashboard.c
    src/audio/cavacore.c
)

# Database sources
set(DATABASE_SOURCES
    src/database/db.c
    src/database/db_read.c
    src/database/db_write.c
    src/database/reconciler.c
    src/database/db_meta.c
    src/database/db_bios.c
    src/database/migrations/001_initial.c
)

# Library sources
set(LIBRARY_SOURCES
    src/library/library_cache.c
    src/library/library_mask.c
    src/library/cache_search.c
    src/library/library_search.c
)

# Indexer core sources (shared between daemon and embedded)
# Includes MusicBrainz/AcoustID resolution (always available)
set(INDEXER_CORE_SOURCES
    src/indexer/indexer.c
    src/indexer/utils.c
    src/indexer/library_validation.c
    src/indexer/artwork.c
    src/indexer/musicbrainz/mb_fingerprint.c
    src/indexer/musicbrainz/mb_backend.c
    src/indexer/musicbrainz/mb_resolver.c
    # HTTP backend (always built — no libpq dep)
    src/indexer/musicbrainz/mb_http_backend.c
    src/indexer/musicbrainz/mb_http_ops.c
    src/indexer/artist_art.c
    src/indexer/artist_atlas.c
    src/indexer/artist_bio.c
    src/indexer/change_tracker.c
)

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

# Indexer core library (shared between daemon and embedded modes)
# Includes MusicBrainz resolution — chromaprint, taglib, libpq linked here
add_library(quadrature_indexer_core STATIC ${INDEXER_CORE_SOURCES})
if(QUADRATURE_USE_LIBPQ)
    target_sources(quadrature_indexer_core PRIVATE
        src/indexer/musicbrainz/mb_pg_client.c
        src/indexer/musicbrainz/mb_pg_acoustid.c
        src/indexer/musicbrainz/mb_pg_backend.c
    )
    target_include_directories(quadrature_indexer_core PRIVATE ${LIBPQ_INCLUDE_DIRS})
endif()
target_include_directories(quadrature_indexer_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src/indexer
    ${CMAKE_SOURCE_DIR}/src/indexer/musicbrainz
    ${CMAKE_SOURCE_DIR}/src/database
)
target_link_libraries(quadrature_indexer_core
    quadrature_engine
    OpenMP::OpenMP_C
    anl
    ${VIPS_LIBRARIES}
    ${CHROMAPRINT_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ${LIBSOUP3_LIBRARIES}
    ${JSON_GLIB_LIBRARIES}
)
if(QUADRATURE_USE_LIBPQ)
    target_link_libraries(quadrature_indexer_core ${LIBPQ_LIBRARIES})
endif()
target_link_directories(quadrature_indexer_core PUBLIC
    ${VIPS_LIBRARY_DIRS}
    ${CHROMAPRINT_LIBRARY_DIRS}
    ${TAGLIB_LIBRARY_DIRS}
    ${LIBSOUP3_LIBRARY_DIRS}
    ${JSON_GLIB_LIBRARY_DIRS}
)
if(QUADRATURE_USE_LIBPQ)
    target_link_directories(quadrature_indexer_core PUBLIC ${LIBPQ_LIBRARY_DIRS})
endif()

# GPIO library (Axia Livewire+ integration)
set(GPIO_SOURCES
    src/gpio/axia_gpio.c
    src/gpio/axia_protocol.c
)
add_library(quadrature_gpio STATIC ${GPIO_SOURCES})
target_include_directories(quadrature_gpio PRIVATE
    ${CMAKE_SOURCE_DIR}/src/gpio
)
target_link_libraries(quadrature_gpio
    ${GLIB_LIBRARIES}
    Threads::Threads
)
target_link_directories(quadrature_gpio PUBLIC ${GLIB_LIBRARY_DIRS})

# quadrature-cli (unified CLI: indexer, setup-rt, etc.)
if(BUILD_CLI)
    set(CLI_SOURCES
        src/cli/cli_indexer.c
        src/cli/cli_setup_rt.c
        src/indexer/indexer_daemon.c
        src/indexer/indexer_config.c
        src/indexer/indexer_watcher.c
    )
    add_executable(quadrature-cli bin/quadrature_cli.c ${CLI_SOURCES})
    target_include_directories(quadrature-cli PRIVATE
        ${CMAKE_SOURCE_DIR}/src/cli
        ${CMAKE_SOURCE_DIR}/src/indexer
    )
    target_link_libraries(quadrature-cli quadrature_indexer_core)
endif()

# GTK4 UI (optional)
if(BUILD_UI)
    pkg_check_modules(GTK4 REQUIRED gtk4)
    pkg_check_modules(LIBADWAITA REQUIRED libadwaita-1)

    # Find glib-compile-resources for GResource compilation
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

    # CSS concatenation - combine modular CSS into single file
    set(CSS_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/ui/css)
    set(CSS_SOURCES
        ${CSS_SOURCE_DIR}/quadrature.css
        ${CSS_SOURCE_DIR}/ui.css
        ${CSS_SOURCE_DIR}/library.css
        ${CSS_SOURCE_DIR}/detail_view.css
        ${CSS_SOURCE_DIR}/channel-strip.css
    )
    set(CSS_COMBINED ${CMAKE_BINARY_DIR}/quadrature.css)

    add_custom_command(
        OUTPUT ${CSS_COMBINED}
        COMMAND ${CMAKE_COMMAND} -E cat ${CSS_SOURCES} > ${CSS_COMBINED}
        DEPENDS ${CSS_SOURCES}
        COMMENT "Combining CSS files"
    )

    # GResource compilation
    set(GRESOURCE_XML ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature.gresource.xml)
    set(GRESOURCE_C ${CMAKE_BINARY_DIR}/quadrature-resources.c)
    set(GRESOURCE_DEPS
        ${CSS_COMBINED}
        ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature_window.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/channel_strip.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/nav_bar.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/search_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_list_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/libraries_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/settings_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/help_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/metadata_dialog.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_artist_row.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_album_row.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/library_track_row.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/album_detail_track_item.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/album_detail_track_row_wrapper.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/album_card.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/album_button.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/filter_sort_bar.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/advanced_search_bar.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/perf_view.ui
        ${CMAKE_SOURCE_DIR}/src/ui/templates/perf_channel_cell.ui
    )

    add_custom_command(
        OUTPUT ${GRESOURCE_C}
        COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_BINARY_DIR}
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
        src/ui/library_monitor.c
        src/ui/app_settings.c
        src/ui/utils.c
        src/ui/ui_math.c
        src/ui/overflow_plan.c
        src/ui/row_helpers.c
        src/ui/artist_buttons.c
        src/ui/row_factories.c
        src/ui/search/search_view.c
        src/ui/libraries/libraries_view.c
        src/ui/libraries/indexer_bridge.c
        src/ui/settings/settings_view.c
        src/ui/settings/device_settings.c
        src/ui/settings/gpio_bridge.c
        src/ui/library/filter_bar.c
        src/ui/library/views.c
        src/ui/library/lazy_list.c
        src/ui/library/scrubber.c
        src/ui/library/detail_view.c
        src/ui/library/gradient_widgets.c
        src/ui/library/credits_view.c
        src/ui/library/errors_view.c
        src/ui/library/artwork_manager.c
        src/ui/perf/perf_chart.c
        src/ui/perf/perf_grouped_hist.c
        src/ui/perf/perf_view.c
        ${GRESOURCE_C}
    )


    add_executable(quadrature ${UI_SOURCES})
    target_include_directories(quadrature PRIVATE ${GTK4_INCLUDE_DIRS} ${LIBADWAITA_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src/ui ${CMAKE_SOURCE_DIR}/src/ui/library ${CMAKE_SOURCE_DIR}/src/ui/perf ${CMAKE_SOURCE_DIR}/src/ui/search ${CMAKE_SOURCE_DIR}/src/ui/libraries ${CMAKE_SOURCE_DIR}/src/ui/settings ${CMAKE_SOURCE_DIR}/src/indexer)
    target_link_libraries(quadrature quadrature_indexer_core quadrature_gpio ${GTK4_LIBRARIES} ${LIBADWAITA_LIBRARIES})
    target_link_directories(quadrature PRIVATE ${GTK4_LIBRARY_DIRS} ${LIBADWAITA_LIBRARY_DIRS})
endif()

# Tests (optional)
if(BUILD_TESTS)
    enable_testing()
    pkg_check_modules(CRITERION REQUIRED criterion)

    function(unit_test NAME)
        cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
        add_executable(${NAME} tests/unit/${NAME}.c ${T_SOURCES})
        target_include_directories(${NAME} PRIVATE ${CRITERION_INCLUDE_DIRS} ${T_INCLUDES})
        target_link_libraries(${NAME} ${CRITERION_LIBRARIES} ${T_LIBS})
        target_link_directories(${NAME} PRIVATE ${CRITERION_LIBRARY_DIRS})
        target_compile_definitions(${NAME} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_test(NAME ${NAME} COMMAND ${NAME})
    endfunction()

    set(S ${CMAKE_SOURCE_DIR}/src)

    # Standalone (no engine)
    unit_test(test_overflow_layout  SOURCES src/ui/overflow_plan.c  LIBS ${GLIB_LIBRARIES})
    unit_test(test_library_mask       SOURCES src/library/library_mask.c  LIBS ${GLIB_LIBRARIES})

    # Engine tests
    unit_test(test_database         INCLUDES ${S}/database          LIBS quadrature_engine)
    unit_test(test_library_cache                                    LIBS quadrature_engine)
    unit_test(test_path_resolution                                  LIBS quadrature_engine)

    # Audio tests
    unit_test(test_audio_pipeline   INCLUDES ${S}/audio ${S}/database  LIBS quadrature_engine)
    unit_test(test_audio_cache      INCLUDES ${S}/audio ${S}/database  LIBS quadrature_engine)
    unit_test(test_audio_scrubber   INCLUDES ${S}/audio ${S}/database  LIBS quadrature_engine)

    # Indexer tests
    unit_test(test_indexer              INCLUDES ${S}/indexer ${S}/database  LIBS quadrature_indexer_core)
    unit_test(test_indexer_integration  INCLUDES ${S}/indexer ${S}/database  LIBS quadrature_indexer_core quadrature_engine)
    unit_test(test_reconciler           INCLUDES ${S}/database  LIBS quadrature_engine)
    unit_test(test_multi_disc           INCLUDES ${S}/indexer ${S}/database  LIBS quadrature_indexer_core)

    # MusicBrainz tests
    unit_test(test_mb_resolve  INCLUDES ${S}/indexer/musicbrainz ${S}/database  LIBS quadrature_indexer_core quadrature_engine)

    # End-to-end integration tests (require MB PG + SOLR — skip if unavailable)
    unit_test(test_e2e_library_resolution  INCLUDES ${S}/indexer ${S}/indexer/musicbrainz ${S}/database  LIBS quadrature_indexer_core quadrature_engine)
    unit_test(test_metadata_search         INCLUDES ${S}/indexer ${S}/indexer/musicbrainz ${S}/database  LIBS quadrature_indexer_core quadrature_engine)

    # GPIO tests
    unit_test(test_axia_gpio  INCLUDES ${S}/gpio  LIBS quadrature_gpio ${GLIB_LIBRARIES} Threads::Threads)
endif()

# Install targets
if(BUILD_CLI)
    install(TARGETS quadrature-cli RUNTIME DESTINATION bin)
endif()

if(BUILD_UI)
    install(TARGETS quadrature RUNTIME DESTINATION bin)
    install(FILES ${CMAKE_BINARY_DIR}/quadrature.css DESTINATION share/quadrature)

    if(BUILD_PRODUCTION)
        # Install desktop file
        configure_file(
            ${CMAKE_SOURCE_DIR}/data/org.quadrature.player.desktop.in
            ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop
            @ONLY
        )
        install(FILES ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop
                DESTINATION share/applications)

        # Install AppStream metainfo (required for Flathub; harmless on native)
        install(FILES ${CMAKE_SOURCE_DIR}/data/org.quadrature.player.metainfo.xml
                DESTINATION share/metainfo)

        # Install icons (data/icons/hicolor/... → share/icons/hicolor/...)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/icons/
                DESTINATION share/icons
                FILES_MATCHING PATTERN "*.png" PATTERN "*.svg")
    endif()
endif()
