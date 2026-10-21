# Third-party notices

## Qt 6.10.2

The application dynamically links unmodified Qt libraries, under their upstream LGPLv3 / GPLv3 / commercial terms. Qt sources: https://code.qt.io/cgit/qt/ . Source archives: https://download.qt.io/archive/qt/6.10/6.10.2/single/ . License texts accompany this application under `licenses/`; the installed Qt SDK's SPDX SBOMs are included there with component copyright and license information. Replaceable Qt libraries are deployed as separate shared libraries. No PySide or PyInstaller is bundled.

## Optional standalone CPython

Standard releases include CPython 3.12.15 from https://github.com/astral-sh/python-build-standalone/releases/tag/20261020 using the `install_only_stripped` archives. CPython uses the Python Software Foundation license; its bundled native libraries retain their own licenses. The entire upstream installation, including license files, is retained under `runtime/python/`. Minimal releases omit this distribution. Download origins, exact archive hashes and platform variants are pinned in `scripts/python-distributions.json` and recorded in each standard package's `build-info.json`.

## QtKeychain 0.15.0 and miniz 3.0.2

QtKeychain (https://github.com/frankosterfeld/qtkeychain) uses BSD-3-Clause. miniz (https://github.com/richgel999/miniz) uses the MIT license. Their license texts accompany this application under `licenses/`.

Windows archives include app-local Microsoft Visual C++ 2022 runtime DLLs, distributed with the application under the Visual Studio redistributable terms: https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files .

## WebP

Lossless WebP assets are decoded by Qt's image format plugin. libwebp (https://github.com/webmproject/libwebp) uses BSD-3-Clause; its license text is included under `licenses/`.

## Bundled search tools

Release bundles include fd 10.3.0 (MIT or Apache-2.0, https://github.com/sharkdp/fd/tree/v10.3.0) and ripgrep 14.1.1 (MIT or Unlicense, https://github.com/BurntSushi/ripgrep/tree/14.1.1). Their upstream license files accompany the binaries under `runtime/releases/coding-1.3.0/bin`. Archive SHA-256 values are pinned in `scripts/search-tools.json`.

## Pi Coding Agent

Pi Coding Agent and its dependencies are from https://github.com/earendil-works/pi (version 1.3.0). The complete upstream coding agent, tools, resource loader and session implementation are retained unmodified. The Pi AI built-in provider catalog and generated model list are replaced with empty catalogs; the application registers its explicit loopback gateway. Upstream protocol SDKs and their individual licenses remain in the runtime node_modules directory.

Copyright (c) 2025 Mario Zechner

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Dependencies retain their upstream license files in the runtime node_modules directory. Locally drawn SVG interface icons belong to this project. The Yanami artwork is reused from the Python edition; see `app/icons/yanami-pixel.provenance.json`. No affiliation with the character rights holders is implied.

## Bundled Node.js and npm

Release packages include the official Node.js 22.23.3 distribution and npm from https://nodejs.org/dist/v22.23.3/ . The full distribution retains its LICENSE files and npm dependency notices under `runtime/node/`. Downloads are pinned by SHA-256 in `scripts/node-distributions.json`; the archive origin and version are recorded in build metadata.
