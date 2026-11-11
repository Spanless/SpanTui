# Contributing to SpanTui

Thanks for your interest in SpanTui. Issues and pull requests are welcome in English or Chinese. The developer documentation under `docs/` is written in Chinese.

感谢参与 SpanTui。Issue 与 Pull Request 可以用中文或英文；`docs/` 下的开发文档以中文为主。

## Before you start / 开始之前

- Search existing issues first. For a larger change, open an issue to discuss the design before writing code.
- Set up the toolchain described in [docs/workflow/build.md](docs/workflow/build.md) (Qt 6.12 MinGW, CMake, Ninja), then run `dev.bat build`.
- Read the [architecture overview](docs/architecture/overview.md). The code has six layers with one-way dependencies, and each piece of state has a single owner ([state ownership](docs/architecture/state-ownership.md)).

## Branches / 分支

`dev` is the default branch and the development line (beta releases); `master` is the stable line (stable releases) and only takes pull requests from `dev`. Branch off `dev`, and open your pull request against `dev`. Neither branch accepts direct or force pushes ([branch model](docs/workflow/release.md#1-分支模型)). Pull requests into `dev` are squash-merged; `dev` goes into `master` with a merge commit.

`dev` 是默认分支与开发主线（发布 beta 版），`master` 是稳定线（发布稳定版），只接受来自 `dev` 的 Pull Request。从 `dev` 切出功能分支，向 `dev` 提 Pull Request。

## Making a change / 修改流程

1. Find the docs that own the files you plan to touch:

   ```powershell
   tools\check-ownership.ps1 -Files <paths you plan to change>
   ```

   Read the listed docs. When your change alters behavior, an interface, configuration or a storage format, update those docs in the same pull request.
2. Keep the change focused. Migrate every caller when you replace something; do not leave the old and new paths side by side.
3. User-facing strings go through `qsTr()`. Add the Chinese translation to `translations/SpanTui_zh_CN.ts` in the same change ([localization](docs/workflow/localization.md)).
4. Database schema changes go only through `WorkspaceDatabaseMigration` ([database schema](docs/services/storage/database-schema.md)).
5. Tests assert public behavior and real side effects, run headless (offscreen), and use isolated fixtures for data directories, Git remotes and PTYs ([testing](docs/workflow/testing.md)).

## Commit messages / 提交信息

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`, for example `fix(git): drop stale history results` or `docs(workflow): describe the format check`. Common types are `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build` and `chore`. Since pull requests into `dev` are squash-merged, the pull request title becomes the commit on `dev`, so give it the same form.

提交信息与 PR 标题使用 Conventional Commits 格式（`类型(范围): 摘要`）。进 `dev` 的 PR 以 squash 合并，PR 标题就是最终的提交信息。

## Checks / 检查

While you work, run the static gate and the tests your change affects:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent-check.ps1
dev.bat build
dev.bat test -R <tests affected by your change>
```

GitHub CI runs the gate on every pull request; its `windows` check must pass before merging. Pull requests into `dev` run the checks, the build, the core tests and the tests your change touches (`dev.bat ci --core` locally); after the merge, `dev` runs the full suite (`dev.bat ci`). Both print `CI_OK` when they pass.

C++ lines you add or change must match `.clang-format`; existing code is not reformatted in bulk. `tools\check-format.ps1` checks them (`agent-check.ps1` runs it) and `tools\check-format.ps1 -Fix` formats them in place. Use the clang-format version pinned in `tools/toolchain-versions.json`; pull requests into `dev` fail CI when changed lines are not formatted. `.editorconfig` sets the editor defaults.

`agent-check.ps1` must print `AGENT_CHECK_OK`. A `WARN` means code changed but its owning doc did not; update the doc, or explain in the pull request why the documented behavior is unchanged. `git config core.hooksPath .githooks` runs the static checks on every commit and `dev.bat check` before every push.

Always build and test through `dev.bat`, not by calling `cmake --build` or `ctest` directly ([build](docs/workflow/build.md)).

## Pull requests

- Describe what changed and why, and how you verified it.
- Mention UI changes that need a visual check; include a screenshot when it helps.
- By submitting a pull request you agree that your contribution is licensed under the [MIT License](LICENSE).

## AI coding assistants

[AGENTS.md](AGENTS.md) contains the rules AI coding assistants follow in this repository. Human contributors can use it as a compact summary of the project conventions.
