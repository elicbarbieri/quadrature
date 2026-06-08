cmake_minimum_required(VERSION 3.25)
project(quadrature
    VERSION 0.1.0
    DESCRIPTION "Four-channel broadcast audio player"
    HOMEPAGE_URL "https://github.com/elicbarbieri/quadrature"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# === Options ================================================================

option(BUILD_UI                "Build GTK4 user interface"                       OFF)
option(BUILD_CLI               "Build quadrature-cli"                            OFF)
option(BUILD_TESTS             "Build the Criterion test suite"                  OFF)
option(BUILD_PRODUCTION        "Install desktop file, metainfo, and icons"       OFF)
option(QUADRATURE_USE_LIBPQ    "Build the PostgreSQL MusicBrainz backend"        ON)
option(QUADRATURE_NATIVE_BUILD "Tune for the local CPU; Release builds only"     OFF)
option(QUADRATURE_TEST_SERIAL  "Run Criterion in --jobs 1 (rate-limited HTTP)"   OFF)

set(QUADRATURE_BUILD_MODE "DEBUG" CACHE STRING "Feature gating: DEBUG or BROADCAST")
set_property(CACHE QUADRATURE_BUILD_MODE PROPERTY STRINGS "DEBUG" "BROADCAST")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(QUADRATURE_NATIVE_BUILD AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING "QUADRATURE_NATIVE_BUILD has no effect outside Release (current: ${CMAKE_BUILD_TYPE})")
endif()

message(STATUS "Quadrature ${PROJECT_VERSION} — ${CMAKE_BUILD_TYPE} (${QUADRATURE_BUILD_MODE})")

# === Compiler ===============================================================

add_compile_options(
    -Wall -Wextra
    -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
)
add_compile_definitions(_GNU_SOURCE)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-ftree-vectorize)
    if(QUADRATURE_NATIVE_BUILD)
        add_compile_options(-march=native -mtune=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        add_compile_options(-march=x86-64-v3 -mtune=generic)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        add_compile_options(-march=armv8-a -mtune=generic)
    endif()
    # LTO when the toolchain supports it; defensively check to avoid hard
    # errors on compilers that don't (e.g. some cross toolchains).
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _IPO_OK OUTPUT _IPO_ERR)
    if(_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "LTO unavailable: ${_IPO_ERR}")
    endif()
endif()

if(QUADRATURE_BUILD_MODE STREQUAL "BROADCAST")
    add_compile_definitions(QUADRATURE_BUILD_BROADCAST)
else()
    add_compile_definitions(QUADRATURE_BUILD_DEBUG)
endif()

# === Dependencies ===========================================================

find_package(PkgConfig REQUIRED)
find_package(Threads  REQUIRED)
find_package(OpenMP   REQUIRED)

