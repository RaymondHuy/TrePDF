# Static, reproducible OCR package produced by scripts/build-ocr.py. No system
# PATH lookup or network access occurs during an ordinary application build.
include(FindPackageHandleStandardArgs)

if(NOT OCR_ROOT)
    if(DEFINED ENV{OCR_ROOT} AND NOT "$ENV{OCR_ROOT}" STREQUAL "")
        set(OCR_ROOT "$ENV{OCR_ROOT}" CACHE PATH "TrePDF static OCR package")
    else()
        set(OCR_ROOT "${CMAKE_SOURCE_DIR}/third_party/ocr" CACHE PATH "TrePDF static OCR package")
    endif()
endif()

find_path(TREPDF_OCR_INCLUDE_DIR tesseract/baseapi.h PATHS "${OCR_ROOT}/include" NO_DEFAULT_PATH)
find_library(TREPDF_TESSERACT_LIBRARY NAMES tesseract PATHS "${OCR_ROOT}/lib" NO_DEFAULT_PATH)
find_library(TREPDF_LEPTONICA_LIBRARY NAMES leptonica PATHS "${OCR_ROOT}/lib" NO_DEFAULT_PATH)
find_file(TREPDF_OCR_ENGLISH_MODEL eng.traineddata PATHS "${OCR_ROOT}/share/tessdata" NO_DEFAULT_PATH)
find_file(TREPDF_OCR_VIETNAMESE_MODEL vie.traineddata PATHS "${OCR_ROOT}/share/tessdata" NO_DEFAULT_PATH)
find_file(TREPDF_OCR_PDF_FONT pdf.ttf PATHS "${OCR_ROOT}/share/tessdata" NO_DEFAULT_PATH)
find_package_handle_standard_args(TrePDFOCR
    REQUIRED_VARS TREPDF_OCR_INCLUDE_DIR TREPDF_TESSERACT_LIBRARY TREPDF_LEPTONICA_LIBRARY
        TREPDF_OCR_ENGLISH_MODEL TREPDF_OCR_VIETNAMESE_MODEL TREPDF_OCR_PDF_FONT
    REASON_FAILURE_MESSAGE "Run python3 scripts/build-ocr.py with the native toolchain, or set OCR_ROOT to a prepared package."
)

if(TrePDFOCR_FOUND)
    find_package(ZLIB REQUIRED)
    find_package(Threads REQUIRED)
    add_library(Leptonica::Leptonica STATIC IMPORTED)
    set_target_properties(Leptonica::Leptonica PROPERTIES
        IMPORTED_LOCATION "${TREPDF_LEPTONICA_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TREPDF_OCR_INCLUDE_DIR};${TREPDF_OCR_INCLUDE_DIR}/leptonica"
        INTERFACE_LINK_LIBRARIES "ZLIB::ZLIB"
    )
    add_library(Tesseract::Tesseract STATIC IMPORTED)
    set_target_properties(Tesseract::Tesseract PROPERTIES
        IMPORTED_LOCATION "${TREPDF_TESSERACT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TREPDF_OCR_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "Leptonica::Leptonica;Threads::Threads"
    )
    if(WIN32)
        set_property(TARGET Leptonica::Leptonica APPEND PROPERTY INTERFACE_LINK_LIBRARIES user32 gdi32)
        set_property(TARGET Tesseract::Tesseract APPEND PROPERTY INTERFACE_LINK_LIBRARIES ws2_32)
    elseif(UNIX)
        set_property(TARGET Leptonica::Leptonica APPEND PROPERTY INTERFACE_LINK_LIBRARIES m)
    endif()
    set(TREPDF_OCR_DATA_DIR "${OCR_ROOT}/share/tessdata")
endif()
