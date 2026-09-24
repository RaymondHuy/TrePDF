# TeX Gyre Termes regression fixture

This directory is a partial selection from the TeX Gyre distribution for
TrePDF's offline font validation, cache and PDF persistence tests. It is not
the complete TeX Gyre package. These fonts are not bundled in the TrePDF app.

## Upstream attribution and licence

TeX Gyre Termes, version 2.004 (7 October 2009), by Bogusław Jackowski and
Janusz M. Nowacki. Copyright 2006–2009 for the TeX Gyre extensions by
B. Jackowski and J. M. Nowacki on behalf of TeX Users Groups. Vietnamese
characters were added by Han The Thanh.

The upstream font and documentation retain the GUST Font License, based on
the LaTeX Project Public License version 1.3c or later. Verbatim upstream
notices are included as `GUST-FONT-LICENSE.txt`, `README-TeX-Gyre-Termes.txt`
and `MANIFEST-TeX-Gyre-Termes.txt`; the LPPL text is `LPPL-1.3c.txt`.
The archived GFL file begins with the upstream preliminary-version notice;
that notice is preserved verbatim.

This selection was prepared for TrePDF on 24 September 2026. The only change
to the distribution is selecting the files below and adding this notice;
the original font bytes, names, copyright and licence text are unchanged.
No outline conversion, subsetting or font metadata edits were performed.
The original authors do not provide support for this TrePDF fixture or its
tests. Report fixture/test problems through the TrePDF project.

The complete, unmodified upstream work, including the remaining files listed
in the manifest, is available from:

- [Complete TeX Gyre archive](https://mirror.ctan.org/fonts/tex-gyre.zip).
- [CTAN package and upstream home page](https://ctan.org/pkg/tex-gyre).
- [GUST TeX Gyre project](https://www.gust.org.pl/projects/e-foundry/tex-gyre).

Keep this notice and the included upstream notices with any redistribution
of the fixture. This fixture selection does not change TrePDF's own licence.

## Reproducible sources

Retrieved through HTTPS from these original URLs on 24 September 2026:

| File | Original source | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `texgyretermes-regular.otf` | [CTAN OpenType file](https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-regular.otf) | 140932 | `cc3fe7c707b81428d23d54df3eadd9228a2bf6a4d43125d94df56f5f63134659` |
| `texgyretermes-italic.otf` | [CTAN OpenType file](https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-italic.otf) | 141324 | `6dd103a1672e50568cd2f8a706ccd48443d44d7d073a59d2286f4e6f746575d6` |
| `texgyretermes-bold.otf` | [CTAN OpenType file](https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-bold.otf) | 139480 | `2fb3e952065fa153c7e4e64e04b98b9d79225739b6025aa3f0f0782d299ff61e` |
| `texgyretermes-bolditalic.otf` | [CTAN OpenType file](https://mirror.ctan.org/fonts/tex-gyre/opentype/texgyretermes-bolditalic.otf) | 144180 | `1bf6af99cb0e26c12951317032d79b96ae009551e59ccf02a5b24f325ecfec87` |
| `GUST-FONT-LICENSE.txt` | [CTAN GFL](https://mirror.ctan.org/fonts/tex-gyre/doc/GUST-FONT-LICENSE.txt) | 1456 | `2bd69affc3da00715116f713f57eab9707e96daf3562ad0215987b15b9c16f73` |
| `README-TeX-Gyre-Termes.txt` | [CTAN README](https://mirror.ctan.org/fonts/tex-gyre/doc/README-TeX-Gyre-Termes.txt) | 18858 | `2ca38f2af08da678b2cdb4ac99f8b3ff72fb7bdcc785ace290cd8ef1b2b947ab` |
| `MANIFEST-TeX-Gyre-Termes.txt` | [CTAN manifest](https://mirror.ctan.org/fonts/tex-gyre/doc/MANIFEST-TeX-Gyre-Termes.txt) | 8198 | `f68e1704d13ab91fb1ef3c9f14e1db94c066c53aa2ac686879213e3619214bc7` |
| `LPPL-1.3c.txt` | [LaTeX Project LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c.txt) | 19102 | `3d262cdf34dafa6955f703c634a8c238ec44109bc8dd6ef34fb7aa54809f7e66` |

All four styles have an `OTTO` sfnt signature and a `CFF ` outline table,
with no `glyf` table. Their family is TeX Gyre Termes. This distinction is
deliberate: renamed TrueType fonts cannot replace these fixtures without
losing coverage of the real CTAN OpenType/CFF validation and save paths.
