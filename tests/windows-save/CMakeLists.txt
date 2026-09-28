cmake_minimum_required(VERSION 3.22)
project(TrePDFWindowsSaveTests LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "These tests exercise Windows atomic file replacement")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Test)
find_package(PDFium REQUIRED)

add_executable(trepdf_windows_save_tests
    tst_WindowsSave.cpp
    ../../src/PDFAtomicSaveFile.cpp
    ../../src/PDFiumDocument.cpp
)
target_include_directories(trepdf_windows_save_tests PRIVATE ../../src)
target_compile_definitions(trepdf_windows_save_tests PRIVATE NOMINMAX)
target_link_libraries(trepdf_windows_save_tests PRIVATE Qt6::Core Qt6::Test PDFium::PDFium)
if(MSVC)
    target_compile_options(trepdf_windows_save_tests PRIVATE /utf-8 /W4 /WX)
endif()
add_custom_command(TARGET trepdf_windows_save_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:PDFium::PDFium>" "$<TARGET_FILE_DIR:trepdf_windows_save_tests>"
    VERBATIM
)

enable_testing()
add_test(NAME windows_atomic_save COMMAND trepdf_windows_save_tests
    -o -,txt -o windows-atomic-save.xml,junitxml)
get_filename_component(_compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(windows_atomic_save PROPERTIES
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${_compiler_directory}"
)
