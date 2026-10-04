include(FindPackageHandleStandardArgs)

set(PDFium_ROOT "" CACHE PATH "PDFium installation prefix")

if(IOS)
    # iOS binaries and macOS binaries are incompatible even on the same CPU.
    # Only accept the framework prepared with the iOS dependency script.
    find_path(PDFium_INCLUDE_DIR fpdfview.h
        PATHS "${PDFium_ROOT}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_path(PDFium_FRAMEWORK PDFium
        PATHS "${PDFium_ROOT}/lib/PDFium.framework" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_package_handle_standard_args(PDFium REQUIRED_VARS PDFium_INCLUDE_DIR PDFium_FRAMEWORK)
    if(PDFium_FOUND AND NOT TARGET PDFium::PDFium)
        add_library(PDFium::PDFium SHARED IMPORTED)
        set_target_properties(PDFium::PDFium PROPERTIES
            FRAMEWORK TRUE
            IMPORTED_LOCATION "${PDFium_FRAMEWORK}/PDFium"
            INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR}"
        )
    endif()
    mark_as_advanced(PDFium_INCLUDE_DIR PDFium_FRAMEWORK)
    return()
endif()

find_path(PDFium_INCLUDE_DIR
    NAMES fpdfview.h
    HINTS
        "${PDFium_ROOT}"
        "${PDFium_ROOT}/include"
        "$ENV{PDFIUM_ROOT}"
        "$ENV{PDFIUM_ROOT}/include"
        "$ENV{PDFIUM_DIR}"
        "$ENV{PDFIUM_DIR}/include"
    PATH_SUFFIXES
        include
        pdfium
        include/pdfium
)

set(_PDFium_LIBRARY_NAMES pdfium libpdfium)
if(WIN32)
    list(PREPEND _PDFium_LIBRARY_NAMES pdfium.dll)
endif()

find_library(PDFium_LIBRARY
    NAMES
        ${_PDFium_LIBRARY_NAMES}
    HINTS
        "${PDFium_ROOT}"
        "${PDFium_ROOT}/lib"
        "${PDFium_ROOT}/bin"
        "$ENV{PDFIUM_ROOT}"
        "$ENV{PDFIUM_ROOT}/lib"
        "$ENV{PDFIUM_ROOT}/bin"
        "$ENV{PDFIUM_DIR}"
        "$ENV{PDFIUM_DIR}/lib"
        "$ENV{PDFIUM_DIR}/bin"
)

set(_PDFium_REQUIRED_VARS PDFium_LIBRARY PDFium_INCLUDE_DIR)
if(WIN32)
    get_filename_component(_PDFium_LIBRARY_DIR "${PDFium_LIBRARY}" DIRECTORY)
    find_file(PDFium_RUNTIME_LIBRARY
        NAMES pdfium.dll
        HINTS
            "${PDFium_ROOT}"
            "$ENV{PDFIUM_ROOT}"
            "$ENV{PDFIUM_DIR}"
            "${_PDFium_LIBRARY_DIR}/.."
        PATH_SUFFIXES bin lib
    )
    list(APPEND _PDFium_REQUIRED_VARS PDFium_RUNTIME_LIBRARY)
endif()

find_package_handle_standard_args(PDFium
    REQUIRED_VARS ${_PDFium_REQUIRED_VARS}
)

if(PDFium_FOUND AND NOT TARGET PDFium::PDFium)
    if(WIN32)
        add_library(PDFium::PDFium SHARED IMPORTED)
        set_target_properties(PDFium::PDFium PROPERTIES
            IMPORTED_IMPLIB "${PDFium_LIBRARY}"
            IMPORTED_LOCATION "${PDFium_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR}"
        )
    else()
        add_library(PDFium::PDFium UNKNOWN IMPORTED)
        set_target_properties(PDFium::PDFium PROPERTIES
            IMPORTED_LOCATION "${PDFium_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(PDFium_INCLUDE_DIR PDFium_LIBRARY)
if(WIN32)
    mark_as_advanced(PDFium_RUNTIME_LIBRARY)
endif()
