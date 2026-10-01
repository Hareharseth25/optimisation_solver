# Runs at install time after macdeployqt (KAIRO_DESKTOP_DEPLOY=ON, macOS).
#
# macdeployqt copies the Qt frameworks and their dependencies into the bundle
# and rewrites every reference between them to @executable_path/..., so
# nothing is loaded from the build machine's Qt prefix. Two inert traces of
# that prefix remain: the copied libraries' own install names (LC_ID_DYLIB)
# and absolute LC_RPATH entries some third-party libraries were built with
# (e.g. a Homebrew Cellar path). Neither is used when loading, but a bundle
# should not name the build machine at all, so both are removed here and the
# bundle is re-signed ad hoc (arm64 refuses to run modified unsigned code).
#
# Expects BUNDLE (absolute path to KAIRO.app).

if(NOT BUNDLE OR NOT IS_DIRECTORY "${BUNDLE}")
    message(FATAL_ERROR "finalize_bundle: BUNDLE not found: ${BUNDLE}")
endif()

file(GLOB_RECURSE candidates LIST_DIRECTORIES false "${BUNDLE}/Contents/*")
foreach(file IN LISTS candidates)
    if(IS_SYMLINK "${file}")
        continue()
    endif()
    execute_process(COMMAND file -b "${file}" OUTPUT_VARIABLE kind OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT kind MATCHES "Mach-O")
        continue()
    endif()
    execute_process(COMMAND otool -l "${file}" OUTPUT_VARIABLE loads)
    # Absolute rpaths outside the bundle.
    string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n[ ]*path ([^ \n]+)" rpaths "${loads}")
    foreach(entry IN LISTS rpaths)
        string(REGEX REPLACE ".*path ([^ \n]+)$" "\\1" rpath "${entry}")
        if(rpath MATCHES "^/")
            execute_process(COMMAND install_name_tool -delete_rpath "${rpath}" "${file}" ERROR_QUIET)
        endif()
    endforeach()
    # A library's own install name: make it bundle-relative.
    execute_process(COMMAND otool -D "${file}" OUTPUT_VARIABLE id_out OUTPUT_STRIP_TRAILING_WHITESPACE)
    # otool -D prints "<file>:" then the install name on the next line.
    set(id "")
    if(id_out MATCHES "\n([^\n]+)$")
        set(id "${CMAKE_MATCH_1}")
    endif()
    if(id MATCHES "^/")
        file(RELATIVE_PATH relative "${BUNDLE}/Contents/Frameworks" "${file}")
        execute_process(COMMAND install_name_tool -id "@rpath/${relative}" "${file}" ERROR_QUIET)
    endif()
endforeach()

execute_process(COMMAND codesign --force --deep --sign - "${BUNDLE}" RESULT_VARIABLE signed ERROR_VARIABLE sign_error)
if(NOT signed EQUAL 0)
    message(FATAL_ERROR "finalize_bundle: ad hoc codesign failed: ${sign_error}")
endif()
message(STATUS "KAIRO.app finalized: no build-machine paths, ad hoc signed")
