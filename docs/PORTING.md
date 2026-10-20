# Native desktop port

Parity baseline: `umeiko/umeko-desktop` commit `f648e18b0c8ef86c65363dc78d99223ef0276fcf` (v0.2.0).

The desktop controller, local preferences, session orchestration, clipboard, key management, package import and syntax highlighting run in C++/Qt. The existing QML frontend and image assets preserve the welcome interactions, PID tab highlight, resizable window/sidebar, selection and activity folds. Pi remains the upstream Node.js coding engine. A small Node transport owns HTTP/SSE conversion; it does not replace Pi's agent loop or tools.

Account providers keep API v1 of `umeko-provider-sdk`. An external Python 3.10+ interpreter runs a provider only when requested. The app itself and ordinary API providers do not depend on Python or PySide. Global and per-account `python_path` settings select the executable. Provider credentials pass through private pipes and the system credential store, never command-line arguments or QML.

Implementation and verification checkpoints are tracked in the source tests and release build reports. Private account implementations, internal endpoints and live credentials do not belong in this repository.
