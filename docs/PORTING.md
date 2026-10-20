# Native desktop port

Parity baseline: `umeiko/umeko-desktop` commit `f648e18b0c8ef86c65363dc78d99223ef0276fcf` (v0.2.0).

The desktop controller, local preferences, session orchestration, clipboard, key management, package import and syntax highlighting run in C++/Qt. The existing QML frontend and image assets preserve the welcome interactions, PID tab highlight, resizable window/sidebar, selection and activity folds. Pi remains the upstream Node.js coding engine. A small Node transport owns HTTP/SSE conversion; it does not replace Pi's agent loop or tools.

Account providers keep API v1 of `umeko-provider-sdk`. An external Python 3.10+ interpreter runs a provider only when requested. The app itself and ordinary API providers do not depend on Python or PySide. Global and per-account `python_path` settings select the executable. Provider credentials pass through private pipes and the system credential store, never command-line arguments or QML.

Implementation and verification checkpoints are tracked in the source tests and release build reports. Private account implementations, internal endpoints and live credentials do not belong in this repository.

## Parity and verification

| Area | Implementation | Automated evidence |
| --- | --- | --- |
| Native agent | Full upstream Pi SDK, coding tools and resource loaders | Real local model → native write tool → persisted Pi JSONL; asserts all ten enabled tool names |
| Chat lifecycle | Per-session processes, stable model rows, LRU idle retirement, unread metadata, history and queues | Concurrent background conversations, follow-up promotion to steering, cold continuation after the old model is retired |
| Interface | Shared baseline QML; native bounded highlighting | Real Qt window, wheel scrolling, cross-block copying, collapsible tool/thinking rows, file-path clipboard |
| Animation and artwork | Shared PID controller and mascot interaction code; lossless WebP assets | Rapid PID retarget, click/hold/drag spring return, all twelve portraits loaded; decoded-pixel equality manifest |
| Providers | Public SDK v1; external Python worker; global and account interpreter override | Malicious ZIP traversal rejection, echo ZIP import, interpreter probe, protected login/model discovery and real Pi inference through the worker |
| Pi management | Native resources adapter, extension toggles, MCP JSON validation, original CLI and factory runtime retained | Toggle builtin MCP off/on and validate/save MCP configuration; installed bridge self-test |
| API proxy | HTTP/SSE OpenAI/Anthropic transport, loopback/LAN binding, SHA-256 key verifiers | Real HTTP streams, multimodal/tool conversion, key revocation, private gateway separation, malformed input and occupied-port rollback |
| Distribution | Native Qt deployment, complete Pi modules, offline fd/rg, public SDK and Skill | Installed app starts after removing SDK search paths; installed Pi loop and search executable version checks |

Factory reset keeps original settings and backs up its pointer before selecting a fresh verified factory runtime. Kernel updates are staged and self-tested before their active pointer changes; the factory's search binaries are carried forward. Imported provider code executes only for login, discovery or inference. The worker is a process boundary, not a security sandbox.

Desktop-specific IME composition, system dialogs, keychain permission prompts, extension packages from arbitrary third parties and every hardware graphics driver remain manual acceptance areas. CI uses the real OS credential service with synthetic accounts and never authenticates to an enterprise deployment. It cannot certify every third-party plugin or model's declared context/vision capability.
