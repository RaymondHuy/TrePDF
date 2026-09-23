# qpdf backend for TrePDF

Version: 12.4.1
Source: https://github.com/qpdf/qpdf/releases/download/v12.4.1/qpdf-12.4.1.tar.gz
SHA-256: `f045aa277be2356ff53a89a8622945958291177d2483afc20ede7c8a8cd3873c`

Only `include/` and `libqpdf/` plus upstream notices are vendored. Upstream
library sources are unmodified. The local top-level CMake builds the upstream
source list as a static library using its native crypto provider and OS secure
random source. It disables diagnostic coverage hooks and omits CLI, tests, docs
and installation targets. C++20 is limited to this library; TrePDF calls its C API.

Dependencies: system zlib and the static libjpeg already bundled with the selected
Qt SDK. No Homebrew or runtime subprocess is required.

`PDFPasswordWriter` applies AES-256 (revision 6), encrypts metadata, and permits
all document operations after opening. It only accepts unencrypted input and
writes a separate staging path. TrePDF validates that output with PDFium before
atomically replacing the destination. Passwords never appear on a command line.
