# Build Umeko C++

Use CMake 3.24+, a C++20 compiler, Qt 6.10.2 (Core, Gui, Quick, Quick Controls, Quick Dialogs, Network, Concurrent, Test, SVG and Image Formats), Node.js 22.19+, and Python 3.10+ for build helpers/tests. Python is not embedded in releases.

The [native build workflow](../.github/workflows/build.yml) is the reproducible recipe for Windows MSVC 2022, macOS ARM/Intel and Linux GCC. CMake fetches pinned QtKeychain and miniz sources. The Qt Image Formats module is required for the lossless WebP character assets.

```bash
npm ci --prefix runtime --ignore-scripts
node scripts/prepare-runtime.mjs
python scripts/fetch-search-tools.py
python scripts/build-provider-resources.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/gcc_64
cmake --build build --config Release --parallel 3
node --test tests/gateway.test.mjs
ctest --test-dir build -C Release --output-on-failure
python scripts/package.py
```

On Windows, use the Qt `msvc2022_64` directory. Visual Studio Build Tools 2022 is sufficient; CMake selects its generator. On macOS, use the Qt `macos` directory and Xcode command-line tools. Add Qt's `bin` directory to PATH when running tests from a build tree. `scripts/package.py` uses CMake's native Qt deployment, copies the full Pi runtime and provider resources, runs the installed application, tests the installed Pi tool loop, and creates a portable archive plus checksums.

For direct source development:

```bash
./build/UmekoCpp --resource-root "$PWD" --data-dir "$PWD/build/dev-profile"
# Windows: build/Release/UmekoCpp.exe
# macOS: build/UmekoCpp.app/Contents/MacOS/UmekoCpp
```

Use an isolated profile for tests. Tests use synthetic local HTTP fixtures, an isolated Pi agent directory and an echo provider; no paid model or enterprise login is contacted. Credentials require the real system credential service. CI Linux starts an ephemeral unlocked Secret Service keyring inside a D-Bus session.

## Linux input methods

For fcitx5, install the distribution's `fcitx5-frontend-qt6` plugin only when compatible with the bundled Qt ABI. Otherwise enable fcitx5's IBus frontend and run `fcitx5 --enable ibus`. The app automatically selects the bundled IBus input context when an fcitx context is unavailable. To explicitly select an installed context, use `UMEKO_QT_IM_MODULE=ibus ./UmekoCpp` (or `fcitx` for a compatible plugin). Restart the app after changing input method settings.

Native IME composition, desktop credential prompts, file dialogs and interactive terminal appearance require manual checks on an actual desktop; headless CI does not prove these OS interactions.

## Release contents and size

Release packaging excludes the build SDK, development environment, Python/PySide, duplicate source PNGs and JavaScript source maps. It retains the complete Pi coding package, CLI, extension loader, search binaries, runtime modules, public SDK/Skill archives and licenses. Images are losslessly encoded as WebP; decoded RGBA equality is recorded in `app/icons/lossless-assets.json`.

Do not strip Pi's CLI bundle or extension dependencies to reduce size: doing so would break the native terminal, package management or extension execution. The separate external Node.js and optional Python installations are not included in archive-size comparisons.
