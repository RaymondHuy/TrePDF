qt_add_executable(trepdf_edit_controller_tests
    tst_PDFEditController.cpp
    ../src/PDFDocument.cpp
    ../src/PDFDocument.h
    ../src/PDFEditController.cpp
    ../src/PDFEditController.h
    ../src/PDFiumSynchronization.h
)

target_include_directories(trepdf_edit_controller_tests
    PRIVATE
        ../src
)

target_link_libraries(trepdf_edit_controller_tests
    PRIVATE
        Qt6::Gui
        Qt6::Qml
        Qt6::Test
        PDFium::PDFium
)

if(APPLE)
    get_target_property(TREPDF_TEST_PDFIUM_LIBRARY PDFium::PDFium IMPORTED_LOCATION)
    add_custom_command(TARGET trepdf_edit_controller_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_TEST_PDFIUM_LIBRARY}"
            "$<TARGET_FILE_DIR:trepdf_edit_controller_tests>/libpdfium.dylib"
        COMMAND install_name_tool -change
            ./libpdfium.dylib
            @loader_path/libpdfium.dylib
            "$<TARGET_FILE:trepdf_edit_controller_tests>"
        VERBATIM
    )
endif()

add_test(
    NAME PDFEditControllerTests
    COMMAND trepdf_edit_controller_tests
)
