# Project instructions

- Always require user permission before merging PRs.
- IDA Pro 9.4 MCP is available. For game modding or reverse engineering, read
  and briefly name the relevant installed universal-modder skills, especially
  mod-any-game, game-recon and reverse-engineering.
- Use Tera Ultra subagents for game testing, unless the user explicitly
  authorizes an available substitute. Keep test data isolated from real saves
  and settings.
- Code in Rust whenever possible; retain existing language/ABI boundaries when
  that is the appropriate implementation route.

## Releases

Before publishing or replacing release artifacts, read and follow
[docs/RELEASING.md](docs/RELEASING.md). A target-runtime shader compilation or
startup failure blocks that target's release. Windows/WARP, installer `--help`
and archive validation do not replace a cold-cache Proton/DXVK game run.

## Windows worktree cleanup safeguard (2026-09-30)

- Before `archive_worktree`, `git worktree remove`, or recursive deletion of
  any Windows worktree, scan the root, ancestors and all descendants for reparse
  points without following links. Use
  `K:\Anything\worktree-archive-audit-20260930\check-worktree-reparse-points.ps1`
  with `-WorktreePath <absolute path>` or an equivalent complete non-following scan.
- Do not archive or recursively remove a worktree containing any junction,
  symlink or other reparse point, or when the scan is incomplete. Leave it in
  place until link handling is separately reviewed and safely resolved. Do not
  use force or another deletion tool as a workaround.
- A blocked link-removal command does not authorize `archive_worktree` as an
  alternative. Preserve recovery copies and logs.
- Git 2.49.0.windows.1 locally emptied five junction targets outside a checkout
  during `git worktree remove --force`. This guidance is a precaution, not a fix
  to Git or the desktop app, and does not control automatic/UI cleanup.
