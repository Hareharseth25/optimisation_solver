# KAIRO Desktop: native Qt 6 front end to KAIRO Core (macOS, Windows, Linux).
#
# Talks to the solver in-process through solver_orchestrator / mps_reader /
# solve_report_json -- no CLI, no HTTP, no network. Included from the
# top-level CMakeLists.txt only when Qt 6 is found.

qt_standard_project_setup()

# Interpretation of an optimsolver.solve.v1 record (Qt Core only, no widgets).
add_library(kairo_desktop_record STATIC
    src/record/Comparison.cpp
    src/record/Format.cpp
    src/record/RecordReaders.cpp
)
target_include_directories(kairo_desktop_record PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kairo_desktop_record PUBLIC Qt6::Core)

# The bridge to KAIRO Core, and local run persistence.
add_library(kairo_desktop_session STATIC
    src/core/KairoSession.cpp
    src/history/RunStore.cpp
)
target_include_directories(kairo_desktop_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kairo_desktop_session PUBLIC
    kairo_desktop_record
    solver_orchestrator
    mps_reader
    solve_report_json
    Qt6::Core
)

set(KAIRO_DESKTOP_VERSION 0.1.0)

# The widgets: the application and the widget tests both link this.
add_library(kairo_desktop_ui STATIC
    src/ui/ComparisonView.cpp
    src/ui/ComparisonView.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ReportView.cpp
    src/ui/ReportView.h
    src/ui/Theme.cpp
    src/ui/Theme.h
)
target_link_libraries(kairo_desktop_ui PUBLIC kairo_desktop_session Qt6::Widgets Qt6::Concurrent)

