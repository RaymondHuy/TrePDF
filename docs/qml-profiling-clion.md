# QML profiling from CLion

Local paths target Qt 6.12.0 on macOS. The profiling workflow below was
verified with Qt 6.11.0 on 2026-10-06; a new profiling trace has not yet been
recorded with Qt 6.12.0.

## Configured build and run

CLion has a separate local CMake profile:

| Setting | Value |
| --- | --- |
| Profile | `Debug-QMLProfiler` |
| Build type | `Debug` |
| Build directory | `cmake-build-debug-qml` |
| CMake options | `-DCMAKE_CXX_FLAGS_INIT=-DQT_QML_DEBUG -DCMAKE_PREFIX_PATH=/Users/luongvanhuy/Qt/6.12.0/macos` |

The profile is stored in the local `.idea/workspace.xml`. The run configuration
is in `.run/trepdf-qml-profiler.run.xml`, with target `trepdf` and arguments:

```text
-qmljsdebugger=port:3768,host:127.0.0.1,block,services:CanvasFrameRate,EngineControl,DebugMessages
```

The ordinary `Debug` profile remains unchanged. No application source, rendering,
cache policy, or global compiler flags were changed. Use this profile only for
Debug profiling. `CMAKE_CXX_FLAGS_INIT` seeds the flags on the first configure,
so recreating this setup requires a fresh build directory.

## Record a trace

1. In CLion, select CMake profile `Debug-QMLProfiler` and run configuration
   `trepdf-qml-profiler`, then **Run**. The C++ Debug button is not required.
2. The app waits for a profiler because `block` is enabled. Look for
   `QML Debugger: Waiting for connection on port 3768` in the Run console.
3. Open `/Users/luongvanhuy/Qt/Qt Creator.app` and select **Analyze > QML Profiler
   (Attach to Waiting Application)**. Select kit **Qt 6.12.0 for macOS** and port
   **3768**.
4. Enable Profiling if recording is off, then reproduce the UI interaction.
5. Disable Profiling and wait for the data before closing TrePDF. Save it with
   **Analyze > QML Profiler Options > Save QML Trace**.

Only one profiling client should connect at a time. For normal use, select the
ordinary `Debug` profile and `trepdf` run configuration instead.

## Verification

- Built `trepdf` successfully in the separate directory and launched it through
  CLion's Run action.
- Verified `QT_QML_DEBUG` is present in the profiling build's `main.cpp` compile
  command and absent from the ordinary Debug build.
- Verified Qt Creator establishes a connection to TrePDF on `127.0.0.1:3768`.
- A separate CLI capture of View/Edit tab switches contains 6,171 event ranges,
  including JavaScript, bindings, object creation, signal handling, pixmap cache,
  memory allocation, and scene graph events. The temporary trace is
  `/private/tmp/trepdf-qml-profiler-smoke.qtd` and opens in Qt Creator.

QML memory-allocation events describe the JavaScript heap, not all native or
Metal memory. Profiling also retains trace data, so use the ordinary build for
baseline process-memory measurements and the existing memory telemetry for
`phys_footprint` / malloc statistics.

References: [Qt debugging infrastructure](https://doc.qt.io/qt-6/qtquick-debugging.html),
[Qt profiling services](https://doc.qt.io/qt-6/qtquick-profiling.html), and
[Qt Creator profiling](https://doc.qt.io/qtcreator/creator-how-to-profile-qml.html).
