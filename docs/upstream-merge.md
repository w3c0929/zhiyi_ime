# 从上游合并与发布：维护铁律

本仓库是 [w3c0929](https://github.com/w3c0929) 维护的 fork，产品名 **知语输入法 / Zhiyu IME**。
它派生自上游 **知意输入法 / Zhiyi IME**（`dd1000001000/zhiyi_ime`），上游又派生自 `deanxyuan/cxx-ime`。

**核心目标：本 fork 必须能与上游「知意输入法」在同一台机器上并存、互不影响。**
凡是会破坏「身份隔离」「署名」「本 fork 自制功能」的改动，一律不做。

远端：`origin` = `w3c0929/zhiyi_ime`，`upstream` = `dd1000001000/zhiyi_ime`。
长期分支：`feat/translate-selection` 与 `main` 保持同点（一起推）。

本文件是维护铁律；做「从上游合并」或「发版」前先读它。

---

## 一、红线：绝不修改

这些是「身份 / 署名 / 磁盘格式 / 上游引用」，改了会破共存或失真：

- 版权头：`Copyright (c) 2026 Zhiyi IME Contributors…`、`Modified by Zhiyi IME Contributors:`
- 上游与仓库链接、署名：`w3c0929/zhiyi_ime`、`dd1000001000/zhiyi_ime`、`dd1000001000/zhiyi-glossary`、
  `deanxyuan/cxx-ime`；`README*` / `NOTICE` / `THIRD_PARTY_NOTICES.txt` 里「based on 知意输入法」的表述
- 磁盘格式标识（兼容旧数据，不能改）：`zhiyi-wordlist`、`zhiyi-user-backup`、`zhiyi_prompt`、`zhiyi-mt-1`
- 环境变量 `ZHIYI_UPDATE_TEST_BASE`；`.gitignore` 的 `/scripts/zhiyi-setup.nsi`
- 上游基准图表 `docs/media/compare.zh-CN.svg`、`translator.zh-CN.svg`（其中「知意 1.1/1.2/1.5.4/v2」是**上游版本标签**）
- 语言示例 `engine/src/word_list_read.cc` 里的 `zhi'yi → 知意`（“知晓其意”，不是产品名）
- **用户的财产**：scoop bucket 里上游版 `zhiyiime.json` 必须与 `zhiyuime.json` **并存**，且两份结构
  完全对齐（字段顺序、autoupdate、installer/post_uninstall 步骤、三处 roots 等）。只做**新增 / 并列修改**，
  **绝不改名、替换或删除** `zhiyiime.json`。

## 二、本 fork 的身份（共存核心，别改回去）

| 项 | 值 |
|---|---|
| TSF CLSID | `{94f5f388-9732-439e-a73f-ebc4e6f4a64b}` |
| TSF Profile GUID | `{a549be94-7073-440d-b293-acf8820a527b}` |
| 注册表键 | `Software\ZhiyuIME` |
| 数据目录 / 本地缓存 | `%USERPROFILE%\zhiyu` / `%LOCALAPPDATA%\zhiyu` |
| 互斥体、窗口类、日志前缀 | `ZhiyuIME*`、`[ZhiyuIME]` |
| 产物名 | `zhiyu-server.exe`、`zhiyu-settings.exe`、`zhiyu_tsf_x64\|x86.dll`、`zhiyu_ime_x64\|x86.ime`、`zhiyu-resources.dll`、`zhiyu-installer-helper.exe` |
| 用户可见名 | 知语输入法 / Zhiyu IME |

定义处：`shared/include/cxxime/text_service_profile.h`（CLSID/Profile）、`shared/src/data_path.cc`（路径/注册表）、
`data/ui.*.json`（界面串）、`resource/cxxime-resources.rc` + `tsf/src/resource_loader.cc`（IME 显示名）、
`shared/include/cxxime/settings_route.h`（设置窗口标题）。
图标：`resource/zhiyu.ico`（由 `resource/make_icons.py` 生成，`.rc` 引用此名）。

## 三、本 fork 自制功能（合并上游时不可破坏）

- **划词翻译**：快捷键 `Ctrl+C+C`。协议见 `shared/include/cxxime/ipc_protocol.h`：
  `IPCCommand::TRANSLATE_SELECTION = 35`、`TRANSLATE_STATUS = 36`，`IPCRequest.translate_text`，
  `IPCResponse.translate_state / translate_text`；配置见 `engine/include/cxxime/config.h` 的
  `translate_selection_shortcut / translate_direct_insert / translate_device / translate_idle_seconds`。
  **译成哪种语言跟「学习」页走**：源语言按文字系统猜（`server/src/translate_service.cc`），目标取
  `Config::gloss_targets` 那一行的语言（`learning.gloss_targets`），空则回落「中→英、其他→中」。
  **合并上游时若上游也改 `ipc_protocol.h`，先确认它新增的枚举值 / flag 位不与 35、36 及我们已用的位冲突。**
- **在线 AI（设置「AI」页，默认关闭）**：`shared/include/cxxime/ai_config.h`（配置）、
  `shared/src/ai_client.cc`（约 30 家提供方的协议表、DPAPI 加密的多密钥、`list_models()`、
  `thinking_levels()` 与 `apply_thinking()` 按服务商映射「关思考」字段）。
  `server/src/translate_service.cc` 让划词翻译优先走线上、失败回退本地；语音识别后处理在
  `server/src/voice_service.cc` 的 `finish_recording()` 里插一步（`ai.use_for.voice` 开时把识别文字交给
  `proofread` 动作过一遍再上屏，失败/超限一律退回识别原文，录音与按键流程不动）。
  数据文件 `data/ai_thinking_levels.json`。**默认关，关着时行为与从前完全一致**——合并时别把它改成默认开。
  用途开关是 `ai.use_for.*`（translate / selection / voice / glossary），**没有 english**（1.7.36 删掉）。
- **选中文字的 AI 动作（翻译 / 润色 / 纠错 / 改写 / 摘要）**：动作定义是数据（`data/ai_actions.json`，
  读取器 `shared/src/ai_actions.cc`，三端共用：服务端取提示词、TSF 取名字画菜单、设置页列行）；
  提示词调用 `ai_client` 的 `run_action()`（含去引号/去「以下是…」包装）；服务端
  `server/src/ai_action_service.cc`（单 worker 线程、`owner_`/`generation_` 失效、结果留内存供分块取回、
  失败给可读原因 `kTranslateFailed`）；TSF 侧在 `tsf/src/text_service_translate.cpp`（菜单、写回、长选区提示）。
  结果的去向由动作自己的 `insert` 决定：`replace` 替换选区、`below` 写在选区**下方**
  （`EditSession::Action::INSERT_TEXT_UNDER_SELECTION`，`tsf/src/edit_session.cpp`）、`preview` 只显示。
  **`replace` 是粘贴送回的**（剪贴板 + `Ctrl+V`，`paste_text()`），不是 TSF 写入：TSF 写入在有些程序里
  根本不进撤销栈（Win11 Notepad 的 `Ctrl+Z` 会把整个文档清空，1.7.56 实测），粘贴才是各处都能一步撤销的写法。
  开关是 `ai.use_for.selection`，键是 `ai.action_key` + `ai.action_keys`（都是「第二个键」，第一个键是划词翻译键）。
- **`\fy` 换翻译语言**：**没有自己的窗口/按键/IPC 命令**——`\fy` 是一次普通符号组字，候选换成 12 种语言
  （`Engine::ai_language_result`，`engine/src/engine.cc`；候选带 `AiLanguageAction`，`candidate_selection.h`），
  选中走 `Engine::dispatch_candidate_selection` → `Context::choose_ai_language()`（记下语言、结束组字、不上屏），
  服务端 `SessionManager::persist_ai_language()` 取走并写 `ai.translate_language`。引擎识别在
  `engine/src/symbol_processor.cc`（`kAiLanguageCommand`，见 `symbol_table.h`）。
  **坑（以后加 UiCommandType 或 IPCCommand 时记住）**：1.7.32–1.7.33 这一功能走过"另发一条 UI 命令"的老路，
  连着被两道关口静默丢掉——`ipc/src/ui_protocol.cc` 的 `valid_command_type()` 白名单（没登记 = 发不出去）和
  `decode_ui_command_packet()` 里 `> kLastKnown` 的上限（没抬高 = 收到也当"更新的服务端发来的"扔掉）。
  现在 `kLastKnown` 与白名单在 `ipc/include/cxxime/ui_protocol.h` 同一处注释里互相点名；**能不加协议就不加**，
  `\fy` 的重做就是例子。
- **长文本分块通道**：`TEXT_UPLOAD_BEGIN = 37` / `TEXT_UPLOAD_CHUNK = 38` / `TEXT_RESULT_POLL = 39` /
  `TEXT_RESULT_READ = 40`（`ipc_protocol.h`，**零结构体改动**，靠 `key_code`/`candidate_index`/`modifiers`/
  `search_query`/`translate_text` 的按命令含义；`text_result_bytes` 是追加在 `IPCResponse` 末尾的新字段）。
  划词翻译与 AI 动作共用：短文本走 35/36 老路，超 4095 字节才启用它。
  **合并上游时注意**：它一旦也往 `IPCRequest`/`IPCResponse` 里追加字段，`text_result_bytes` 之后的偏移要一起核对。
- 五页设置（常规 / 按键 / AI / 词库 / 关于）+ 学习页。

## 四、从上游合并的流程

1. `git fetch upstream --tags`；`base=$(git merge-base HEAD upstream/main)`；
   `git log --oneline $base..upstream/main` 列出待合并提交。
2. **上游常有「提交 + 回退」成对出现**（例：`c77a7f5` 与 `e97dd9a` 净效果为零）。
   先 `git diff <较早> <较晚>`（或 `git diff $base..upstream/main`）看**净改动**，别无脑全合。
3. 逐个 `git show --stat` / `git show <sha>` 评估与本地功能的冲突点。
4. 工作区通常有大量未提交改动 → **不要用 `git cherry-pick`**（因 dirty tree 会失败或搞乱）。
   改为**手动把上游改动套进工作区**，保留本地品牌与自制功能。
5. 套用时同步品牌：`zhiyi→zhiyu`、`知意→知语`、`知意输入法→知语输入法`、`Zhiyi IME→Zhiyu IME`
   （红线清单里的除外）。
6. 若上游带来**新的双拼方案或词典数据**，**必须重新生成词典包**：
   ```
   python scripts/prepare_dictionary_bundle.py --data-dir data --output-dir dist-data \
       --topn-builder build-x64\tools\topn_index\Release\topn_builder.exe
   ```
   否则 `package_release.py` 的二进制数据取自「参考安装目录」，不会带上新数据，用户选了会**静默回退**。
7. 构建验证（都要 exit=0）：
   ```
   cmake --build build-x64 --config Release
   cmake --build build-x86 --config Release --target cxxime-tsf cxxime-ime
   ```
   （x86 **不**构建 server；`cmake --build build-x86` 默认会连 server 一起编并失败。）

## 五、发布流程（已走通）

1. 改 `VERSION`（并加 `docs/release-notes/<版本>.{zh-CN,en-US}.md`）。
2. 构建（见第四节第 7 步）。
3. `python scripts/package_release.py --reference "<上一版已安装目录，或 7z 解出的安装包>" --version X --out dist`
4. 清理产出里残留的**上游命名文件**（`zhiyi-server.exe`、`zhiyi_tsf_*.dll`、`zhiyi_ime_*.ime`…）；
   把重新生成的 `pinyin.*`、`wubi86.*`、`dictionary_manifest.json` 拷进 `dist\zhiyu-vX\data\`。
5. `makensis /INPUTCHARSET UTF8 /DVERSION=X "/DSRCDIR=<dist 绝对路径>" scripts\zhiyu-setup.nsi`
   （脚本是 UTF-8 无 BOM，**必须**带 `/INPUTCHARSET UTF8`）
6. `python scripts/make_release.py --version X --installer scripts\zhiyu-vX-setup.exe --notes-zh … --notes-en …`
7. 更新 `scripts/zhiyu-scoop.json` 的 version/url/hash，并**一字不差**同步到 bucket
   `E:\09.同步\06.配置\myscoop\bucket\zhiyuime.json`。
8. `gh release create vX -R w3c0929/zhiyi_ime scripts\zhiyu-vX-setup.exe scripts\latest.json scripts\latest.json.sig --title vX --target main --notes-file <说明文件>`
   （`gh` 默认会认到 upstream 仓库，**务必带 `-R w3c0929/zhiyi_ime`**）。正文沿用上一版的格式：
   「知语输入法 vX」+ 中文要点 + 空行 +「Zhiyu IME vX」+ 英文要点 + 末尾一行
   `Install: run \`zhiyu-vX-setup.exe\` as administrator, or \`scoop install zhiyuime\`.`
9. 提交并推送：`feat/translate-selection` 与 `main` 一起推；bucket 仓库提交后推
   （远端常有自动更新提交 → 先 `git pull --rebase origin main`）。
10. **只给最终版本建 Release**：中间的过程/测试构建**不建** tag 与 Release（用户明确要求，
    否则 Releases 列表被测试版淹没）。`latest.json` 每次发布**重新生成**（`make_release.py` 会连同
    `latest.json.sig` 一起重签并写入 version/size/sha256 与中英说明），别沿用上一版的旧文件。
11. 发布后自查（三条都过才算发成功）：
    - `gh api repos/w3c0929/zhiyi_ime/releases/latest --jq .tag_name` → `vX`；
    - 三个 Release 资产的 sha256 与本地 `scripts\` 下同名文件一致（`latest.json` 必须是刚生成的那份）；
    - `raw.githubusercontent.com/w3c0929/myscoop/main/bucket/zhiyuime.json` 的 version/hash 与本地一致。
12. 收尾清理：`scripts\zhiyu-v*-setup.exe` 只留**已发布版本**与最新版，中间测试包删掉
    （每个约 129 MB）；`dist\` 同理。

## 六、发布/安装前必查的坑

- **改了 `VERSION` 后一定要让配置跟上再编译**：`version.h` 与每个目标的 `*_version.rc` 是 `configure_file` 生成的，
  构建系统**不保证**会自动重跑 configure（实测 1.7.56：`cmake --build` 只重编了被改动的那个文件，`version.h`
  仍是 1.7.55，于是包里的 `zhiyu-server.exe`/`zhiyu-settings.exe`/… 内嵌版本全是 1.7.55.0，设置页与「检查更新」
  会永远提示有新版本）。**先 `cmake -S . -B build-x64`（以及 `-B build-x86`）再 build**；`scripts/package_release.py`
  现在会核对包里每个 `zhiyu*` 程序的内嵌版本，对不上直接 exit 1，兜住这条。
- **NSIS 开始菜单位置**：elevated 安装器必须 `SetShellVarContext all`，否则菜单项落到「当前用户」
  （`%APPDATA%\…\Start Menu`）而不是「所有用户」（`%ProgramData%\…\Start Menu`）。
- **升级前先停 `zhiyu-server`**：否则旧目录里的 DLL 被正在运行的 App 映射，卸载删不干净，
  留下 `D:\scoop\persist\zhiyuime\zhiyuime.busy-*` 残留（关掉这些 App 或重启系统后才能删）。
  **另一条更隐蔽的后果**（2026-10-17 实测）：升级会**退休**旧版本目录（连它的 `data\` 一起删），
  而还在跑的旧 server 以自己那个目录为家 → 读不到包内 `default.json` → **拒绝一切保存**
  （设置页报 "后台服务没有采用这份配置…The data is invalid. (13)"，手输配置也写不进去）。
  1.7.60 起服务端会自愈：读不到自己的包内配置时改用注册表 `InstallLocation` 指向的那份
  （`server/src/config_store.cc` 的 `relocate_package_paths`，并在 `problems.log` 里留一行）；
  **但升级/测试时仍应先停 server**，别指望自愈兜住一切。
- `package_release.py` **只覆盖文本配置**（`DATA_OVERLAY`），二进制数据来自参考目录；
  参考目录里上游命名的 `zhiyi-*` 会被 `File /r *.*` 打进包，务必删除。
- 产物名以 `package_release.py` 的 `BUILT` 表为准（如 `zhiyu_tsf_x64.dll`）；
  改 CMake `OUTPUT_NAME` 时（`server/settings/tsf/legacy_ime/resource/installer` 各 `CMakeLists.txt`）要一起核对。
- **`.ime` 必须复制进系统目录**：`zhiyu_ime_x64.ime` → `%WINDIR%\System32\zhiyu.ime`，
  `zhiyu_ime_x86.ime` → `%WINDIR%\SysWOW64\zhiyu.ime`（注册表里的
  `SYSTEM\CurrentControlSet\Control\Keyboard Layouts\E0210804` 指向 `zhiyu.ime`）。缺了文件**输入法加载不起来**，
  看起来就是「装完了打不开」。（32 位安装器写 System32 会被重定向，需 `DisableX64FSRedirection`。）
- **安装信息必须写 HKLM 的 64 位视图**：`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyuIME`
  的 `InstallLocation` = **活动版本目录**（不带尾斜杠）。`shared/src/server_launcher.cc`、
  `settings_launcher.cc` 都从那里读（`KEY_WOW64_64KEY`），只写 HKCU 会让
  `start_server_on_demand()` 找不到 server——**后台服务永远不会被拉起**。
- **要写自启动项**：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ZhiyuIMEServer` =
  `"<版本目录>\zhiyu-server.exe" --autostart`（`startup.autostart` 为 false 时该进程立即退出）。
- **多版本布局**：`<基目录>\<版本号>.<8位十六进制>\`（`installer_lifecycle_state.cc` 的 `allocate_target()`）
  + `<基目录>\maintenance\install-state.json` + 版本目录内的 `install-manifest.json`
  （`package_release.py` 生成）与 `uninstall.exe`。升级装进**新**目录，不覆盖运行中的版本
  （这正是以前升级出现 `zhiyuime.busy-*` 残留、文件被锁的根源）。
- **卸载器里 `$INSTDIR` 不是基目录**（是版本目录）：`RMDir /r "$INSTDIR"` 只会删掉版本目录，
  基目录里的 `maintenance\` 会留下，卸载后 `persist\zhiyuime` 不干净（与上游不一致）。
  要从注册表的 `InstallLocation` 取活动版本目录、用 `${GetParent}` 反推基目录再删
  （`FileFunc.nsh`）。改完必须**实测一次装+卸**，看基目录是否整个消失。
- `.nsi` 的注释**不能以 `\` 结尾**：NSIS 视作续行，会把下一行整行吃掉（编译只给一条 warning）。
  例：`Var BaseDir ; … maintenance\` 会吞掉后面的 `!macro` 行。
- **加设置字段必同步 `KEEP_PAGE_EDIT`**（`settings/editor_app.cc`）：那是**逐字段手抄**的「本页自管、
  保存时不覆盖」白名单，漏一个字段的症状是「改完保存又变回旧值」或「删掉的条目自己回来」
  （磁盘值把界面值盖掉）。已漏过 `ai.thinking`、`ai.keys_by_provider`。
  根治办法是按页面标脏（AI 页控件 ID 段 6000–6020），别再加手抄清单。
- **设置控件类型**：文本输入框要用自建的 `make_text_edit`；`make_edit` 带 `ES_NUMBER`，只收数字。
  另注意 `kCtrlH` 一类常量**已是缩放后的值**，用的时候别再套 `S()`。
- **新增的文本数据文件必须进 `DATA_OVERLAY`**（`scripts/package_release.py`）：例 `data/ai_thinking_levels.json`。
  漏了它就不会被打进安装包，运行时读不到（静态表/等级列表静默为空）。
- **JSON 一律 UTF-8 无 BOM，`bucket\` 下的文件尤其不许有 BOM**（用户的明确要求，2026-10-14）。
  写文件要用 `[IO.File]::WriteAllBytes` 或 `UTF8Encoding($false)`：`Out-File` / `>` 在 PS 5.1 会写成
  UTF-16，`Set-Content -Encoding UTF8`（5.1）也会**加** BOM。实测 `scoop info` 读无 BOM 的 manifest
  中文完全正常（上游 `zhiyiime.json` 本来也没有 BOM），反倒是 PS 5.1 的 `ConvertFrom-Json` 会被 BOM
  绊倒（`Invalid JSON primitive`）——**早前"必须带 BOM"那条结论是错的，已作废**。
- **scoop 升级失败 exit code 13** = 旧的 `zhiyu-server` 还在跑、文件被占用（签名/覆盖写不进去）。
  bucket 的 `installer.script` 已改成「读 HKLM `InstallLocation` → helper `force-stop-server` →
  `Get-Process zhiyu-server \| Stop-Process -Force` 兜底 → 安装 → 回读注册表校验」，别退回只 `Start-Process` 的旧写法。
- **改本仓库文件的换行**：本仓库多处是 **LF**；用 PowerShell 做整段替换时，LF 与 CRLF 两种匹配都要试，
  否则替换静默不生效（或把 CRLF 写回 LF 文件里造成混杂）。单引号 here-string 里的反引号会**原样写入**
  （曾导致 `.cc` 里出现 `0x60` 而编译失败）。

## 七、模型资产（glossary / translator / voice）

`update.h` 把语言包、翻译模型、语音模型的下载指向**本仓库**的三个 Release（`kDownloadPrefix` + `glossary/`、
`translator/`、`voice/`）；上游的对应资产在 `dd1000001000/zhiyi_ime` 的同名 Release 里。

- **不能把地址指回上游**：那些清单是**上游私钥**签的，而程序内置的是本仓库公钥
  （`update/src/update_public_key.inc`），签名验证不过会被拒。
- 正确做法：下载上游资产 → **丢弃上游的 `*.json.sig`** → 用本仓库私钥重签
  （`python scripts/update_signing.py sign update-signing-key.pem <清单>`）→ 在本仓库建同名 Release 上传。
- **必须 `--latest=false`**（`gh release create … --latest=false`）：否则会抢走 `/releases/latest`，
  更新检查就会指向模型 Release。**忘了带的话**：`--latest=false` 在**已发布**的 release 上不生效
  （2026-10-17 实测：`gh release edit <tag> --latest=false` 与 `PATCH … make_latest=false` 都不动它），
  要用 `gh release edit <tag> --prerelease` 才能把它从 `/releases/latest` 上摘下来；摘完用
  `gh api repos/w3c0929/zhiyi_ime/releases/latest --jq .tag_name` 确认回到了版本 tag，并确认
  `https://github.com/w3c0929/zhiyi_ime/releases/latest/download/latest.json` 能取到（HTTP 200）。
- 清单里**没有 URL**（`parse_*_manifest` 用 `download_prefix` 自己拼），所以上游清单内容可原样复用，只重签。
- 现有内容：`glossary`（28 个 `.gloss` + `glossary.json`）、`translator`（`zhiyi-mt-v2-Q4_K_M.gguf` 396 MB 与
  v3 的 gguf 322 MB，用户可在「学习」页选；`llama-b11480-vulkan-x64.zip` 32 MB）、`voice`（`model.int8.onnx`
  239 MB + `tokens.txt` + `silero_vad.onnx` + 两个 LICENSE 文件）。
- **Laya 推荐模型是例外**：它不走上面那套签名清单，而是**随安装包发**（打包时模型文件来自参考安装目录），
  `scripts/fetch_model.py` 另外把它下到 `models/laya/` 供源码构建用——那个脚本指向**本仓库**的
  `model-zhen-long-l18` Release（上游 1.5.9 起的默认模型，文件逐字节相同）。换模型 = 转存上游资产到本仓库
  同名 Release → 改 `fetch_model.py` 的 ASSET/URL/SHA 与 `FILES`/`OLD_FILES` → 换掉参考目录里的模型文件
  再打包（**不需要**重签清单、也不用重新生成词典包）。

## 八、发布前必须等用户亲自测试

**构建完、提交推送完就停**：把「装哪个包、怎么验」告诉用户，**等他实机测试并明确说可以发布**再发
（建 Release、传资产、更新 bucket 都算发布）。这是用户的明确要求，别再自动往下走到发布。

范围提醒：本 fork 的联网面只有两处——**设置程序的更新检查/模型下载**，以及**可选的在线 AI**
（`ai_config.h`，默认关，只有 `zhiyu-server` 用）。改动若扩大联网面，要一并更新 `docs/privacy.md`
与 `update.h` 的说明。

## 九、不要提交

`build-x64/`、`build-x86/`、`dist/`、`scripts/*-setup.exe`、`scripts/latest.json`、`scripts/latest.json.sig`
（均已写入 `.gitignore`）。

## 十、环境

- 构建目录：`build-x64`（全量）、`build-x86`（只构建 `cxxime-tsf` / `cxxime-ime`）。
- 工具：`makensis`（scoop `nsis`）、`gh`（已登录账号 w3c0929）、`7z`（scoop）、Python 3.12、CMake、MSVC。
- 安装验证要点：`HKLM\SOFTWARE\Microsoft\CTF\TIP\{94f5f388-…}\LanguageProfile` 下应有「知语输入法」；
  `HKCU\Software\ZhiyuIME`、卸载项 `…\Uninstall\ZhiyuIME` 应存在；`zhiyu-server` 与上游 `zhiyi-server`
  应能同时运行、数据目录互不相干。
