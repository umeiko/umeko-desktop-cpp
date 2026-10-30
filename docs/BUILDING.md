# Build Umeko-Desktop

Use CMake 3.24+, a C++20 compiler, Qt 6.10.2 (Core, Gui, Widgets, Quick, Quick Controls, Quick Dialogs, Network, Concurrent, Test, SVG and Image Formats), Node.js 22.19+, and Python 3.12+ for build helpers/tests. Standard releases include a standalone Python interpreter. New minimal releases are no longer maintained.

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

## Python build option

`UMEKO_BUNDLE_PYTHON` defaults to `ON`. Packaging downloads pinned CPython 3.12.15 from Astral's [python-build-standalone](https://github.com/astral-sh/python-build-standalone) `install_only_stripped` distributions. Debug symbols are omitted; the standard library, SSL, SQLite, ctypes, pip, venv and upstream licenses are retained. All four platform downloads have pinned sizes and SHA-256 hashes in `scripts/python-distributions.json`.

```bash
# Standard release: bundled Python
cmake -S . -B build -DUMEKO_BUNDLE_PYTHON=ON
python scripts/package.py

# Local development without bundled Python
cmake -S . -B build -DUMEKO_BUNDLE_PYTHON=OFF
```

Use `-DUMEKO_BUNDLE_PYTHON=OFF` only for local builds. Official CI and releases use `ON`; no new minimal tags are published. The packaging override `--bundle-python` / `--no-bundle-python` must agree with the intended local runtime.

The pinned downloads are approximately 21 MiB (Windows), 24 MiB (macOS) and 33 MiB (Linux). Packaging verifies the installed app's runtime selection, imports native Python modules, runs pip and executes Provider SDK process tests with the bundled interpreter. Bundled Python is used when present; system Python is the automatic fallback. There are no global or per-account overrides. The app does not install plugin dependencies automatically. Use a custom environment or the package's `vendor/` directory for extra dependencies.

## Linux input methods

Linux builds require `extra-cmake-modules`, `libxkbcommon-dev`, `libxcb1-dev` and `libx11-dev`. CMake builds the pinned fcitx5-qt 5.1.13 input plugin with the application's own Qt SDK and includes it in the portable package. DBusAddons is included in the plugin; no system Qt input plugin or IBus fallback is required. Keep fcitx5 and your language engine running. Explicit overrides such as `UMEKO_QT_IM_MODULE=ibus ./UmekoCpp` are preserved. See the [input method regression and verification guide](linux-ime-fcitx5.md).

Native IME composition, desktop credential prompts, file dialogs and interactive terminal appearance require manual checks on an actual desktop. CI checks the real UI and credentials with isolated test data (offscreen on Windows/Linux, Cocoa on macOS); it does not prove every user's desktop configuration.

## Release contents and size

Release packaging excludes the build SDK, development environment, PySide, duplicate source PNGs and JavaScript source maps. It retains the complete Pi coding package, CLI, extension loader, search binaries, runtime modules, public SDK/Skill archives and licenses. Standard releases also retain the complete stripped Python installation. Images are losslessly encoded as WebP; decoded RGBA equality is recorded in `app/icons/lossless-assets.json`.

Do not strip Pi's CLI bundle or extension dependencies to reduce size: doing so would break the native terminal, package management or extension execution. Node.js 22.23.3 and npm are bundled in official releases, with official archive hashes pinned in `scripts/node-distributions.json`. GUI startup, the Pi tool loop and npm are verified with system Node removed from PATH. Earlier size comparisons refer to the minimal edition, without an external Python installation.

macOS packaging also needs `python -m pip install ds-store==1.3.1 mac-alias==2.2.3`. It creates and mounts a verified DMG alongside the updater ZIP; see [macOS disk images](macos-dmg.md).

正式发行版使用 `UMEKO_BUNDLE_PYTHON=ON`。不再维护新的 minimal 发行版；关闭该选项仅用于本地开发。Widget 终端的 VT 解析依赖已随插件附带，原生宿主不需要 PySide。
