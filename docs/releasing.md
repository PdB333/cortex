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
2. The `## [v2.0.0]` section of `CHANGELOG.md` is final. Replace "not yet
   released" with the date.
3. Someone has tried the archive on a real Windows desktop:
   - the icon in Explorer and the taskbar;
   - attaching and scanning;
   - an MCP client with and without `--allow-writes`.

   CI cannot check these.
4. `docs/limitations.md` matches what is shipped.

## Publishing (maintainer)

Merge the pull request into `main` (or push the release commit there) with a
head commit message that contains `Release v2.0.0`, for example
`Release v2.0.0: Cortex 2.0`. The workflow builds, validates and publishes.

To release another version, change `kVersion` in `core/version.h`, the tag
and archive names in `publish-release.yml`, the version in
`README_INSTALL.txt`, and the CHANGELOG section heading.
