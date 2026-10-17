# The browser viewer has its own dependency boundary. Desktop PDF editing,
# native PDF libraries and OCR are deliberately not linked into this target.
find_package(Qt6 6.12 REQUIRED COMPONENTS Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(trepdf_web main.cpp)
set_source_files_properties(../Components/web/Viewer.qml PROPERTIES
    QT_RESOURCE_ALIAS Viewer.qml
)
qt_add_qml_module(trepdf_web
    URI TrePDFWeb
    QML_FILES ../Components/web/Viewer.qml
    SOURCES
        BrowserDocument.cpp BrowserDocument.h
        PageRaster.cpp PageRaster.h
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
