include(FindPackageHandleStandardArgs)

set(QPDF_ROOT "" CACHE PATH "Prebuilt qpdf installation prefix")

find_path(QPDF_INCLUDE_DIR
    NAMES qpdf/qpdf-c.h
    HINTS
        "${QPDF_ROOT}"
        "$ENV{QPDF_ROOT}"
    PATH_SUFFIXES include
)

set(_QPDF_LIBRARY_NAMES qpdf)
if(WIN32)
    list(APPEND _QPDF_LIBRARY_NAMES qpdf.dll libqpdf.dll)
endif()

find_library(QPDF_LIBRARY
    NAMES ${_QPDF_LIBRARY_NAMES}
    HINTS
        "${QPDF_ROOT}"
        "$ENV{QPDF_ROOT}"
    PATH_SUFFIXES lib
)

set(_QPDF_REQUIRED_VARS QPDF_LIBRARY QPDF_INCLUDE_DIR)
if(WIN32)
    get_filename_component(_QPDF_LIBRARY_DIR "${QPDF_LIBRARY}" DIRECTORY)
    # Official Windows packages use a versioned qpdf DLL (currently qpdf30.dll).
    file(GLOB _QPDF_RUNTIME_CANDIDATES
        "${QPDF_ROOT}/bin/qpdf*.dll"
        "${QPDF_ROOT}/bin/libqpdf*.dll"
        "$ENV{QPDF_ROOT}/bin/qpdf*.dll"
        "$ENV{QPDF_ROOT}/bin/libqpdf*.dll"
        "${_QPDF_LIBRARY_DIR}/../bin/qpdf*.dll"
        "${_QPDF_LIBRARY_DIR}/../bin/libqpdf*.dll"
    )
    set(_QPDF_RUNTIME_NAMES qpdf.dll libqpdf.dll)
    foreach(_QPDF_RUNTIME_CANDIDATE IN LISTS _QPDF_RUNTIME_CANDIDATES)
        get_filename_component(_QPDF_RUNTIME_NAME "${_QPDF_RUNTIME_CANDIDATE}" NAME)
        list(APPEND _QPDF_RUNTIME_NAMES "${_QPDF_RUNTIME_NAME}")
    endforeach()
    find_file(QPDF_RUNTIME_LIBRARY
        NAMES ${_QPDF_RUNTIME_NAMES}
        HINTS
            "${QPDF_ROOT}"
            "$ENV{QPDF_ROOT}"
            "${_QPDF_LIBRARY_DIR}/.."
        PATH_SUFFIXES bin lib
    )
    list(APPEND _QPDF_REQUIRED_VARS QPDF_RUNTIME_LIBRARY)
endif()

find_package_handle_standard_args(QPDF
    REQUIRED_VARS ${_QPDF_REQUIRED_VARS}
)

if(QPDF_FOUND AND NOT TARGET QPDF::QPDF)
    add_library(QPDF::QPDF SHARED IMPORTED)
    if(WIN32)
        set_target_properties(QPDF::QPDF PROPERTIES
            IMPORTED_IMPLIB "${QPDF_LIBRARY}"
            IMPORTED_LOCATION "${QPDF_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${QPDF_INCLUDE_DIR}"
        )
    else()
        set_target_properties(QPDF::QPDF PROPERTIES
            IMPORTED_LOCATION "${QPDF_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${QPDF_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(QPDF_INCLUDE_DIR QPDF_LIBRARY)
if(WIN32)
    mark_as_advanced(QPDF_RUNTIME_LIBRARY)
endif()
