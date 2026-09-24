qt_add_executable(trepdf_edit_controller_tests
    tst_PDFEditController.cpp
    PDFEditControllerTest.h
    PDFEditControllerTestSupport.h
    PDFEditControllerTestSupport.cpp
    PDFEditControllerTestSupportAnnotations.cpp
    PDFEditControllerTestSupportCommentFixtures.cpp
    PDFEditControllerTestSupportForms.cpp
    PDFEditControllerTestSupportPdfInspection.cpp
    PDFEditControllerTestSupportText.cpp
    tst_PDFEditControllerCommentPersistence.cpp
    tst_PDFEditControllerCommentState.cpp
    tst_PDFEditControllerCommentThreads.cpp
    tst_PDFEditControllerComments.cpp
    tst_PDFEditControllerDocument.cpp
    tst_PDFEditControllerDocumentView.cpp
    tst_PDFEditControllerDrawingComments.cpp
    tst_PDFEditControllerFonts.cpp
    tst_PDFEditControllerFormButtons.cpp
    tst_PDFEditControllerFormChoices.cpp
    tst_PDFEditControllerFormTextPersistence.cpp
    tst_PDFEditControllerForms.cpp
    tst_PDFEditControllerImages.cpp
    tst_PDFEditControllerInk.cpp
    tst_PDFEditControllerParagraphs.cpp
    tst_PDFEditControllerProtection.cpp
    tst_PDFEditControllerRendering.cpp
    tst_PDFEditControllerSearch.cpp
    tst_PDFEditControllerSessions.cpp
    tst_PDFEditControllerSignatures.cpp
    tst_PDFEditControllerShapes.cpp
    tst_PDFEditControllerTextFormatting.cpp
    tst_PDFEditControllerTextMarkup.cpp
    tst_PDFEditControllerTextPersistence.cpp
    tst_PDFEditControllerTextSpacing.cpp
    tst_PDFEditControllerWebLinks.cpp
    ../src/ApplicationFonts.cpp
    ../src/ApplicationFonts.h
    ../src/FontDownloadService.cpp
    ../src/FontDownloadService.h
    ../src/IFontProvider.h
    ../src/ReviewedCtanFonts.h
    ../src/PDFFontAliases.cpp
    ../src/PDFFontAliases.h
    ../src/GoogleFontsProvider.cpp
    ../src/GoogleFontsProvider.h
    ../src/CtanFontProvider.cpp
    ../src/CtanFontProvider.h
    ../src/DraftImageItem.cpp
    ../src/DraftImageItem.h
    ../src/FormFieldLayer.cpp
    ../src/FormFieldLayer.h
    ../src/PDFAcroFormCanonicalizer.cpp
    ../src/PDFAcroFormCanonicalizer.h
    ../src/PDFAcroFormWriter.cpp
    ../src/PDFAcroFormWriter.h
    ../src/PDFAnnotationUtils.cpp
    ../src/PDFAnnotationUtils.h
    ../src/PDFDocument.cpp
    ../src/PDFDocument.h
    ../src/LibxlsxwriterWorkbook.cpp
    ../src/SpreadsheetWorkbook.h
    ../src/SignatureLibrary.cpp
    ../src/SignatureLibrary.h
    ../src/PDFExcelExportController.cpp
    ../src/PDFExcelExportController.h
    ../src/PDFTableExtractor.cpp
    ../src/PDFTableExtractor.h
    ../src/PDFTableLayout.h
    ../src/PDFEditController.cpp
    ../src/PDFEditController.h
    ../src/PDFEditControllerCandidates.cpp
    ../src/PDFEditControllerAnnotationScan.cpp
    ../src/PDFEditControllerText.cpp
    ../src/PDFEditControllerComments.cpp
    ../src/PDFEditControllerGeometry.cpp
    ../src/PDFEditControllerDrafts.cpp
    ../src/PDFEditControllerSave.cpp
    ../src/PDFEditControllerWebLinks.cpp
    ../src/PDFEditControllerManualLinks.cpp
    ../src/PDFEditControllerSupport.cpp
    ../src/PDFEditController_p.h
    ../src/PDFFormController.cpp
    ../src/PDFFormController.h
    ../src/PDFFormAppearance.h
    ../src/PDFFormDesignerController.cpp
    ../src/PDFFormDesignerController.h
    ../src/PDFFormView.cpp
    ../src/PDFFormView.h
    ../src/PDFFormWorker.cpp
    ../src/PDFFormWorker.h
    ../src/PDFPageItem.cpp
    ../src/PDFPageItem.h
    ../src/PDFRasterBudget.cpp
    ../src/PDFRasterBudget.h
    ../src/PDFRasterPlan.h
    ../src/PDFSaveWriter.cpp
    ../src/PDFSaveWriter.h
    ../src/PDFShapeUtils.h
    ../src/PDFSearchController.cpp
    ../src/PDFSearchController.h
    ../src/PDFTabModel.cpp
    ../src/PDFTabModel.h
    ../src/PDFiumSynchronization.h
    ../src/PDFWebLinkScanner.cpp
    ../src/PDFWebLinkScanner.h
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
        Qt6::Network
        trepdf_font_archive
        trepdf_password_writer
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
        PDFium::PDFium
        Libxlsxwriter::Libxlsxwriter
)

