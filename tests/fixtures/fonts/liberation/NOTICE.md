# Liberation Sans test fixtures

These four unmodified static TrueType faces come from the official
[Liberation Fonts 2.1.5 release](https://github.com/liberationfonts/liberation-fonts/releases/tag/2.1.5).
They are licensed under SIL OFL 1.1; see `LICENSE` and `AUTHORS`.

Source archive: `https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz`

Archive SHA-256: `7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0`.
Individual file hashes are recorded in `SHA256SUMS`.

Only the controller test executable embeds this fixture directory, under
`:/trepdf/tests/fonts/liberation`. The web preview distributes a separate,
unmodified Regular face in `src/web/fonts`; desktop font resources are unchanged.
Tests use a temporary private download store when
they need locally available complete faces, and remove that store afterward.
All fixture families are renamed in memory, including the default `TrePDFTestSans`,
to prevent collisions with installed Liberation Sans. Synthetic missing-font,
Unicode-alias and symbol fixtures also use private names; the original files
remain unchanged.
