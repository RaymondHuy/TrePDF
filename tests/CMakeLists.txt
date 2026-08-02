qt_add_executable(trepdf_edit_controller_tests
    tst_PDFEditController.cpp
    ../src/ApplicationFonts.cpp
    ../src/ApplicationFonts.h
    ../src/DraftImageItem.cpp
    ../src/DraftImageItem.h
    ../src/FormFieldLayer.cpp
    ../src/FormFieldLayer.h
    ../src/PDFAcroFormCanonicalizer.cpp
    ../src/PDFAcroFormCanonicalizer.h
    ../src/PDFAcroFormWriter.cpp
    ../src/PDFAcroFormWriter.h
    ../src/PDFDocument.cpp
    ../src/PDFDocument.h
    ../src/PDFEditController.cpp
    ../src/PDFEditController.h
    ../src/PDFFormController.cpp
    ../src/PDFFormController.h
    ../src/PDFFormAppearance.h
    ../src/PDFFormDesignerController.cpp
    ../src/PDFFormDesignerController.h
    ../src/PDFFormView.cpp
    ../src/PDFFormView.h
    ../src/PDFFormWorker.cpp
    ../src/PDFFormWorker.h
    ../src/PDFSaveWriter.cpp
    ../src/PDFSaveWriter.h
    ../src/PDFTabModel.cpp
    ../src/PDFTabModel.h
    ../src/PDFiumSynchronization.h
    ../src/PDFWidgetAnnotationSuppression.cpp
    ../src/PDFWidgetAnnotationSuppression.h
)

if(TREPDF_BUNDLED_FONT_FILES)
    qt_add_resources(trepdf_edit_controller_tests "trepdf_test_fonts"
        PREFIX "/trepdf/fonts"
        BASE "${CMAKE_SOURCE_DIR}/src/assets/fonts"
        FILES ${TREPDF_BUNDLED_FONT_FILES}
    )
endif()

target_include_directories(trepdf_edit_controller_tests
    PRIVATE
        ../src
)

target_link_libraries(trepdf_edit_controller_tests
    PRIVATE
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
        PDFium::PDFium
)

if(APPLE)
    target_link_libraries(trepdf_edit_controller_tests
        PRIVATE
            "${TREPDF_CORE_FOUNDATION_FRAMEWORK}"
            "${TREPDF_CORE_TEXT_FRAMEWORK}"
    )

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