if(APPLE)
    target_link_libraries(trepdf_edit_controller_tests
        PRIVATE
            "${TREPDF_CORE_FOUNDATION_FRAMEWORK}"
            "${TREPDF_CORE_TEXT_FRAMEWORK}"
    )

    get_target_property(TREPDF_TEST_PDFIUM_LIBRARY PDFium::PDFium IMPORTED_LOCATION)
    get_target_property(TREPDF_TEST_QPDF_LIBRARY QPDF::QPDF IMPORTED_LOCATION)
    add_custom_command(TARGET trepdf_edit_controller_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_TEST_PDFIUM_LIBRARY}"
            "$<TARGET_FILE_DIR:trepdf_edit_controller_tests>/libpdfium.dylib"
        COMMAND install_name_tool -change
            ./libpdfium.dylib
            @loader_path/libpdfium.dylib
            "$<TARGET_FILE:trepdf_edit_controller_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_TEST_QPDF_LIBRARY}"
            "$<TARGET_FILE_DIR:trepdf_edit_controller_tests>/libqpdf.dylib"
        COMMAND install_name_tool -change
            @rpath/libqpdf.dylib
            @loader_path/libqpdf.dylib
            "$<TARGET_FILE:trepdf_edit_controller_tests>"
        VERBATIM
    )
endif()

add_test(
    NAME PDFEditControllerTests
    COMMAND trepdf_edit_controller_tests
)

qt_add_executable(trepdf_excel_export_tests
    tst_PDFExcelExport.cpp
    ../src/PDFDocument.cpp
    ../src/PDFDocument.h
    ../src/PDFExcelExportController.cpp
    ../src/PDFExcelExportController.h
    ../src/PDFTableExtractor.cpp
    ../src/PDFTableExtractor.h
    ../src/PDFTableLayout.h
    ../src/LibxlsxwriterWorkbook.cpp
    ../src/SpreadsheetWorkbook.h
)
target_include_directories(trepdf_excel_export_tests PRIVATE ../src)
target_link_libraries(trepdf_excel_export_tests PRIVATE
    Qt6::Gui Qt6::Qml Qt6::Test PDFium::PDFium Libxlsxwriter::Libxlsxwriter
)
if(APPLE)
    add_custom_command(TARGET trepdf_excel_export_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_TEST_PDFIUM_LIBRARY}"
            "$<TARGET_FILE_DIR:trepdf_excel_export_tests>/libpdfium.dylib"
        COMMAND install_name_tool -change ./libpdfium.dylib
            @loader_path/libpdfium.dylib "$<TARGET_FILE:trepdf_excel_export_tests>"
        VERBATIM
    )
endif()
add_test(NAME PDFExcelExportTests COMMAND trepdf_excel_export_tests)
set_tests_properties(PDFExcelExportTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(trepdf_spreadsheet_workbook_tests
    tst_SpreadsheetWorkbook.cpp
    ../src/LibxlsxwriterWorkbook.cpp
    ../src/SpreadsheetWorkbook.h
)
target_include_directories(trepdf_spreadsheet_workbook_tests PRIVATE ../src)
target_link_libraries(trepdf_spreadsheet_workbook_tests PRIVATE
    Qt6::Core Qt6::Test Libxlsxwriter::Libxlsxwriter
)
add_test(NAME SpreadsheetWorkbookTests COMMAND trepdf_spreadsheet_workbook_tests)
