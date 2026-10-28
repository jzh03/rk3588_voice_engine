# 版本与正式发布

正式发布统一在 [GitHub Releases](https://github.com/jzh03/rk3588_voice_engine/releases) 提供。当前源码版本为 `0.2.0`，见 [VERSION](../VERSION)、[变更记录](../CHANGELOG.md) 和 [v0.2.0 发布说明](releases/v0.2.0.md)。

`v0.2.0` 的发布日期为 2026-10-28，正式入口为 [v0.2.0 Release](https://github.com/jzh03/rk3588_voice_engine/releases/tag/v0.2.0)。下载附件与标签身份以该页面为准；以下维护者流程供后续版本参考，不用于覆盖已发布的标签。

## 三种版本标识

| 标识 | 用途 |
|---|---|
| `VERSION` 中的 `0.2.0` / Git 标签 `v0.2.0` | 公开源码版本；标签应指向包含许可证、安装说明和版本资料的提交 |
| Git 提交 SHA | 精确标识源码；功能与性能资料的整理基线为 `8a2464afaaba7ac2d5172d7b222060f22913daf4`，不等于最终发布提交 |
| `p7-m2-20261027-r2` 等部署 ID | 一块板上的程序、配置和资产集合，由 `scripts/deploy.py` 管理；公开性能样例对应此产物 |

项目使用 `MAJOR.MINOR.PATCH` 形式，0.x 阶段允许配置或内部接口演进。后续版本应在变更记录中说明升级影响，不移动已经公开的标签。

## 用户获取和安装

从 GitHub Releases 选择 `v0.2.0`，下载项目上传的 `rk3588_voice_engine-0.2.0-source.tar.gz` 与 `SHA256SUMS`。将二者放在同一目录，在板端 Linux 终端执行：

```bash
sha256sum -c SHA256SUMS
tar -xzf rk3588_voice_engine-0.2.0-source.tar.gz
cd rk3588_voice_engine-0.2.0
cat VERSION
```

随后按 [支持平台与系统依赖](supported-platform.md) → [资产准备](assets-setup.md) → [构建与部署](build-and-run.md) 完成安装。仓库源码版本可从 `VERSION` 读取，已安装的本地版本可用 `python3 scripts/deploy.py status` 查询。

源码包包括公开代码、配置模板、清单、固定合成测试材料、许可证和公开文档。它不包含模型权重、运行库实体、私人录音、本机配置、私有实验资料或操作系统镜像；获取资产的网络阶段完成后，语音推理在板端运行。

## 维护者准备

以下命令在板端唯一工程根目录的 Bash 中执行。提交、推送、创建标签和 GitHub 发布属于仓库写操作，按 [协作规范](../AGENTS.md#私有资料与git边界) 取得相应授权后执行；日常文档修改不会自动触发发布。

发布前完成以下检查：

- `VERSION`、变更记录、发布说明与目标标签一致，MIT 许可证和第三方声明齐全。
- 支持组合、模型/运行库来源和安装步骤与本次代码一致；声明尚未完成的空白系统复现，不将其写成已验收。
- 只暂存逐项审阅后的公开文件；检查 `git diff --cached --name-status` 和 `git diff --cached`，保留私有资料的忽略规则。不要用整个工作区打包。
- 运行与变更相关的检查。只改发布资料时使用静态检查；如修改资产准备工具，另运行其必要检查。不要将旧性能快照改写为新发布的重新测试结果。
- 每个新版本发布当天填写实际发布日期，并同步版本号、发布状态和链接；确认这些修改进入最终发布提交。

```bash
git status --short --branch
git diff --check
git diff --cached --check
python3 scripts/verify_performance_snapshot.py
```

以上复算不依赖模型、耳机或 NPU。若需要构建、CTest 或大文件校验，按 [构建手册](build-and-run.md#基础环境检查) 使用共享互斥锁。

## 提交、打标签与生成附件

完成逐项暂存和审阅后，使用已经获得授权的板端 Git 身份操作。不把现有 `8a2464a` 直接标成包含本次新增资料的版本。

```bash
git diff --cached --name-status
git diff --cached --check
git commit -m "Prepare v0.2.0 source release"
```

以下按当前仓库的 `main` 分支发布；任一校验失败即停止。标签存在时，先核对是否属于已发布版本，不覆盖它。

```bash
set -euo pipefail
rk_version=$(cat VERSION)
rk_tag="v$rk_version"
test "$rk_version" = "0.2.0"
test "$(git branch --show-current)" = "main"
test -z "$(git status --porcelain)"
git tag -a "$rk_tag" -m "RK3588 voice engine $rk_tag"
git push origin main
git push origin "refs/tags/$rk_tag"

rk_dist="output/releases/$rk_tag"
mkdir -p output/releases
mkdir "$rk_dist"
git archive --format=tar --prefix="rk3588_voice_engine-$rk_version/" "$rk_tag" \
  | gzip -n > "$rk_dist/rk3588_voice_engine-$rk_version-source.tar.gz"
(
  cd "$rk_dist"
  sha256sum "rk3588_voice_engine-$rk_version-source.tar.gz" > SHA256SUMS
  sha256sum -c SHA256SUMS
)
tar -tzf "$rk_dist/rk3588_voice_engine-$rk_version-source.tar.gz"
```

`git archive` 只导出该标签跟踪的文件；维护者仍需检查归档列表，确保没有误跟踪的私有资料。新建的输出目录拒绝覆盖同名目录，重新准备时先查明既有产物身份。附件不依赖板端已安装服务，也不会改变当前部署。

## 创建并核对 Release

GitHub CLI 已安装且由仓库维护者本人登录时，使用以下命令。正文从文件读取，避免在命令行重写多行 Markdown；`--verify-tag` 要求远端标签已存在。`--draft` 先保存未公开的 Release，便于核对附件和正文。[GitHub CLI 文档](https://cli.github.com/manual/gh_release_create)

```bash
gh release create "$rk_tag" \
  "$rk_dist/rk3588_voice_engine-$rk_version-source.tar.gz" \
  "$rk_dist/SHA256SUMS" \
  --repo jzh03/rk3588_voice_engine \
  --verify-tag --draft \
  --title "v0.2.0 — RK3588 离线语音助手" \
  --notes-file docs/releases/v0.2.0.md
gh release view "$rk_tag" --repo jzh03/rk3588_voice_engine
```

核对标签提交、版本化文档链接、正文及附件后，将草稿正式发布：

```bash
gh release edit "$rk_tag" --repo jzh03/rk3588_voice_engine --draft=false --latest
gh release view "$rk_tag" --repo jzh03/rk3588_voice_engine
```

这是独立的发布操作，采用 [官方草稿发布命令](https://cli.github.com/manual/gh_release_edit)。没有 GitHub CLI 时，也可在 [Releases 页面](https://github.com/jzh03/rk3588_voice_engine/releases) 选择现有标签、粘贴同一说明、上传同一附件，先保存草稿，再发布；步骤见 [GitHub 官方说明](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository)。

发布完成后从公开页面下载源码附件与校验文件，再运行 `sha256sum -c SHA256SUMS` 并核对归档中的 `VERSION`。这验证公开下载物身份，不代替空白系统安装或实机验收。
