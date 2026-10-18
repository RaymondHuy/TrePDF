# Liberation Sans for the browser editor

`LiberationSans-Regular.ttf` is an unmodified static TrueType face from the
[Liberation Fonts 2.1.5 release](https://github.com/liberationfonts/liberation-fonts/releases/tag/2.1.5).
It is licensed under SIL OFL 1.1; see `LICENSE` and `AUTHORS`.

Source archive: `https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz`

Archive SHA-256: `7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0`.
Font SHA-256: `76d04c18ea243f426b7de1f3ad208e927008f961dc5945e5aad352d0dfde8ee8`.

The web package serves this face from `pdfium/fonts/` so the browser worker can
embed text added by the Edit preview, including Vietnamese text. The desktop
font resources and font resolution are unchanged. This copy is separate from
the controller tests' font fixtures, which use private family names in memory.
