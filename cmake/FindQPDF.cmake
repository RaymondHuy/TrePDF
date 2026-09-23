include(FindPackageHandleStandardArgs)

set(QPDF_ROOT "" CACHE PATH "Prebuilt qpdf installation prefix")

find_path(QPDF_INCLUDE_DIR
    NAMES qpdf/qpdf-c.h
    HINTS
        "${QPDF_ROOT}"
        "$ENV{QPDF_ROOT}"
    PATH_SUFFIXES include
)

find_library(QPDF_LIBRARY
    NAMES qpdf
    HINTS
        "${QPDF_ROOT}"
        "$ENV{QPDF_ROOT}"
    PATH_SUFFIXES lib
)

find_package_handle_standard_args(QPDF
    REQUIRED_VARS
        QPDF_LIBRARY
        QPDF_INCLUDE_DIR
)

if(QPDF_FOUND AND NOT TARGET QPDF::QPDF)
    add_library(QPDF::QPDF SHARED IMPORTED)
    set_target_properties(QPDF::QPDF PROPERTIES
        IMPORTED_LOCATION "${QPDF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${QPDF_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(QPDF_INCLUDE_DIR QPDF_LIBRARY)