pkg_check_modules(GLIB        REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(FFMPEG      REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample libswscale libavfilter)
pkg_check_modules(SQLITE3     REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(PIPEWIRE    REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFTW3       REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(VIPS        REQUIRED IMPORTED_TARGET vips)
pkg_check_modules(RUBBERBAND  REQUIRED IMPORTED_TARGET rubberband>=3.0)
pkg_check_modules(CHROMAPRINT REQUIRED IMPORTED_TARGET libchromaprint)
pkg_check_modules(TAGLIB      REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(LIBSOUP3    REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(JSON_GLIB   REQUIRED IMPORTED_TARGET json-glib-1.0)

if(QUADRATURE_USE_LIBPQ)
    pkg_check_modules(LIBPQ REQUIRED IMPORTED_TARGET libpq)
endif()

# GTK4 is needed by the UI binary and by tests that compile UI sources.
if(BUILD_UI OR BUILD_TESTS)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
endif()

# Application identifiers
set(ACOUSTID_APPLICATION_KEY  "N3PlLWjzbn"                       CACHE STRING "")
set(FANART_TV_APPLICATION_KEY "400cfe22b47e1fb93bfeb3ca452059c8" CACHE STRING "")
set(MUSICBRAINZ_USER_AGENT
    "quadrature/${PROJECT_VERSION} ( ${PROJECT_HOMEPAGE_URL} )" CACHE STRING "")

add_compile_definitions(
    ACOUSTID_APPLICATION_KEY="${ACOUSTID_APPLICATION_KEY}"
    FANART_TV_APPLICATION_KEY="${FANART_TV_APPLICATION_KEY}"
    MUSICBRAINZ_USER_AGENT="${MUSICBRAINZ_USER_AGENT}"
    QUADRATURE_VERSION="${PROJECT_VERSION}"
)

# === Public headers (INTERFACE) =============================================

add_library(quadrature_headers INTERFACE)
target_include_directories(quadrature_headers INTERFACE ${CMAKE_SOURCE_DIR}/include)

# === quadrature_engine ======================================================

add_library(quadrature_engine STATIC
    src/audio/audio_pipeline.c
    src/audio/audio_cache.c
    src/audio/audio_scrub.c
    src/audio/audio_spectrum.c
    src/audio/audio_devices.c
    src/audio/cavacore.c
    src/core/perf_dashboard.c
    src/database/db.c
    src/database/db_read.c
    src/database/db_write.c
    src/database/reconciler.c
    src/database/db_meta.c
    src/database/db_bios.c
    src/database/migrations/001_initial.c
    src/library/library_cache.c
    src/library/library_mask.c
    src/library/cache_search.c
    src/library/library_search.c
)
target_link_libraries(quadrature_engine PUBLIC
    quadrature_headers
    PkgConfig::GLIB
    PkgConfig::FFMPEG
    PkgConfig::SQLITE3
    PkgConfig::PIPEWIRE
    PkgConfig::FFTW3
    PkgConfig::RUBBERBAND
    Threads::Threads
    m
)

# === quadrature_indexer_core ================================================

add_library(quadrature_indexer_core STATIC
    src/indexer/indexer.c
    src/indexer/utils.c
    src/indexer/library_validation.c
    src/indexer/artwork.c
    src/indexer/artist_art.c
    src/indexer/artist_atlas.c
    src/indexer/artist_bio.c
    src/indexer/change_tracker.c
    src/indexer/musicbrainz/mb_fingerprint.c
    src/indexer/musicbrainz/mb_backend.c
    src/indexer/musicbrainz/mb_resolver.c
    src/indexer/musicbrainz/mb_http_backend.c
    src/indexer/musicbrainz/mb_http_ops.c
)
target_include_directories(quadrature_indexer_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src/indexer
    ${CMAKE_SOURCE_DIR}/src/indexer/musicbrainz
    ${CMAKE_SOURCE_DIR}/src/database
)
target_link_libraries(quadrature_indexer_core PUBLIC
    quadrature_engine
    OpenMP::OpenMP_C
    PkgConfig::VIPS
    PkgConfig::CHROMAPRINT
    PkgConfig::TAGLIB
    PkgConfig::LIBSOUP3
    PkgConfig::JSON_GLIB
)

if(QUADRATURE_USE_LIBPQ)
    target_sources(quadrature_indexer_core PRIVATE
        src/indexer/musicbrainz/mb_pg_client.c
        src/indexer/musicbrainz/mb_pg_acoustid.c
        src/indexer/musicbrainz/mb_pg_backend.c
    )
    target_compile_definitions(quadrature_indexer_core PUBLIC QUADRATURE_USE_LIBPQ)
    # libanl: getaddrinfo_a in mb_pg_acoustid.c. Being folded into glibc 2.41+;
    # replace with c-ares or thread-pool resolver when targeting newer glibcs.
    target_link_libraries(quadrature_indexer_core PRIVATE PkgConfig::LIBPQ anl)
endif()

# === quadrature_gpio ========================================================

add_library(quadrature_gpio STATIC
    src/gpio/axia_gpio.c
    src/gpio/axia_protocol.c
)
target_include_directories(quadrature_gpio PUBLIC ${CMAKE_SOURCE_DIR}/src/gpio)
target_link_libraries(quadrature_gpio PUBLIC quadrature_headers PkgConfig::GLIB Threads::Threads)

# === quadrature-cli =========================================================

if(BUILD_CLI)
    add_executable(quadrature-cli
        src/cli/main.c
        src/cli/cli_indexer.c
        src/cli/cli_setup_rt.c
        src/indexer/indexer_daemon.c
        src/indexer/indexer_config.c
        src/indexer/indexer_watcher.c
    )
    target_include_directories(quadrature-cli PRIVATE ${CMAKE_SOURCE_DIR}/src/cli)
    target_link_libraries(quadrature-cli PRIVATE quadrature_indexer_core)
endif()

# === quadrature (GTK4 UI) ===================================================

if(BUILD_UI)
    pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
    find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

    file(GLOB CSS_SOURCES   CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/css/*.css)
    file(GLOB UI_TEMPLATES  CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/templates/*.ui)
    set(CSS_COMBINED  ${CMAKE_BINARY_DIR}/quadrature.css)
    set(GRESOURCE_XML ${CMAKE_SOURCE_DIR}/src/ui/templates/quadrature.gresource.xml)
    set(GRESOURCE_C   ${CMAKE_BINARY_DIR}/quadrature-resources.c)

    add_custom_command(
        OUTPUT ${CSS_COMBINED}
        COMMAND ${CMAKE_COMMAND} -E cat ${CSS_SOURCES} > ${CSS_COMBINED}
        DEPENDS ${CSS_SOURCES}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${GRESOURCE_C}
        COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_BINARY_DIR}
            --sourcedir=${CMAKE_SOURCE_DIR}/src/ui/templates
            --generate-source
            ${GRESOURCE_XML}
        DEPENDS ${GRESOURCE_XML} ${UI_TEMPLATES} ${CSS_COMBINED}
        VERBATIM
    )

    add_executable(quadrature
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
    target_include_directories(quadrature PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ui
        ${CMAKE_SOURCE_DIR}/src/ui/library
        ${CMAKE_SOURCE_DIR}/src/ui/perf
        ${CMAKE_SOURCE_DIR}/src/ui/search
        ${CMAKE_SOURCE_DIR}/src/ui/libraries
        ${CMAKE_SOURCE_DIR}/src/ui/settings
    )
    target_link_libraries(quadrature PRIVATE
        quadrature_indexer_core
        quadrature_gpio
        PkgConfig::GTK4
        PkgConfig::LIBADWAITA
    )
endif()

# === Tests ==================================================================

if(BUILD_TESTS)
    enable_testing()
    pkg_check_modules(CRITERION REQUIRED IMPORTED_TARGET criterion)

    function(unit_test NAME)
        cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
        add_executable(${NAME} tests/unit/${NAME}.c ${T_SOURCES})
        target_include_directories(${NAME} PRIVATE ${T_INCLUDES})
        target_link_libraries(${NAME} PRIVATE PkgConfig::CRITERION ${T_LIBS})
        target_compile_definitions(${NAME} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        if(QUADRATURE_TEST_SERIAL)
            add_test(NAME ${NAME} COMMAND ${NAME} --jobs 1)  # Dont spam musicbrainz/acoustid
        else()
            add_test(NAME ${NAME} COMMAND ${NAME})
        endif()
    endfunction()

    set(S ${CMAKE_SOURCE_DIR}/src)

    unit_test(test_overflow_layout SOURCES ${S}/ui/overflow_plan.c
        LIBS quadrature_headers PkgConfig::GTK4)
    unit_test(test_library_mask    SOURCES ${S}/library/library_mask.c
        LIBS quadrature_headers PkgConfig::GLIB)

    unit_test(test_database         INCLUDES ${S}/database LIBS quadrature_engine)
    unit_test(test_library_cache                           LIBS quadrature_engine)
    unit_test(test_path_resolution                         LIBS quadrature_engine)

    unit_test(test_audio_pipeline INCLUDES ${S}/audio ${S}/database LIBS quadrature_engine)
    unit_test(test_audio_cache    INCLUDES ${S}/audio ${S}/database LIBS quadrature_engine)
    unit_test(test_audio_scrubber INCLUDES ${S}/audio ${S}/database LIBS quadrature_engine)

    unit_test(test_indexer             INCLUDES ${S}/indexer ${S}/database LIBS quadrature_indexer_core)
    unit_test(test_indexer_integration INCLUDES ${S}/indexer ${S}/database LIBS quadrature_indexer_core quadrature_engine)
    unit_test(test_reconciler          INCLUDES ${S}/database              LIBS quadrature_engine)
    unit_test(test_multi_disc          INCLUDES ${S}/indexer ${S}/database LIBS quadrature_indexer_core)

    unit_test(test_mb_resolve INCLUDES ${S}/indexer/musicbrainz ${S}/database
        LIBS quadrature_indexer_core quadrature_engine)

    unit_test(test_e2e_library_resolution INCLUDES ${S}/indexer ${S}/indexer/musicbrainz ${S}/database
        LIBS quadrature_indexer_core quadrature_engine)
    unit_test(test_metadata_search        INCLUDES ${S}/indexer ${S}/indexer/musicbrainz ${S}/database
        LIBS quadrature_indexer_core quadrature_engine)

    unit_test(test_axia_gpio INCLUDES ${S}/gpio
        LIBS quadrature_gpio PkgConfig::GLIB Threads::Threads)
endif()

# === Install ================================================================

include(GNUInstallDirs)

if(BUILD_CLI)
    install(TARGETS quadrature-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_UI)
    install(TARGETS quadrature RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${CMAKE_BINARY_DIR}/quadrature.css
            DESTINATION ${CMAKE_INSTALL_DATADIR}/quadrature)

    if(BUILD_PRODUCTION)
        configure_file(
            ${CMAKE_SOURCE_DIR}/data/org.quadrature.player.desktop.in
            ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop @ONLY)
        install(FILES ${CMAKE_BINARY_DIR}/org.quadrature.player.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES ${CMAKE_SOURCE_DIR}/data/org.quadrature.player.metainfo.xml
                DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
        install(DIRECTORY ${CMAKE_SOURCE_DIR}/data/icons/
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons
                FILES_MATCHING PATTERN "*.png" PATTERN "*.svg")
    endif()
endif()
