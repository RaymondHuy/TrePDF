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
    Qml
    Quick
    QuickControls2
)
find_package(PDFium REQUIRED)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(trepdf
    src/main.cpp
)

qt_add_qml_module(trepdf
    URI TrePDF
    SOURCES
        src/PDFDocument.cpp
        src/PDFDocument.h
        src/PDFPageItem.cpp
        src/PDFPageItem.h
    QML_FILES
        src/Main.qml
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

include(GNUInstallDirs)
install(TARGETS trepdf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