# Platform icons: .icns in the macOS bundle, .ico as a Windows resource, and
# the PNG as the window icon everywhere (Linux also installs the SVG).
set(KAIRO_PLATFORM_SOURCES)
if(APPLE)
    set(KAIRO_PLATFORM_SOURCES packaging/macos/KAIRO.icns)
    set_source_files_properties(packaging/macos/KAIRO.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(WIN32)
    set(KAIRO_PLATFORM_SOURCES packaging/windows/KAIRO.rc)
endif()
qt_add_executable(KAIRO WIN32 MACOSX_BUNDLE src/main.cpp ${KAIRO_PLATFORM_SOURCES})
qt_add_resources(KAIRO kairo_icons PREFIX /icons BASE packaging/icons FILES packaging/icons/org.kairo.desktop.png)
target_link_libraries(KAIRO PRIVATE kairo_desktop_ui)
target_compile_definitions(KAIRO PRIVATE KAIRO_DESKTOP_VERSION="${KAIRO_DESKTOP_VERSION}")
set_target_properties(KAIRO PROPERTIES
    OUTPUT_NAME KAIRO
    MACOSX_BUNDLE_BUNDLE_NAME KAIRO
    MACOSX_BUNDLE_GUI_IDENTIFIER org.kairo.desktop
    MACOSX_BUNDLE_BUNDLE_VERSION ${KAIRO_DESKTOP_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${KAIRO_DESKTOP_VERSION}
    MACOSX_BUNDLE_ICON_FILE KAIRO.icns
)
if(UNIX AND NOT APPLE)
    # The installed binary finds the deployed Qt libraries in ../lib.
    set_target_properties(KAIRO PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()
foreach(target kairo_desktop_record kairo_desktop_session kairo_desktop_ui KAIRO)
    if(MSVC)
        # Sources are UTF-8 (record wording uses "—", "·", "µs").
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# ---------------------------------------------------------------- install / packaging
#
#   cmake --install <build> --prefix <dir>
#
# installs the application: KAIRO.app on macOS, bin/KAIRO(.exe) elsewhere,
# plus the freedesktop entry on Linux. With -DKAIRO_DESKTOP_DEPLOY=ON the
# install also copies the Qt runtime next to it using Qt's own deployment
# (macdeployqt on macOS, windeployqt on Windows, Qt's runtime-dependency
# deployment on Linux; on Windows the plugins go into bin/ next to KAIRO.exe),
# so the result does not depend on where Qt was
# installed on the build machine (e.g. a Homebrew prefix).
include(GNUInstallDirs)
install(TARGETS KAIRO
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/org.kairo.desktop.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/icons/org.kairo.desktop.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
if(WIN32)
    # The MSVC runtime DLLs next to KAIRO.exe, so no separate redistributable
    # installer is needed.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
endif()
option(KAIRO_DESKTOP_DEPLOY "Copy the Qt runtime into the installed KAIRO Desktop application" OFF)
if(KAIRO_DESKTOP_DEPLOY)
    if(WIN32)
        # windeployqt with the plugins next to KAIRO.exe (bin/platforms/qwindows.dll,
        # bin/styles/, ...), so bin/ alone is the complete application and is what
        # the zip ships. Qt's default puts them in a sibling plugins/ directory,
        # which a zip of bin/ leaves out. Qt's .qm translations are not deployed:
        # KAIRO installs no QTranslator, so they are never loaded.
        qt_generate_deploy_script(
            TARGET KAIRO
            OUTPUT_SCRIPT kairo_deploy_script
            CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE $<TARGET_FILE:KAIRO>
    GENERATE_QT_CONF
    PLUGINS_DIR ${CMAKE_INSTALL_BINDIR}
    NO_TRANSLATIONS
)
")
    else()
        qt_generate_deploy_app_script(
            TARGET KAIRO
            OUTPUT_SCRIPT kairo_deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
    endif()
    install(SCRIPT ${kairo_deploy_script})
    if(APPLE)
        install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\"
            -DBUNDLE=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KAIRO.app
            -P \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/finalize_bundle.cmake\"
            COMMAND_ERROR_IS_FATAL ANY)")
    endif()
endif()

# ---------------------------------------------------------------- tests
if(BUILD_TESTING OR NOT DEFINED BUILD_TESTING)
    find_package(Qt6 COMPONENTS Test QUIET)
    if(TARGET Qt6::Test)
        qt_add_executable(kairo_desktop_tests tests/test_desktop.cpp)
        target_link_libraries(kairo_desktop_tests PRIVATE kairo_desktop_ui Qt6::Test)
        if(MSVC)
            target_compile_options(kairo_desktop_tests PRIVATE /utf-8)
        endif()
        target_compile_definitions(kairo_desktop_tests PRIVATE
            KAIRO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        add_test(NAME desktop_readers COMMAND kairo_desktop_tests)
        set_tests_properties(desktop_readers PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    # The real application, end to end, without a display: solve a model,
    # render every section, save a screenshot.
    add_test(NAME desktop_capture_lp
        COMMAND KAIRO -platform offscreen --capture ${PROJECT_SOURCE_DIR}/tests/cli/presolve_reduction.mps
                --out ${CMAKE_CURRENT_BINARY_DIR}/capture_lp.png
                --runs-dir ${CMAKE_CURRENT_BINARY_DIR}/capture-runs)
    set_tests_properties(desktop_capture_lp PROPERTIES
        PASS_REGULAR_EXPRESSION "pipeline\tmodel:completed classification:completed presolve:completed dispatch:completed engine:completed postsolve:completed validation:completed result:completed")

    # Export -> import -> compare through the real application. The two
    # exports solve the same MILP with branch and cut and with barrier (which
    # solves only the continuous relaxation).
    set(_knapsack ${PROJECT_SOURCE_DIR}/benchmarks/instances/known/milp_knapsack.mps)
    add_test(NAME desktop_export_runs
        COMMAND ${CMAKE_COMMAND}
            -DKAIRO=$<TARGET_FILE:KAIRO> -DMODEL=${_knapsack} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/records
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/export_runs.cmake)
    set_tests_properties(desktop_export_runs PROPERTIES FIXTURES_SETUP desktop_records)
    add_test(NAME desktop_import_record
        COMMAND KAIRO -platform offscreen --import ${CMAKE_CURRENT_BINARY_DIR}/records/barrier.json
                --out ${CMAKE_CURRENT_BINARY_DIR}/import.png)
    set_tests_properties(desktop_import_record PROPERTIES
        FIXTURES_REQUIRED desktop_records
        PASS_REGULAR_EXPRESSION "headline\tOptimal for the continuous relaxation")
    add_test(NAME desktop_compare_records
        COMMAND KAIRO -platform offscreen
                --compare ${CMAKE_CURRENT_BINARY_DIR}/records/auto.json
                --compare ${CMAKE_CURRENT_BINARY_DIR}/records/barrier.json
                --out ${CMAKE_CURRENT_BINARY_DIR}/compare.png)
    set_tests_properties(desktop_compare_records PROPERTIES
        FIXTURES_REQUIRED desktop_records
        PASS_REGULAR_EXPRESSION "same_model\tyes.*row\toutcome\tIntegrality respected\tyes\tno"
        FAIL_REGULAR_EXPRESSION "[Bb]etter|[Ww]inner|[Bb]est ")
endif()
