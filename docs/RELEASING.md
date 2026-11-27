# Releasing XPTablet

Releases are made by the "SkunkCrafts release" workflow (.github/workflows/skunkcrafts-release.yml), started by hand. From a
Claude Code session the GitHub CLI's `gh workflow run` is blocked (it uses GraphQL); start it through the REST API instead.

1. Put the new version in `src/version.h` (`XPT_VERSION`), and the notes for it in `docs/RELEASE_NOTES.md`. Push to main.
2. Wait for that commit's `build-plugin` run to pass (all three platforms and the bundle).
3. Dry run (checks everything, publishes nothing):
   `gh api -X POST repos/domvid2005/XPTablet/actions/workflows/skunkcrafts-release.yml/dispatches -f ref=main`
4. Publish (SkunkCrafts Updater users get it at once, then the GitHub release with the zip is made):
   `gh api -X POST repos/domvid2005/XPTablet/actions/workflows/skunkcrafts-release.yml/dispatches -f ref=main -f 'inputs[publish]=true'`
5. Check: the run's summary, the `release` branch's `XPTablet/skunkcrafts_updater.cfg` (its `version|` line), and
   `gh api repos/domvid2005/XPTablet/releases/tags/v<version>`.

Inputs: `version` (empty: the one in src/version.h), `commit` (empty: the newest on main), `github_only` (only the GitHub release, for
a version already on SkunkCrafts). The same workflow runs from GitHub: Actions > SkunkCrafts release > Run workflow.

A version can be released once: the workflow stops if the release branch already has it. The release branch only gets new commits.
