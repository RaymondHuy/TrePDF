# Keep the Windows executable runnable from CLion without changing global PATH.
function(trepdf_deploy_windows_runtime target)
    if(NOT WIN32)
        return()
    endif()

    set(_runtime_libraries)
    foreach(_dependency IN ITEMS PDFium::PDFium QPDF::QPDF)
        get_target_property(_runtime_library ${_dependency} IMPORTED_LOCATION)
        get_filename_component(_runtime_directory "${_runtime_library}" DIRECTORY)
        file(GLOB _package_libraries "${_runtime_directory}/*.dll")
        list(APPEND _runtime_libraries "${_runtime_library}" ${_package_libraries})
    endforeach()
    list(REMOVE_DUPLICATES _runtime_libraries)
    # Let Qt deploy its matching MinGW runtime. qpdf's portable package also
    # ships compiler DLLs, which must not overwrite the Qt toolchain's copies.
    list(FILTER _runtime_libraries EXCLUDE REGEX
        "/(libgcc_s_[^/]+|libstdc\\+\\+-6|libwinpthread-1)\\.dll$")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_runtime_libraries} "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::qmake>;$ENV{PATH}"
            "$<TARGET_FILE_DIR:Qt6::qmake>/windeployqt.exe"
            --qtpaths "$<TARGET_FILE_DIR:Qt6::qmake>/qtpaths.exe"
            --qmldir "${PROJECT_SOURCE_DIR}/src"
            --no-translations
            --compiler-runtime
            "$<TARGET_FILE:${target}>"
        COMMENT "Deploying Windows PDF and Qt runtime dependencies"
        VERBATIM
    )
endfunction()
