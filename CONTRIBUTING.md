# Contributing to TrePDF

TrePDF is in **beta**. Contributions that make PDF editing more reliable,
improve the interface, explain the code, or help the app run on more machines
are welcome. You can contribute through bug reports, reproducible PDF samples,
translations, documentation, tests, or code.

## Start here

1. Follow the [build instructions](README.md#build) for your platform and run
   the app before changing it. Keep a copy of any PDF you use for testing.
2. Read the [source map](docs/source-file-map.md) to find the relevant component.
   The [C++ guide](docs/cpp-file-guide.md) and [QML guide](docs/qml-file-guide.md)
   provide more detail. These architecture guides are currently in Vietnamese.
3. Pick one focused change. For a large feature or architectural change, open
   an issue describing the problem and proposed behavior before investing in
   the implementation.
4. Fork the repository, create a branch in your fork, and make the change.
5. Run the checks relevant to your change, then open a pull request against
   the repository's default branch.

If setup fails, report the command you ran and its error output. Include the
operating system, CPU architecture, compiler, Qt version, and dependency paths
you configured. Replace personal information in paths before posting logs.

## Report a bug

Search existing issues first. A useful report includes:

- TrePDF version from the Information tab, or the commit you built.
- Operating system and version, CPU architecture, and Qt version if built locally.
- Numbered steps to reproduce the problem, starting from opening the document.
- Expected behavior and what actually happened.
- Whether the issue occurs before saving, after Save/Save As, or after reopening.
- Relevant logs or screenshots, and a small PDF that reproduces the problem
  when it can be shared.

Use a synthetic or redacted sample when possible. Only upload files you have
permission to share, and remove confidential content and metadata. A minimal
fixture is more useful than a large document with unrelated pages. If the PDF
cannot be shared, describe how it was created and which features it uses.

## Work in the codebase

Follow the style of the surrounding code:

- Use C++23, PascalCase for C++ classes and QML components, camelCase for
  methods and properties, and `m_` for private C++ data members.
- Prefer RAII and Qt ownership mechanisms. Make object lifetimes and thread
  boundaries explicit.
- Keep a pull request focused. Avoid unrelated formatting, refactors, and
  generated-file changes.
- Register new application C++ and QML sources in `CMakeLists.txt`; register
  test sources in `tests/CMakeLists.txt`.
- Keep build output, local SDK paths, downloaded test documents, and temporary
  debugging code out of commits.

The detailed implementation rules are in [AGENTS.md](AGENTS.md). The following
constraints are particularly important when changing document behavior:

- **Independent document sessions:** selection, drafts, history, and background
  work must stay associated with the correct tab and document revision.
- **PDFium ownership:** a worker uses its own document handle. Do not pass the
  GUI thread's PDFium document to a worker. Use the existing process-wide lock
  and cancellation checks around PDFium work.
- **Bounded work:** keep memory and pending rendering work proportional to
  visible and nearby pages. Avoid loading entire PDFs or caching every page's
  bitmap. Reject stale asynchronous results after edits, navigation, or closing.
- **Drafts and persistence:** retain Undo/Redo and Discard behavior. Validate
  edits before publishing output, keep failed saves from replacing the
  destination, and check the result after saving and reopening.
- **PDF compatibility:** preserve unrelated page content, annotations, forms,
  and document protection. Document unsupported cases instead of silently
  producing a damaged output file.

Consult the feature's pipeline document before changing its storage or lifecycle
contract; the [README documentation links](README.md#source-documentation)
provide starting points.

## Test your change

Use a build configured with `BUILD_TESTING=ON` for automated tests. See the
[test guide](tests/README.md) for executables, fixtures, and how to run individual
Qt Test methods. Run the relevant tests first and include the exact commands
and results in your pull request.

| Change | Useful verification |
| --- | --- |
| Documentation | Check links, commands, and Markdown formatting; no app build is needed. |
| Translations or UI text | Update catalogs, build the app, run QML lint, and inspect longer strings in the UI. |
| Editing or saving | Add a regression for the failing behavior; check Undo/Redo, Save/Save As, and reopen. |
| Rendering or object lifetime | Build the app, run relevant automated tests, and perform the native smoke test below. |
| Packaging or platform support | Build and launch the package on the affected platform; note any platform you could not check. |

For regressions in the main edit-controller suite:

- Declare the test slot in `tests/PDFEditControllerTest.h`.
- Put its implementation and optional `_data` method in the matching
  `tests/tst_PDFEditController<Feature>.cpp` file.
- Reuse fixture helpers from `tests/PDFEditControllerTestSupport*.cpp`, or add a
  small self-contained fixture. Avoid depending on your Downloads folder or
  locally installed fonts.
- Include the fixture's origin and license when adding a third-party file.
  Prefer a generated document when it reproduces the same behavior.

For rendering or lifecycle changes, open a multi-page PDF in the built app,
verify the first page, scroll rapidly in both directions, change zoom several
times, then close and reopen the document while rendering is active. Quit
normally and check for a crash. Texture ownership changes also need a native
macOS Metal run; offscreen tests do not cover that lifecycle.

Before submitting, run:

```sh
git diff --check
```

State any checks you could not run and why. Documentation-only changes do not
need the full test suite.

## Translate the interface

See [Localization](README.md#localization) for catalogs and build commands.
TrePDF uses Qt's ID-based translations:

```qml
//% "Open PDF"
text: qsTrId("OpenPdf")
```

Use `qsTrId()` in QML and `qtTrId()` in C++, with a literal, globally unique
PascalCase ID and an English `//%` source comment. Keep existing IDs stable.
Use placeholders such as `%1`, `%2`, and `%n` rather than joining translated
sentence fragments, and preserve those placeholders in every translation.

After changing interface text, run `update_translations`, complete the affected
catalog entries, build `trepdf`, and run `trepdf_qmllint`. Check the actual UI
for clipped labels and dialog text. New entries should not remain marked
`type="unfinished"`.

To add a language, register its locale in `I18N_TRANSLATED_LANGUAGES` in
`CMakeLists.txt`, generate `i18n/qml_<locale>.ts`, and translate the catalog.
The [language build guide](docs/macos-release-local.md#build-with-another-ui-language)
explains how the current desktop entry point selects a UI locale.

## Open a pull request

Describe the problem and the resulting behavior so someone can review the
change without reading the issue discussion. Include:

- A linked issue, if one exists, and concise reproduction steps for a bug fix.
- The main implementation decisions and any compatibility limitations.
- Test commands and results, plus manual checks and screenshots for UI changes.
- Documentation updates when behavior, setup, or a file format changes.

Keep follow-up fixes in the same pull request and explain any change in scope.
Never include passwords, signing credentials, or personal PDF documents.

TrePDF's source is licensed under [Apache License 2.0](LICENSE). Preserve the
existing license and the notices accompanying third-party dependencies,
fonts, icons, and test fixtures.
