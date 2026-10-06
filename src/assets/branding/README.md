# TrePDF application icon

`TrePDF-logo.png` is the approved soft-orange variant of the supplied logo.
Its bamboo background has fewer details, while the white PDF letters and folded
paper corner remain part of the artwork. The exterior is transparent. Keep it
in the repository so packaged apps do not depend on a file in Downloads. The
original supplied image remains in Downloads.

- `TrePDF-logo.png` remains the 1254×1254 master; it is not a runtime resource.
- Qt uses `:/trepdf/branding/TrePDF-icon-512.png` for runtime window icons on
  Windows/Linux and as a macOS fallback outside an app bundle, with a missing
  bundle icon, or on a non-Cocoa platform plugin. This checked-in 512×512 RGBA
  resize supports 256-point icons at 2× DPR without decoding the master at startup.
- macOS bundles `TrePDF.icns` in `Contents/Resources` and references it through
  `CFBundleIconFile`. It contains standard 16–512 point icons at 1× and 2×.
  On Cocoa, startup checks the main application's bundle metadata and resource;
  when the native icon is present, it leaves the icon to macOS. Direct execution
  of `trepdf.app/Contents/MacOS/trepdf` from CLion or a terminal uses this same path.
- Windows embeds `TrePDF.ico` through `TrePDF.rc.in`. The ICO contains PNG
  images at 16, 24, 32, 48, 64, 128 and 256 pixels.

The platform icons are resized copies of the transparent PNG; retain RGBA at all
sizes. When replacing the logo, regenerate both platform files from the same
source image and the runtime PNG; ordinary builds use these checked-in assets.
Regenerate the runtime image on macOS with:

```sh
sips -z 512 512 src/assets/branding/TrePDF-logo.png \
  --out src/assets/branding/TrePDF-icon-512.png
```

The runtime PNG has 1,048,576 bytes per RGBA32 buffer, versus 6,290,064 for the
master. These are pixel-buffer sizes, not a prediction of process footprint.
See `docs/startup-icon-memory-2026-10-06.md` for measured bundle results.

The artwork was edited with the built-in `imagegen` tool using
`transparent_background: true`. Latest edit prompt summary:

> Soften the orange bamboo background and reduce its detail so the icon remains clear at small sizes. Preserve the rounded-square silhouette, white PDF lettering, and white folded paper corner. Keep the exterior transparent with clean edges and no background matte.
