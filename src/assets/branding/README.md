# TrePDF application icon

`TrePDF-logo.png` is the approved soft-orange variant of the supplied logo.
Its bamboo background has fewer details, while the white PDF letters and folded
paper corner remain part of the artwork. The exterior is transparent. Keep it
in the repository so packaged apps do not depend on a file in Downloads. The
original supplied image remains in Downloads.

- Qt loads the PNG from `:/trepdf/branding/TrePDF-logo.png` at startup.
- macOS bundles `TrePDF.icns` in `Contents/Resources` and references it through
  `CFBundleIconFile`. It contains standard 16–512 point icons at 1× and 2×.
- Windows embeds `TrePDF.ico` through `TrePDF.rc.in`. The ICO contains PNG
  images at 16, 24, 32, 48, 64, 128 and 256 pixels.

The platform icons are resized copies of the transparent PNG; retain RGBA at all
sizes. When replacing the logo, regenerate both platform files from the same
source image; ordinary builds use these checked-in assets.

The artwork was edited with the built-in `imagegen` tool using
`transparent_background: true`. Latest edit prompt summary:

> Soften the orange bamboo background and reduce its detail so the icon remains clear at small sizes. Preserve the rounded-square silhouette, white PDF lettering, and white folded paper corner. Keep the exterior transparent with clean edges and no background matte.
