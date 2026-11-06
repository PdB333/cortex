# Releasing Cortex

A release is published only after a maintainer's explicit approval. Nothing
in CI publishes on its own.

## How the release workflow behaves

`.github/workflows/publish-release.yml`:

- **On a pull request or a manual run** it builds the x64 and x86 stages,
  assembles `cortex-v2.0.0-windows-portable.zip` and validates it, then keeps
  it as a workflow artifact. It publishes nothing.
- **It publishes only on a push to `main`** whose head commit message
  contains `Release v2.0.0`. Then it extracts the `## [v2.0.0]` section of
  `CHANGELOG.md` as the release notes and runs `gh release create v2.0.0`.
- **It refuses to overwrite** a release that already exists.

The archive is checked before it is zipped:

- the runtimes for both architectures and the 32-bit helper are present, and
  no `cortex_core.dll` sits at the bundle root;
- `cortex.exe --version` reports 2.0.0, and `--smoke-test` and
  `--window-smoke-test` pass with a bare `PATH`;
- `--help` lists `mcp`, `inspect`, `provider`, `live`, `evidence` and
  `investigation`, and `provider list` works with no engine installed;
- the README, README_INSTALL (naming v2.0.0), CHANGELOG, LICENSE,
  CONTRIBUTING, SECURITY, `docs/` (with the limitations, provider and MCP
  guides and the README images) and `agent/` are included, and every
  relative link and image of the README resolves inside the archive;
- nothing is left over from tests: no runtime tokens, crash reports,
  sessions, test targets or `e2e` folder.

## Before approving v2.0.0

1. Every workflow is green on the commit to release, including the GUI suite
   with `analysis-engines`.
2. The `## [v2.0.0]` section of `CHANGELOG.md` is final, with the release
   date in its heading.
3. The **Desktop acceptance** workflow (`desktop-acceptance.yml`) is green.
   It drives the built `cortex.exe` on the runner's real Windows desktop
   with keyboard and mouse input (`tests/desktop_acceptance.py`):
   - the icon of the executable and of the window (Explorer, taskbar);
   - Ctrl+O, the system file dialog and the recent files;
   - Help > Getting started opened by the shell;
   - attaching from the process picker and a first scan from the keyboard;
   - an MCP client read-only and with `--allow-writes`, next to the desktop;
   - a clean close.

   Its screenshots are in the run's `desktop-acceptance` artifact. Attach and
   scan, and the AI status, are confirmed by reading them: the window draws
   with Dear ImGui and exposes no accessibility tree. The runner is Windows
   Server at 1024x768 and 100 % scale; other scales and multi-monitor setups
   are not covered.
4. `docs/limitations.md` matches what is shipped.

## Publishing (maintainer)

Merge the pull request into `main` (or push the release commit there) with a
head commit message that contains `Release v2.0.0`, for example
`Release v2.0.0: Cortex 2.0`. The workflow builds, validates and publishes.

To release another version, change `kVersion` in `core/version.h`, the tag
and archive names in `publish-release.yml`, the version in
`README_INSTALL.txt`, and the CHANGELOG section heading.
