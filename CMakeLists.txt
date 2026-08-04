cmake_minimum_required(VERSION 3.21)

project(TrePDF
    VERSION 0.1.0
    LANGUAGES CXX
)

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

find_package(Qt6 6.8 REQUIRED COMPONENTS
    LinguistTools
    Qml
    Quick
    QuickControls2
)
find_package(PDFium REQUIRED)

qt_standard_project_setup(
    REQUIRES 6.8
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES vi
)

file(GLOB_RECURSE TREPDF_BUNDLED_FONT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.otf"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.OTF"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.ttc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.TTC"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/fonts/*.TTF"
)

qt_add_executable(trepdf
    src/main.cpp
)

qt_add_qml_module(trepdf
    URI TrePDF
    SOURCES
        src/ApplicationFonts.cpp
        src/ApplicationFonts.h
        src/DraftImageItem.cpp
        src/DraftImageItem.h
        src/FormFieldLayer.cpp
        src/FormFieldLayer.h
        src/PDFAcroFormCanonicalizer.cpp
        src/PDFAcroFormCanonicalizer.h
        src/PDFAcroFormWriter.cpp
        src/PDFAcroFormWriter.h
        src/PDFDocument.cpp
        src/PDFDocument.h
        src/PDFEditController.cpp
        src/PDFEditController.h
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
        src/PDFTabModel.cpp
        src/PDFTabModel.h
        src/PDFiumSynchronization.h
        src/PDFWidgetAnnotationSuppression.cpp
        src/PDFWidgetAnnotationSuppression.h
        src/PDFPageItem.cpp
        src/PDFPageItem.h
    QML_FILES
        src/Main.qml
        src/Components/AppRibbon.qml
        src/Components/AppTheme.qml
        src/Components/DocumentView.qml
        src/Components/DocumentTabBar.qml
        src/Components/DocumentWorkspace.qml
        src/Components/DropdownOptionsPanel.qml
        src/Components/EditOverlay.qml
        src/Components/EditRibbon.qml
        src/Components/FormChoiceOverlay.qml
        src/Components/FormRibbon.qml
        src/Components/FormTextFieldOverlay.qml
        src/Components/OrangeButton.qml
        src/Components/PageInteractionOverlay.qml
        src/Components/PdfPageDelegate.qml
        src/Components/RadioButtonOptionsPanel.qml
        src/Components/RibbonButton.qml
        src/Components/RibbonGroup.qml
        src/Components/RibbonTabButton.qml
        src/Components/ShapeDraftView.qml
        src/Components/SignRibbon.qml
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
        src/assets/icons/calendar-days.svg
        src/assets/icons/chevron-left.svg
        src/assets/icons/chevron-right.svg
        src/assets/icons/delete-page.svg
        src/assets/icons/diamond.svg
        src/assets/icons/dropdown-menu.svg
        src/assets/icons/edit-text.svg
        src/assets/icons/folder-open.svg
        src/assets/icons/highlight.svg
        src/assets/icons/image-plus.svg
        src/assets/icons/italic-font.svg
        src/assets/icons/line.svg
        src/assets/icons/link-2.svg
        src/assets/icons/maximize-2.svg
        src/assets/icons/mouse-pointer-2.svg
        src/assets/icons/mouse-pointer-filled-orange.svg
        src/assets/icons/oval.svg
        src/assets/icons/panel-left.svg
        src/assets/icons/pen-line.svg
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
        Qt6::QmlIntegration
        Qt6::Quick
        Qt6::QuickControls2
        PDFium::PDFium
)

set_target_properties(trepdf PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if(APPLE)
    find_library(TREPDF_CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(TREPDF_CORE_TEXT_FRAMEWORK CoreText REQUIRED)
    target_link_libraries(trepdf
        PRIVATE
            "${TREPDF_CORE_FOUNDATION_FRAMEWORK}"
            "${TREPDF_CORE_TEXT_FRAMEWORK}"
    )

    get_target_property(TREPDF_PDFIUM_LIBRARY PDFium::PDFium IMPORTED_LOCATION)
    set_target_properties(trepdf PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
    )
    add_custom_command(TARGET trepdf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:trepdf>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TREPDF_PDFIUM_LIBRARY}"
            "$<TARGET_BUNDLE_DIR:trepdf>/Contents/Frameworks/libpdfium.dylib"
        COMMAND install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib "$<TARGET_FILE:trepdf>"
        VERBATIM
    )
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS trepdf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
