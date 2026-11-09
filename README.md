# MassiGra Modern (prototype)

Windows 11 x64 native image viewer prototype. Win32 + Direct2D + WIC, libwebp, libavif and libarchive. ZIP/CBZ entries are read into memory and passed to decoders; no extraction directory is created.

## Build

The intended build is GitHub Actions on `windows-2025`, using the included vcpkg manifest and CMake workflow. Open **Actions → Build MassiGra Modern (Windows x64)** and download the `MassiGraModern-win-x64` artifact after a successful run.

## Current prototype limitations

- This is an initial prototype, not a release-quality application.
- The Settings menu exposes explicit register/unregister actions for per-user Explorer context-menu verbs. The registration uses HKCU and does not change default file associations.
- GIF is currently displayed as its first frame only.
- Image ordering is case-insensitive lexical ordering, not natural numeric ordering.
- ZIP entry reads reopen and scan the archive for each page. A page cache / indexed random-access layer is still needed for large CBZ books.
- Encrypted archives, multi-volume archives, malformed files, very large image handling and full EXIF orientation are not supported.
- The project has not yet been compiled or tested on Windows. The first Actions run is required to validate the build.

## Runtime model

The workflow targets `x64-windows-static` so third-party libraries and the MSVC runtime are statically linked where supported. Windows system components (WIC, Direct2D, Shell APIs) remain OS dependencies. The actual binary must be tested on a clean Windows 11 machine before distribution.
