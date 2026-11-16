# Contributing to Sprawl

Bug reports, small improvements, and translations are welcome. Open an
[issue](https://github.com/willykev/sprawl/issues) to discuss a larger change first.

## Build and test

On Arch Linux, install `base-devel`, `qt6-base`, `qt6-declarative`,
`qt6-imageformats`, `qt6-svg`, and `qt6-wayland`. Sprawl also uses
`xdg-desktop-portal` for file dialogs.

```sh
bin/build
bin/test
build/sprawl open
```

The tests run offscreen with temporary data. Keep changes focused, add a
regression test for a bug fix, and describe how you checked the result in
your pull request. Follow the surrounding C++ and QML style.

One test looks at exported PDFs with Qt's own PDF reader, which comes with
`qt6-webengine`; where that is not installed, the test is skipped.

`bin/screenshots` redraws the README's pictures from the example board, off
screen and in Omarchy's default theme. Run it after changing how Sprawl
looks, and commit the pictures with the change. It needs a Wayland session,
ImageMagick and ffmpeg.

The [full guide](docs/guide.md) covers the app's behavior and data format.
Contributions use the project's [MIT license](LICENSE).

## Releases

Update `VERSION`, `pkgver` in `pkgbuild/PKGBUILD`, and `CHANGELOG.md` together.
Write `docs/releases/v<VERSION>.md` with the changes and installation notes.
Then build, test, commit, and create an annotated tag:

```sh
release_version=$(cat VERSION)
git tag -a "v$release_version" -m "Release v$release_version"
bin/release
```

`bin/release` builds from the tagged source, runs the tests, and writes the
source archive, Arch package, standalone `PKGBUILD`, and `SHA256SUMS` to
`dist/v<VERSION>/`. It needs Python 3.12 or newer and Arch's `makepkg`.
Packages are built for the current machine's architecture.

Maintainers can run `bin/release --publish` to push the commit and tag, set
the GitHub description and topics, and publish the release with its assets.
GitHub CLI authentication is required. Verified local assets are reused on
retries; existing release assets are preserved.
