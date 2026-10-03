cmake_minimum_required(VERSION 3.21)

set(TREPDF_VERSION "0.1.0" CACHE STRING "TrePDF application version (major.minor.patch)")
if(NOT TREPDF_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "TREPDF_VERSION must use the numeric major.minor.patch format")
endif()

project(TrePDF
    VERSION ${TREPDF_VERSION}
    LANGUAGES C CXX
)

option(TREPDF_DEPLOY_RUNTIME "Install Qt, QML and PDF runtime dependencies for distribution" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(TREPDF_LOCAL_QT_PREFIX "/Users/luongvanhuy/Qt/6.11.0/macos")
set(TREPDF_LOCAL_QT6_DIR "${TREPDF_LOCAL_QT_PREFIX}/lib/cmake/Qt6")
if((NOT Qt6_DIR OR Qt6_DIR MATCHES "-NOTFOUND$") AND EXISTS "${TREPDF_LOCAL_QT6_DIR}/Qt6Config.cmake")
    set(Qt6_DIR "${TREPDF_LOCAL_QT6_DIR}" CACHE PATH "Qt6 package configuration directory" FORCE)
    list(PREPEND CMAKE_PREFIX_PATH "${TREPDF_LOCAL_QT_PREFIX}")
endif()

set(TREPDF_LOCAL_PDFIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium")
if((NOT PDFium_ROOT OR PDFium_ROOT MATCHES "-NOTFOUND$") AND EXISTS "${TREPDF_LOCAL_PDFIUM_ROOT}/include/fpdfview.h")
    set(PDFium_ROOT "${TREPDF_LOCAL_PDFIUM_ROOT}" CACHE PATH "PDFium installation prefix" FORCE)
endif()

set(TREPDF_LOCAL_QPDF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qpdf")
if(NOT QPDF_ROOT OR QPDF_ROOT MATCHES "-NOTFOUND$")
    if(DEFINED ENV{QPDF_ROOT} AND NOT "$ENV{QPDF_ROOT}" STREQUAL "")
        set(QPDF_ROOT "$ENV{QPDF_ROOT}" CACHE PATH "Prebuilt qpdf installation prefix" FORCE)
    else()
        set(QPDF_ROOT "${TREPDF_LOCAL_QPDF_ROOT}" CACHE PATH "Prebuilt qpdf installation prefix" FORCE)
    endif()
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
    LinguistTools
    Network
    Qml
    Quick
    QuickControls2
)
find_package(PDFium REQUIRED)
find_package(QPDF REQUIRED)

qt_standard_project_setup(
    REQUIRES 6.8
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES vi
)

# Keep third-party build options local and the backend free of extra runtime
# libraries. The vendored 1.2.4 source makes first-time/offline builds identical.
function(trepdf_add_spreadsheet_backend)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_FUZZERS OFF)
    set(USE_SYSTEM_MINIZIP OFF)
    set(USE_STANDARD_TMPFILE OFF)
    set(USE_OPENSSL_MD5 OFF)
    set(USE_MEM_FILE OFF)
    set(USE_FMEMOPEN OFF)
    set(USE_DTOA_LIBRARY ON)
    add_subdirectory(third_party/libxlsxwriter EXCLUDE_FROM_ALL)
    set_target_properties(xlsxwriter PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
    )
    add_library(Libxlsxwriter::Libxlsxwriter ALIAS xlsxwriter)
endfunction()
trepdf_add_spreadsheet_backend()

file(GLOB_RECURSE TREPDF_BUNDLED_FONT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.OTF"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.ttc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.TTC"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.TTF"
)

find_package(ZLIB REQUIRED)
qt_add_library(trepdf_password_writer STATIC
    src/PDFPasswordWriter.cpp
    src/PDFPasswordWriter.h
    src/PDFOpenTypeFontWriter.cpp
    src/PDFOpenTypeFontWriter.h
    src/PDFImageFormResources.cpp
    src/PDFImageFormResources.h
)
target_link_libraries(trepdf_password_writer PRIVATE QPDF::QPDF Qt6::Core ZLIB::ZLIB)

qt_add_library(trepdf_font_archive STATIC
    third_party/libxlsxwriter/third_party/minizip/unzip.c
    third_party/libxlsxwriter/third_party/minizip/ioapi.c
)
set_target_properties(trepdf_font_archive PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(trepdf_font_archive PUBLIC
    third_party/libxlsxwriter/third_party/minizip
)
# The public minizip headers include zlib.h, so consumers need its include path.
target_link_libraries(trepdf_font_archive PUBLIC ZLIB::ZLIB)

qt_add_executable(trepdf
    src/main.cpp
)
target_compile_definitions(trepdf PRIVATE TREPDF_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # Windows min/max macros conflict with standard C++ calls, including in
    # generated QML type registration and MOC sources.
    target_compile_definitions(trepdf PRIVATE NOMINMAX)
endif()

qt_add_resources(trepdf "trepdf_app_icon"
    PREFIX "/trepdf/branding"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/branding"
    FILES src/assets/branding/TrePDF-logo.png
)

if(APPLE)
    set(TREPDF_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/branding/TrePDF.icns")
    set_source_files_properties("${TREPDF_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(trepdf PRIVATE "${TREPDF_MACOS_ICON}")
    set_target_properties(trepdf PROPERTIES
        MACOSX_BUNDLE_ICON_FILE "TrePDF.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TrePDFInfo.plist.in"
    )
elseif(WIN32)
    enable_language(RC)
    configure_file(
        src/assets/branding/TrePDF.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/TrePDF.rc"
        @ONLY
    )
    # Keep C++ dependency include paths/definitions away from windres: its
    # preprocessor can split include paths containing spaces even with temp files.
    add_library(trepdf_windows_resources OBJECT "${CMAKE_CURRENT_BINARY_DIR}/TrePDF.rc")
    set_target_properties(trepdf_windows_resources PROPERTIES AUTOMOC OFF AUTOUIC OFF)
    if(MINGW AND CMAKE_HOST_WIN32)
        # windres' default pipe invokes a shell that splits compiler paths
        # containing spaces (for example, CLion's bundled MinGW directory).
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/TrePDF.rc" PROPERTIES
            COMPILE_OPTIONS "--use-temp-file"
        )
    endif()
    target_sources(trepdf PRIVATE $<TARGET_OBJECTS:trepdf_windows_resources>)
endif()

if(WIN32)
    set(TREPDF_EDIT_OVERLAY_QML src/Components/Windows/EditOverlay.qml)
else()
    set(TREPDF_EDIT_OVERLAY_QML src/Components/EditOverlay.qml)
endif()

qt_add_qml_module(trepdf
    URI TrePDF
    SOURCES
        src/ApplicationFonts.cpp
        src/ApplicationFonts.h
        src/WindowsSystemFonts.cpp
        src/WindowsSystemFonts.h
        src/FontDownloadService.cpp
        src/FontDownloadService.h
        src/IFontProvider.h
        src/ReviewedCtanFonts.h
        src/PDFFontAliases.cpp
        src/PDFFontAliases.h
        src/GoogleFontsProvider.cpp
        src/GoogleFontsProvider.h
        src/CtanFontProvider.cpp
        src/CtanFontProvider.h
        src/DraftImageItem.cpp
        src/DraftImageItem.h
        src/FormFieldLayer.cpp
        src/FormFieldLayer.h
        src/LibxlsxwriterWorkbook.cpp
        src/PDFAcroFormCanonicalizer.cpp
        src/PDFAcroFormCanonicalizer.h
        src/PDFAcroFormWriter.cpp
        src/PDFAcroFormWriter.h
        src/PDFAnnotationUtils.cpp
        src/PDFAnnotationUtils.h
        src/PDFCommentAnnotation.h
        src/PDFStampPresets.h
        src/PDFDocument.cpp
        src/PDFDocument.h
        src/PDFFileOpenHandler.cpp
        src/PDFFileOpenHandler.h
        src/PDFEditController.cpp
        src/PDFEditController.h
        src/PDFEditControllerCandidates.cpp
        src/PDFEditControllerAnnotationScan.cpp
        src/PDFEditControllerText.cpp
        src/PDFEditControllerComments.cpp
        src/PDFEditControllerGeometry.cpp
        src/PDFEditControllerDrafts.cpp
        src/PDFEditControllerSave.cpp
        src/PDFEditControllerWebLinks.cpp
        src/PDFEditControllerManualLinks.cpp
        src/PDFEditControllerWatermarks.cpp
        src/PDFWatermark.cpp
        src/PDFWatermark.h
        src/PDFWebLinkScanner.cpp
        src/PDFWebLinkScanner.h
        src/PDFEditControllerSupport.cpp
        src/PDFEditController_p.h
        src/PDFExcelExportController.cpp
        src/PDFExcelExportController.h
        src/PDFFormController.cpp
        src/PDFFormController.h
        src/PDFFormAppearance.h
        src/PDFFormDesignerController.cpp
        src/PDFFormDesignerController.h
        src/PDFFormView.cpp
        src/PDFFormView.h
        src/PDFFormWorker.cpp
        src/PDFFormWorker.h
        src/PDFSaveWriter.cpp
        src/PDFSaveWriter.h
        src/PDFAtomicSaveFile.cpp
        src/PDFAtomicSaveFile.h
        src/PDFShapeUtils.h
        src/PDFSourceImageGeometry.h
        src/PDFSourceTextGeometry.h
        src/PDFSearchController.cpp
        src/PDFSearchController.h
        src/PDFTabModel.cpp
        src/PDFTabModel.h
        src/PDFTableExtractor.cpp
        src/PDFTableExtractor.h
        src/PDFTableLayout.h
        src/PDFTableSymbolMapping.h
        src/PDFiumSynchronization.h
        src/PDFiumDocument.cpp
        src/PDFiumDocument.h
        src/PDFWidgetAnnotationSuppression.cpp
        src/PDFWidgetAnnotationSuppression.h
        src/PDFPageItem.cpp
        src/PDFPageItem.h
        src/PDFRasterBudget.cpp
        src/PDFRasterBudget.h
        src/PDFRasterPlan.h
        src/SpreadsheetWorkbook.h
        src/SignatureLibrary.cpp
        src/SignatureLibrary.h
    QML_FILES
        src/Main.qml
        src/Components/AppRibbon.qml
        src/Components/AppTheme.qml
        src/Components/CombFieldOptionsPanel.qml
        src/Components/CommentPanel.qml
        src/Components/CommentDrawingView.qml
        src/Components/CommentRibbon.qml
        src/Components/DocumentView.qml
        src/Components/DocumentTabBar.qml
        src/Components/DocumentWorkspace.qml
        src/Components/PageThumbnailPanel.qml
        src/Components/DrawingCommentPropertiesPanel.qml
        src/Components/ImagePropertiesPanel.qml
        src/Components/DropdownOptionsPanel.qml
        ${TREPDF_EDIT_OVERLAY_QML}
        src/Components/EditRibbon.qml
        src/Components/ExportRibbon.qml
        src/Components/FormChoiceOverlay.qml
        src/Components/FormRibbon.qml
        src/Components/FormTextFieldOverlay.qml
        src/Components/InkDrawingView.qml
        src/Components/OrangeButton.qml
        src/Components/HelpToolTip.qml
        src/Components/PageDisplayControl.qml
        src/Components/PageLayoutDelegate.qml
        src/Components/PageInteractionOverlay.qml
        src/Components/WebLinkDraftLayer.qml
        src/Components/ManualLinkLayer.qml
        src/Components/LinkPropertiesDialog.qml
        src/Components/PdfPageDelegate.qml
        src/Components/RadioButtonOptionsPanel.qml
        src/Components/ReaderControlBar.qml
        src/Components/RibbonButton.qml
        src/Components/RibbonGroup.qml
        src/Components/RibbonTabButton.qml
        src/Components/ShapeDraftView.qml
        src/Components/SignRibbon.qml
        src/Components/ProtectRibbon.qml
        src/Components/SetPasswordDialog.qml
        src/Components/WatermarkDialog.qml
        src/Components/SignatureDialog.qml
        src/Components/SearchHighlightLayer.qml
        src/Components/SearchRibbonControl.qml
        src/Components/SquigglyLine.qml
        src/Components/StampView.qml
        src/Components/TextFieldOptionsPanel.qml
        src/Components/TextFormatPanel.qml
        src/Components/ViewRibbon.qml
    RESOURCES
        src/assets/icons/add-page.svg
        src/assets/icons/add.svg
        src/assets/icons/align-center.svg
        src/assets/icons/align-justify.svg
        src/assets/icons/align-left.svg
        src/assets/icons/align-right.svg
        src/assets/icons/arrow.svg
        src/assets/icons/bold-text.svg
        src/assets/icons/cancel.svg
        src/assets/icons/calendar-days.svg
        src/assets/icons/chevron-left.svg
        src/assets/icons/chevron-right.svg
        src/assets/icons/comb-field.svg
        src/assets/icons/comment.svg
        src/assets/icons/delete-page.svg
        src/assets/icons/diamond.svg
        src/assets/icons/down.svg
        src/assets/icons/dropdown-menu.svg
        src/assets/icons/edit-text.svg
        src/assets/icons/folder-open.svg
        src/assets/icons/highlight.svg
        src/assets/icons/image-plus.svg
        src/assets/icons/italic-font.svg
        src/assets/icons/line.svg
        src/assets/icons/link-2.svg
        src/assets/icons/list-box.svg
        src/assets/icons/maximize-2.svg
        src/assets/icons/mouse-pointer-2.svg
        src/assets/icons/mouse-pointer-filled-orange.svg
        src/assets/icons/oval.svg
        src/assets/icons/page-continuous-facing.svg
        src/assets/icons/page-continuous.svg
        src/assets/icons/page-facing.svg
        src/assets/icons/page-separate-cover.svg
        src/assets/icons/page-single.svg
        src/assets/icons/page-split-horizontal.svg
        src/assets/icons/page-split-vertical.svg
        src/assets/icons/panel-left.svg
        src/assets/icons/pen-line.svg
        src/assets/icons/pencil.svg
        src/assets/icons/polygon.svg
        src/assets/icons/polyline.svg
        src/assets/icons/radio.svg
        src/assets/icons/rectangle.svg
        src/assets/icons/redo-2.svg
        src/assets/icons/remove-text.svg
        src/assets/icons/rotate-ccw.svg
        src/assets/icons/rotate-cw.svg
        src/assets/icons/save.svg
        src/assets/icons/search.svg
        src/assets/icons/send.svg
        src/assets/icons/shapes.svg
        src/assets/icons/shield-check.svg
        src/assets/icons/signature.svg
        src/assets/icons/square-check-big.svg
        src/assets/icons/spreadsheet.svg
        src/assets/icons/squiggly.svg
        src/assets/icons/stamp.svg
        src/assets/icons/strikethrough.svg
        src/assets/icons/triangle.svg
        src/assets/icons/text-input.svg
        src/assets/icons/type.svg
        src/assets/icons/underline.svg
        src/assets/icons/underline-text.svg
        src/assets/icons/undo-2.svg
        src/assets/icons/zoom-in.svg
        src/assets/icons/zoom-out.svg
)

qt_add_resources(trepdf "trepdf_font_catalog"
    PREFIX "/trepdf/font-catalog"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts"
    FILES src/assets/fonts/ctan-catalog.json
)

if(TREPDF_BUNDLED_FONT_FILES)
    qt_add_resources(trepdf "trepdf_fonts"
        PREFIX "/trepdf/fonts"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts"
        FILES ${TREPDF_BUNDLED_FONT_FILES}
    )
endif()

# Qt 6.8 and 6.9 require this flag for ID-based catalogs; Qt 6.10 deprecated it.
set(TREPDF_LRELEASE_OPTIONS)
if(Qt6_VERSION VERSION_LESS 6.10)
    set(TREPDF_LRELEASE_OPTIONS -idbased)
endif()

qt_add_translations(trepdf
    SOURCE_TARGETS trepdf
    TS_FILE_BASE qml
    TS_FILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
    RESOURCE_PREFIX "/qt/qml/TrePDF/src/i18n"
    NO_GENERATE_PLURALS_TS_FILE
    LRELEASE_OPTIONS ${TREPDF_LRELEASE_OPTIONS}
)

target_include_directories(trepdf
    PRIVATE
        src
)

target_link_libraries(trepdf
    PRIVATE
        Qt6::Qml
        Qt6::Network
        trepdf_font_archive
        trepdf_password_writer
        Qt6::QmlIntegration
        Qt6::Quick
        Qt6::QuickControls2
        PDFium::PDFium
        Libxlsxwriter::Libxlsxwriter
)

set_target_properties(trepdf PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    WIN32_EXECUTABLE TRUE
)

if(WIN32)
    target_link_libraries(trepdf PRIVATE dwrite)
endif()

if(APPLE)
    find_library(TREPDF_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(TREPDF_CORE_TEXT_FRAMEWORK CoreText REQUIRED)
    target_link_libraries(trepdf
        PRIVATE
            "${TREPDF_CORE_FOUNDATION_FRAMEWORK}"
            "${TREPDF_CORE_TEXT_FRAMEWORK}"
    )

    get_target_property(TREPDF_PDFIUM_LIBRARY PDFium::PDFium IMPORTED_LOCATION)
    get_target_property(TREPDF_QPDF_LIBRARY QPDF::QPDF IMPORTED_LOCATION)
    set_target_properties(trepdf PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
    add_custom_command(TARGET trepdf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:trepdf>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_PDFIUM_LIBRARY}"
            "$<TARGET_BUNDLE_DIR:trepdf>/Contents/Frameworks/libpdfium.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_QPDF_LIBRARY}"
            "$<TARGET_BUNDLE_DIR:trepdf>/Contents/Frameworks/libqpdf.dylib"
        COMMAND install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib "$<TARGET_FILE:trepdf>"
        VERBATIM
    )
endif()

include(CTest)
if(WIN32 AND NOT TREPDF_DEPLOY_RUNTIME)
    include(TrePDFWindowsRuntime)
    trepdf_deploy_windows_runtime(trepdf)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS trepdf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TREPDF_DEPLOY_RUNTIME)
    # The QML ribbon uses SVG icons. Linking the module also makes macdeployqt
    # include its image-format plugin in the installed app bundle.
    find_package(Qt6 6.8 REQUIRED COMPONENTS Svg)
    target_link_libraries(trepdf PRIVATE Qt6::Svg)

    if(WIN32)
        # windeployqt only handles Qt dependencies. Copy the PDF packages' DLLs
        # alongside the executable, keeping Qt's matching compiler runtime.
        set(TREPDF_PDF_RUNTIME_LIBRARIES)
        foreach(TREPDF_PDF_DEPENDENCY IN ITEMS PDFium::PDFium QPDF::QPDF)
            get_target_property(TREPDF_PDF_RUNTIME_LIBRARY ${TREPDF_PDF_DEPENDENCY} IMPORTED_LOCATION)
            get_filename_component(TREPDF_PDF_RUNTIME_DIRECTORY "${TREPDF_PDF_RUNTIME_LIBRARY}" DIRECTORY)
            file(GLOB TREPDF_PDF_PACKAGE_LIBRARIES "${TREPDF_PDF_RUNTIME_DIRECTORY}/*.dll")
            list(APPEND TREPDF_PDF_RUNTIME_LIBRARIES
                "${TREPDF_PDF_RUNTIME_LIBRARY}" ${TREPDF_PDF_PACKAGE_LIBRARIES})
        endforeach()
        list(REMOVE_DUPLICATES TREPDF_PDF_RUNTIME_LIBRARIES)
        list(FILTER TREPDF_PDF_RUNTIME_LIBRARIES EXCLUDE REGEX
            "/(libgcc_s_[^/]+|libstdc\\+\\+-6|libwinpthread-1)\\.dll$")
        install(FILES ${TREPDF_PDF_RUNTIME_LIBRARIES} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    set(TREPDF_DEPLOY_PLUGIN_OPTIONS)
    if(NOT APPLE AND Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
        # Keep SVG readers and the headless CI smoke-test platform even when
        # the linker removes unused direct QtSvg references.
        list(APPEND TREPDF_DEPLOY_PLUGIN_OPTIONS
            INCLUDE_PLUGINS qsvg qsvgicon qoffscreen)
    endif()
    qt_generate_deploy_qml_app_script(
        TARGET trepdf
        OUTPUT_SCRIPT TREPDF_DEPLOY_SCRIPT
        ${TREPDF_DEPLOY_PLUGIN_OPTIONS}
    )
    install(SCRIPT ${TREPDF_DEPLOY_SCRIPT})
endif()
