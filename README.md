# exr-viewer

Windows Explorer thumbnails for OpenEXR (`.exr`) images.

Windows has no OpenEXR decoder. exr-viewer adds a WIC codec built on the official OpenEXR library:

- Thumbnails, preview pane and photo viewers, for every OpenEXR compression (ZIP, PIZ, DWAA/DWAB, B44, HTJ2K...), tiled, multi-part and deep files.
- Correct colours: linear values are converted to sRGB, other primaries (ACES...) too, and HDR images are tone mapped instead of clipped. See [docs/FORMAT.md](docs/FORMAT.md).

## Install

1. Download a release (or build it) and extract it.
2. Run `install.bat` as administrator.
3. If old icons remain, run `clear-thumbnails.bat`.

Uninstall with `uninstall.bat`. If Microsoft Defender ASR rules block the DLL, allow `C:\Program Files\ExrViewer\ExrWic.dll` with `Add-MpPreference -AttackSurfaceReductionOnlyExclusions`.

## Build

Needs CMake 3.24+, Git and Visual Studio 2022 (C++). Run `build.bat`: OpenEXR and Imath are fetched at pinned versions and the package is created in `dist\`.

Testing (generated images and the official OpenEXR test set): see [docs/FORMAT.md](docs/FORMAT.md#testing).

## License

MIT. OpenEXR, Imath, libdeflate, zstd and OpenJPH keep their own licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
