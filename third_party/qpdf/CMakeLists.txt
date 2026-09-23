# Minimal library-only build of unmodified qpdf 12.4.1 sources.
# Keep C++20 confined to this backend; TrePDF uses the C API from C++17.
include(CheckTypeSize)
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)
check_type_size(size_t SIZEOF_SIZE_T)
check_include_file("inttypes.h" HAVE_INTTYPES_H)
check_include_file("stdint.h" HAVE_STDINT_H)
check_symbol_exists(localtime_r "time.h" HAVE_LOCALTIME_R)
check_symbol_exists(random "stdlib.h" HAVE_RANDOM)
check_symbol_exists(fseeko "stdio.h" HAVE_FSEEKO)
check_symbol_exists(fseeko64 "stdio.h" HAVE_FSEEKO64)
check_c_source_compiles("#include <time.h>
int main() { struct tm t; t.tm_gmtoff = 0; return 0; }" HAVE_TM_GMTOFF)
check_c_source_compiles("#include <time.h>
int main() { tzset(); return (int)timezone; }" HAVE_EXTERN_LONG_TIMEZONE)
set(USE_CRYPTO_NATIVE ON)
set(USE_CRYPTO_OPENSSL OFF)
set(USE_CRYPTO_GNUTLS OFF)
set(DEFAULT_CRYPTO "native")
set(USE_INSECURE_RANDOM OFF)
set(SKIP_OS_SECURE_RANDOM OFF)
set(ZOPFLI OFF)
configure_file(libqpdf/qpdf/qpdf-config.h.in qpdf/qpdf-config.h)
add_library(trepdf_qpdf STATIC
    libqpdf/AES_PDF_native.cc
    libqpdf/MD5_native.cc
    libqpdf/QPDFCrypto_native.cc
    libqpdf/RC4_native.cc
    libqpdf/SHA2_native.cc
    libqpdf/rijndael.cc
    libqpdf/sha2.c
    libqpdf/sha2big.c
    libqpdf/BitStream.cc
    libqpdf/BitWriter.cc
    libqpdf/Buffer.cc
    libqpdf/BufferInputSource.cc
    libqpdf/ClosedFileInputSource.cc
    libqpdf/ContentNormalizer.cc
    libqpdf/CryptoRandomDataProvider.cc
    libqpdf/FileInputSource.cc
    libqpdf/InputSource.cc
    libqpdf/InsecureRandomDataProvider.cc
    libqpdf/JSON.cc
    libqpdf/JSONHandler.cc
    libqpdf/MD5.cc
    libqpdf/NNTree.cc
    libqpdf/OffsetInputSource.cc
    libqpdf/PDFVersion.cc
    libqpdf/Pipeline.cc
    libqpdf/Pl_AES_PDF.cc
    libqpdf/Pl_ASCII85Decoder.cc
    libqpdf/Pl_ASCIIHexDecoder.cc
    libqpdf/Pl_Base64.cc
    libqpdf/Pl_Buffer.cc
    libqpdf/Pl_Concatenate.cc
    libqpdf/Pl_Count.cc
    libqpdf/Pl_DCT.cc
    libqpdf/Pl_Discard.cc
    libqpdf/Pl_Flate.cc
    libqpdf/Pl_Function.cc
    libqpdf/Pl_LZWDecoder.cc
    libqpdf/Pl_OStream.cc
    libqpdf/Pl_PNGFilter.cc
    libqpdf/Pl_QPDFTokenizer.cc
    libqpdf/Pl_RC4.cc
    libqpdf/Pl_RunLength.cc
    libqpdf/Pl_SHA2.cc
    libqpdf/Pl_StdioFile.cc
    libqpdf/Pl_String.cc
    libqpdf/Pl_TIFFPredictor.cc
    libqpdf/QPDF.cc
    libqpdf/QPDFAcroFormDocumentHelper.cc
    libqpdf/QPDFAnnotationObjectHelper.cc
    libqpdf/QPDFArgParser.cc
    libqpdf/QPDFCryptoProvider.cc
    libqpdf/QPDFDocumentHelper.cc
    libqpdf/QPDFEFStreamObjectHelper.cc
    libqpdf/QPDFEmbeddedFileDocumentHelper.cc
    libqpdf/QPDFExc.cc
    libqpdf/QPDFFileSpecObjectHelper.cc
    libqpdf/QPDFFormFieldObjectHelper.cc
    libqpdf/QPDFJob.cc
    libqpdf/QPDFJob_argv.cc
    libqpdf/QPDFJob_config.cc
    libqpdf/QPDFJob_json.cc
    libqpdf/QPDFLogger.cc
    libqpdf/QPDFMatrix.cc
    libqpdf/QPDFObject.cc
    libqpdf/QPDFObjectHandle.cc
    libqpdf/QPDFObjectHelper.cc
    libqpdf/QPDFOutlineDocumentHelper.cc
    libqpdf/QPDFOutlineObjectHelper.cc
    libqpdf/QPDFPageLabelDocumentHelper.cc
    libqpdf/QPDFPageObjectHelper.cc
    libqpdf/QPDFParser.cc
    libqpdf/QPDFStreamFilter.cc
    libqpdf/QPDFSystemError.cc
    libqpdf/QPDFTokenizer.cc
    libqpdf/QPDFUsage.cc
    libqpdf/QPDFWriter.cc
    libqpdf/QPDF_Array.cc
    libqpdf/QPDF_Dictionary.cc
    libqpdf/QPDF_Stream.cc
    libqpdf/QPDF_String.cc
    libqpdf/QPDF_encryption.cc
    libqpdf/QPDF_json.cc
    libqpdf/QPDF_linearization.cc
    libqpdf/QPDF_objects.cc
    libqpdf/QPDF_pages.cc
    libqpdf/QTC.cc
    libqpdf/QUtil.cc
    libqpdf/RC4.cc
    libqpdf/ResourceFinder.cc
    libqpdf/SecureRandomDataProvider.cc
    libqpdf/SF_FlateLzwDecode.cc
    libqpdf/global.cc
    libqpdf/qpdf-c.cc
    libqpdf/qpdfjob-c.cc
    libqpdf/qpdflogger-c.cc
)
set_target_properties(trepdf_qpdf PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    AUTOMOC OFF
    AUTOUIC OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(trepdf_qpdf PRIVATE QPDF_DISABLE_QTC=1)
target_include_directories(trepdf_qpdf
    PUBLIC include
    PRIVATE libqpdf "${CMAKE_CURRENT_BINARY_DIR}" "${TREPDF_JPEG_INCLUDE_DIR}"
)
target_link_libraries(trepdf_qpdf PRIVATE ZLIB::ZLIB Qt6::BundledLibjpeg)
add_library(QPDF::QPDF ALIAS trepdf_qpdf)
