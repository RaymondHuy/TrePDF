cmake_minimum_required(VERSION 3.22)
project(TrePDFWindowsSystemFontTests LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "These integration tests require installed Windows fonts and DirectWrite")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Test)
qt_standard_project_setup()
find_package(PDFium REQUIRED)
find_package(ZLIB REQUIRED)

qt_add_executable(trepdf_windows_font_tests
    tst_WindowsSystemFonts.cpp
    ../../src/ApplicationFonts.cpp
    ../../src/PDFFontAliases.cpp
    ../../src/WindowsSystemFonts.cpp
    ../../src/FontDownloadService.cpp
    ../../src/GoogleFontsProvider.cpp
    ../../src/CtanFontProvider.cpp
    ../../third_party/libxlsxwriter/third_party/minizip/unzip.c
    ../../third_party/libxlsxwriter/third_party/minizip/ioapi.c
)
qt_add_resources(trepdf_windows_font_tests "trepdf_windows_font_catalog"
    PREFIX "/trepdf/font-catalog"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/assets/fonts"
    FILES ../../src/assets/fonts/ctan-catalog.json
)
target_include_directories(trepdf_windows_font_tests PRIVATE ../../src
    ../../third_party/libxlsxwriter/third_party/minizip)
target_compile_definitions(trepdf_windows_font_tests PRIVATE NOMINMAX)
target_link_libraries(trepdf_windows_font_tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Test PDFium::PDFium ZLIB::ZLIB dwrite
)
if(MSVC)
    target_compile_options(trepdf_windows_font_tests PRIVATE /utf-8)
endif()
add_custom_command(TARGET trepdf_windows_font_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:PDFium::PDFium>" "$<TARGET_FILE_DIR:trepdf_windows_font_tests>"
    VERBATIM
)

enable_testing()
add_test(NAME windows_system_fonts COMMAND trepdf_windows_font_tests
    -o -,txt -o windows-system-fonts.xml,junitxml)
get_filename_component(_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(windows_system_fonts PROPERTIES
    TIMEOUT 120
    # The offscreen plugin does not enumerate installed Windows fonts. These
    # tests create no windows but need the native platform's font database.
    ENVIRONMENT "QT_QPA_PLATFORM=windows"
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_compiler_directory}"
)
