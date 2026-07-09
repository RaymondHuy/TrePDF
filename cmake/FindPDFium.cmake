include(FindPackageHandleStandardArgs)

set(PDFium_ROOT "" CACHE PATH "PDFium installation prefix")

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

find_library(PDFium_LIBRARY
    NAMES
        pdfium
        libpdfium
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

find_package_handle_standard_args(PDFium
    REQUIRED_VARS
        PDFium_LIBRARY
        PDFium_INCLUDE_DIR
)

if(PDFium_FOUND AND NOT TARGET PDFium::PDFium)
    add_library(PDFium::PDFium UNKNOWN IMPORTED)
    set_target_properties(PDFium::PDFium PROPERTIES
        IMPORTED_LOCATION "${PDFium_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(PDFium_INCLUDE_DIR PDFium_LIBRARY)
