# TrePDF application icon

`TrePDF-logo.png` is the supplied logo with its exterior ivory background removed.
It has an alpha channel, while the white PDF letters and folded paper corner
remain part of the artwork. Keep it in the repository so packaged apps do not
depend on a file in Downloads. The original supplied image remains in Downloads.

- Qt loads the PNG from `:/trepdf/branding/TrePDF-logo.png` at startup.
- macOS bundles `TrePDF.icns` in `Contents/Resources` and references it through
  `CFBundleIconFile`. It contains standard 16–512 point icons at 1× and 2×.
- Windows embeds `TrePDF.ico` through `TrePDF.rc.in`. The ICO contains PNG
  images at 16, 24, 32, 48, 64, 128 and 256 pixels.

The platform icons are resized copies of the transparent PNG; retain RGBA at all
sizes. When replacing the logo, regenerate both platform files from the same
source image; ordinary builds use these checked-in assets.

Background removal used the built-in `imagegen` tool in background-extraction
mode with `transparent_background: true`. Final cleanup prompt:

> Use case: background-extraction. This is an already cut-out orange rounded-square PDF app icon. Make a precision alpha-edge cleanup only. There are stray red and yellow speckles outside the icon, particularly above the top edge and below the bottom edge. Remove ALL disconnected flecks, exterior glow, fringe, white matte, and artifacts. Every pixel outside the one smooth rounded-square silhouette must have alpha ZERO. The icon must have a single continuous clean anti-aliased edge and fully transparent empty margins. Keep the entire interior artwork exactly as supplied: bamboo, orange colors, sun, opaque white PDF letters and opaque folded white paper corner. Do not redraw, shift, resize, change colors, or change the logo. Transparent PNG with clean alpha suitable for a macOS Dock icon.
