# The browser viewer has its own dependency boundary. Desktop PDF editing,
# native PDF libraries and OCR are deliberately not linked into this target.
find_package(Qt6 6.12 REQUIRED COMPONENTS LinguistTools Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(trepdf_web main.cpp)
set(TREPDF_WEB_QML
    ../Components/web/Viewer.qml
    ../Components/web/DocumentViewport.qml
    ../Components/web/BrowserEditController.qml
    ../Components/web/PageEditOverlay.qml
    ../Components/web/EditPropertiesPanel.qml
    ../Components/platform/AppWindow.qml
    ../Components/platform/+web/AppWindow.qml
    ../Components/AppTheme.qml
    ../Components/RibbonTabStrip.qml
    ../Components/RibbonSurface.qml
    ../Components/ViewRibbonTools.qml
    ../Components/EditRibbonTools.qml
    ../Components/RibbonTabButton.qml
    ../Components/RibbonButton.qml
    ../Components/RibbonGroup.qml
    ../Components/DocumentTabs.qml
    ../Components/DocumentEmptyState.qml
    ../Components/OrangeButton.qml
    ../Components/ReaderModes.qml
    ../Components/ReaderControlBar.qml
    ../Components/PageDisplayControl.qml
    ../Components/HelpToolTip.qml
    ../Components/ios/HorizontalControlViewport.qml
)
# Keep the same resource layout as desktop so shared relative icon/import URLs
# resolve without web branches in the presentation components.
foreach(qml IN LISTS TREPDF_WEB_QML)
    string(REGEX REPLACE "^\\.\\./" "src/" alias "${qml}")
    set_source_files_properties("${qml}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()
set_source_files_properties(../Components/platform/+web/AppWindow.qml PROPERTIES
    QT_QML_SKIP_QMLDIR_ENTRY TRUE)
file(GLOB TREPDF_WEB_ICONS CONFIGURE_DEPENDS ../assets/icons/*.svg)
foreach(icon IN LISTS TREPDF_WEB_ICONS)
    get_filename_component(name "${icon}" NAME)
    set_source_files_properties("${icon}" PROPERTIES
        QT_RESOURCE_ALIAS "src/assets/icons/${name}")
endforeach()
set_source_files_properties(fonts/LiberationSans-Regular.ttf PROPERTIES
    QT_RESOURCE_ALIAS "src/web/fonts/LiberationSans-Regular.ttf")
qt_add_qml_module(trepdf_web
    URI TrePDFWeb
    QML_FILES ${TREPDF_WEB_QML}
    RESOURCES ${TREPDF_WEB_ICONS} fonts/LiberationSans-Regular.ttf
    SOURCES
        BrowserDocument.cpp BrowserDocument.h
        PageRaster.cpp PageRaster.h
)
# Reuse the desktop catalogs, without an lupdate that drops desktop-only text.
qt_add_translations(trepdf_web
    TS_FILES ../../i18n/qml_vi.ts ../../i18n/qml_en.ts
    RESOURCE_PREFIX "/trepdf/web/i18n"
)
target_link_libraries(trepdf_web PRIVATE Qt6::Quick Qt6::QuickControls2)
target_compile_definitions(trepdf_web PRIVATE TREPDF_VERSION="${PROJECT_VERSION}")

if(EMSCRIPTEN)
    # PDFium runs in a separate Worker/Wasm instance; Qt stays single threaded.
    set_target_properties(trepdf_web PROPERTIES
        QT_WASM_EXTRA_EXPORTED_METHODS "HEAPU8"
        QT_WASM_MAXIMUM_MEMORY "512MB"
    )
    target_link_options(trepdf_web PRIVATE
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/browser-bridge.js"
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_trepdfWebMessage','_trepdfWebPixels']"
    )
    set_property(TARGET trepdf_web APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/browser-bridge.js")
endif()
