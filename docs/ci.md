# CI and release builds

Daily pushes and pull requests run the full shared Node/Python regressions once
on Ubuntu, alongside a Windows native build. Windows still runs the OS-specific
runtime regressions, all four native test suites, and the installed application
smoke checks before uploading its package. Qt and npm downloads are cached.
An obsolete push/PR build is cancelled when that same ref receives a newer push;
manually requested builds are not cancelled this way.

For Linux and both macOS architectures, run **Native desktop release builds**
manually on `main` with **all_platforms** enabled. Each selected platform runs its
native tests and packaged application checks; macOS also verifies its DMG. Shared
regressions still run once, and platform-sensitive path, process, permission,
update, ACP and PTY regressions also run on each native target.

The build uploads a `ci-build-plan` artifact containing its exact source commit
and selected platforms. The existing publication workflow accepts only a fully
successful, trusted `main` build and verifies every platform declared in that
plan. A daily Windows build produces six release assets: ZIP, its checksum,
build manifest, two provider SDK downloads, and the combined checksums. A full
build retains all nineteen assets, including both macOS ZIP/DMG pairs. Missing
packages, foreign source commits, bad checksums and undeclared platforms stop
publication. Older builds without a plan still require all four platforms.

README and release download buttons include only platforms explicitly selected
and verified for that release. This does not relabel old packages or weaken
native layout/performance assertions. Existing releases and version tags are
left alone unless replacement is explicitly requested; a Windows-only
replacement cannot remove other platforms from an existing release.
