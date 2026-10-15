# 在线 AI（可选）

知语默认**完全在本机运行**：推荐、翻译、语音都用随程序发布的本地模型。本页描述的是一层
**可选的**在线能力——把输入法正在处理的那段文字交给一个 API 去处理，**默认关闭**，只有用户在
设置里打开、并且**逐个功能**打开之后才会联网。

> 隐私：一旦打开，被处理的那段文字（例如划词翻译里选中的文字）会离开本机、发送到所选服务商。
> 相关的隐私说明见 [privacy.md](privacy.md)。

## 配置（`%USERPROFILE%\zhiyu\default.json` 的 `ai` 段）

| 字段 | 默认 | 说明 |
|------|------|------|
| `ai.enabled` | `false` | 总开关。关着时行为与没有这个功能时完全一致 |
| `ai.mode` | `prefer_remote` | `prefer_remote`：线上的结果优先，出错（超时/被拒/网络不可达）自动回退本地模型；`remote_only`：只用线上，失败就不显示 |
| `ai.provider` | `deepseek` | 内置提供方表的 id（见 `shared/include/cxxime/ai_client.h` 的 `table()`，约 30 家：deepseek / openai / anthropic / google / kimi / glm / qwen / siliconflow / hunyuan / minimax / 小米 / groq / openrouter / mistral / ollama …） |
| `ai.base_url` | `""` | 空 = 用该提供方自带的地址；也可填自建/代理地址 |
| `ai.model` | `""` | 空 = 用该提供方自带的默认模型 |
| `ai.reasoning_effort` | `""` | 推理档位；空 = 不发这个字段 |
| `ai.timeout_ms` | `8000` | 单次请求超时（1000–120000） |
| `ai.keys` | `[]` | **多把** API 密钥，**DPAPI（当前用户）加密后的 base64**，不是明文。一把被拒（401/429）自动换下一把 |
| `ai.use_for.*` | 全 `false` | 哪些功能允许走线上：`translate`（划词翻译）/ `selection`（选中文字的 AI 动作）/ `voice`（语音识别后处理）/ `glossary`（语言包兜底，尚未实现） |
| `ai.action_key` | `""` | 动作菜单键：**按一下就弹出菜单**（见下节）。可以是 `"F6"`、`"Ctrl+K"`，或轻按修饰键 `"RShift"` / `"Shift"`（任一侧）/ `"Ctrl"`。空 = 关闭 |
| `ai.action_keys` | `{}` | 某个动作自己的键：`{"proofread": "F7"}`，按它直跑该动作、不弹菜单（写法同上） |
| `ai.translate_language` | `en` | 「翻译」动作译成哪种语言（12 种代码；默认与 `translate` 动作的 `languages` 首项一致）。设置页有下拉可选 |
| `ai.action_disabled` | `{}` | 不在菜单里列出的动作：`{"summarize": true}`。没写的都列出 |
| `ai.voice_polish_action` | `proofread` | 语音识别后处理用哪个动作（`data/ai_actions.json` 的 id）；**空 = 不做这一层** |

密钥不写明文：设置页会把输入的密钥用 `CryptProtectData` 加密后存进 `ai.keys`，只有同一台机器的
同一个用户能解回来（`shared/src/ai_client.cc` 的 `protect_secret` / `unprotect_secret`）。

## 支持的协议

`shared/include/cxxime/ai_client.h` 的 `Protocol`：

- `openai-completions` — `POST {base}/chat/completions`（deepseek、openai、kimi、glm、qwen、ollama…）
- `anthropic-messages` — `POST {base}/v1/messages`（claude、minimax、kimi-coding…）
- `google-generative-ai` — `POST {base}/models/{model}:generateContent`
- `mistral-conversations` — `POST {base}/v1/chat/completions`

网络走系统 **WinHTTP**、JSON 用仓库已有的 **nlohmann**，没有引入任何新的第三方依赖。

## 目前接入的功能

| 功能 | 状态 |
|------|------|
| **划词翻译**（`Ctrl+C+C`） | 已接入：`ai.use_for.translate` 打开后由 API 翻译（自然更省内存、也不用下载本地翻译模型），失败自动回退本地模型（`server/src/translate_service.cc`） |
| **选中文字的 AI 动作** | 已接入：翻译 / 润色 / 纠错 / 改写 / 摘要（见下节） |
| **语音识别后处理** | 已接入：`ai.use_for.voice` 打开后，识别出的文字先过一遍「纠错」动作再上屏（见下节）；语音键与录音状态机完全不变 |
| 语言包兜底 | 规划中 |
| 设置界面「AI」页 | 已接入（总开关、密钥、动作菜单键、每个动作的开关、独立键、翻译的语言下拉） |

## 语音识别后处理（`ai.use_for.voice`）

勾上「AI」页的「语音识别后处理」后，语音识别出的文字**先交给 `ai.voice_polish_action` 指定的那个动作
过一遍**（默认 `proofread`，也就是「纠错」），拿回来的文字才是上屏的内容。触发方式、录音、悬浮窗、按键
流程**一个字没动**：语音键照旧开始/结束录音，客户端照旧每 100 ms 问一次状态——只是「识别中」会多停一会儿
（就是这次请求的时间，受 `ai.timeout_ms` 限制），然后上屏的是处理后的文字。

- **哪个动作**：`ai.voice_polish_action`（`%USERPROFILE%\zhiyu\default.json` 的 `ai` 段），值是
  `data/ai_actions.json` 里某个动作的 id，**默认 `proofread`**；**留空 = 这一层不做**（识别文字原样上屏，
  用途开关留着）。想让语音过去「润色」就把这一项改成 `polish`。设置页没有这个下拉，改配置文件即可。
- **插在哪儿**：`server/src/voice_service.cc` 的 `finish_recording()` 里，`tidy()` 之后、把文字存进
  `text_` 之前（那是语音自己的工作线程，占线也不影响录音与按键）。
- **任何一步不顺都退回识别原文**：总开关没开 / 没勾这个用途 / `ai.voice_polish_action` 空着或写了不存在的
  id / 没有密钥 / 请求失败或超时 / 识别文字超过该动作的 `max_input_chars`（8000）—— 宁可原样上屏，也不会把
  用户口述的内容弄丢。
- **用的是同一个动作定义**：提示词与上限都来自 `data/ai_actions.json` 那一项（改文件即生效），所以这一层的
  规矩与「选中文字 → 同一动作」完全一致。

## 选中文字的 AI 动作

选中一段文字，**按一下「动作菜单键」**即可（默认不设；可以是 `F6` 这类单键、`Ctrl+K` 这类组合，或
**轻按**右 Shift）。选中内容由输入法自己读取——TSF 读不到的程序里（老式 Win32 编辑框等）它会替你按一次
`Ctrl+C` 再取剪贴板——所以**不必自己先按 Ctrl+C**。光标处弹出这个菜单：

```
 1. 润色
 2. 纠错
 3. 改写
```

（菜单是候选窗口的一种用法，但没有拼音行、也没有翻页箭头；顺序就是 `ai_actions.json` 里的顺序，
被 `ai.action_disabled` 关掉的不列出。）

按 `1`～`9` 或方向键选择、`回车`执行、`Esc`取消；也可以给某个动作配一个**自己的键**
（`ai.action_keys`），按它就直跑那个动作、不弹菜单。

**没选中内容时**：那个键照样能用 —— 这时处理的是**剪贴板里已有的文字**（刚复制的一段，不用再划一遍）。
没有选区就**不会有我们自己按的那次 `Ctrl+C`**：空选区上按 Ctrl+C 有的程序会把剪贴板清成空，
那就把我们正想用的那段话毁了。剪贴板里也没有文本（或只有图片之类）时**什么都不会发生**，界面毫无动静。

> 提示：这条意味着「没选中 → 用剪贴板」是**目的性读取**，剪贴板里若留着别的什么（早先复制的链接、
> 密码管理器里的口令），按一次动作键就会把它送去处理 —— 开着在线 AI 时它还会离开本机。
> 想要更保险可以只认「刚复制不久的」内容，需要的话再加。

**默认的五个动作**（`data/ai_actions.json`，顺序就是菜单顺序）：

| id | 名字 | 结果 | 输入上限 | 说明 |
|---|---|---|---|---|
| `translate` | 翻译 | 替换选区 | 8000 字 | **带语言列表**（12 种，见下）：**中文**译成下拉选的那个语言，**其它语言一律译成中文**；**走划词翻译那条服务**（线上优先、失败回退离线模型），所以 Ctrl+C+C 会用到的模型它也用得上 |
| `polish` | 润色 | 替换选区 | 8000 字 | 口语→书面、去赘字 |
| `proofread` | 纠错 | 替换选区 | 8000 字 | 只改错别字/标点/中英之间空格 |
| `rewrite` | 改写 | 替换选区 | 8000 字 | 换一种说法，长度与语气不变 |
| `summarize` | 摘要 | **插到选区下方**（原文不动） | 32000 字 | 同语言概括成一小段 |

- **翻译的语言与方向**：设置页「AI 动作」卡里，`translate` 那一行右侧的下拉选 **12 种**语言之一（英、中、日、韩、法、德、西、俄、越、葡、意、阿）。方向按**源语言**分：
  - 选中的是**中文** → 译成下拉里那个语言（线上）；下线（离线模型）则按「学习」页那两行走；
  - 选中的是**其它语言** → 一律译成**中文**（线上）；
  - **离线模型**（线上失败时的回退）**总是**按「学习」页的候选翻译两行取目标 —— 与 Ctrl+C+C 完全一致。
  **离线模型只覆盖 9 种**（中英日韩法德西俄越），葡/意/阿只有线上可用。
- **在输入框里换语言**：打 **`\fy`**（`\` 就是符号输入的前缀，命令优先于符号名），**候选条里列出的就是那 12 种语言**
  —— 它就是一次普通的符号组字，只不过候选不是符号而是语言名。选法也完全是原来那套：数字键、方向键 + 空格/
  回车、Tab 循环、`,` `.` 翻页、鼠标点选；**选中的语言存进 `ai.translate_language`**（与设置页那个下拉是同一项），
  **不上屏任何文字**（这条命令只是改设置），组字随之结束。
  实现：引擎识别这条命令（`engine/src/symbol_processor.cc`）后**不结束组字**，由翻译那一步
  （`Engine::translate_composition` → `Engine::ai_language_result`）用语言列表替换符号表；每个候选带一个
  `AiLanguageAction`（`engine/include/cxxime/candidate_selection.h`），选中时不走"上屏文字"那条路，而是走
  `Context::choose_ai_language()` 记下语言并结束组字（`Engine::dispatch_candidate_selection`）。服务端在这次按键/
  这次点选之后把它取走（`Engine::take_ai_language_choice()`），以合并补丁写进配置
  （`ConfigWriteCoordinator::enqueue_patch`，与「英文模式」等同类；`server/src/session_manager.cc` 的
  `persist_ai_language`）。所以这一功能**没有自己的窗口、没有自己的按键、没有额外的 IPC 命令**。
  名字取 `shared/src/ai_actions.cc` 的 `ai_language_label()`（中/英两种，与 `ai_action_label` 同规则）。
- **动作定义是数据**：`data/ai_actions.json`（随安装包发布，也可以自己改）。每条 =
  `id` + 名字（中/英）+ `prompt`（system 提示词，用户消息就是选中的原文）+
  `insert`（`replace` 替换选中文字，可 `Ctrl+Z` 一步撤销；`below` 写在选中文字**下方**、自成一行
  ——摘要用的就是它；`preview` 只显示）+
  `max_input_chars`（超过就只处理前面这么多，并明确提示）+ `timeout_ms` +
  `languages`（可选：带它就表示"这个动作要选一种语言"，翻译用的就是它，且不写 `prompt`——
  翻译的提示词是程序内置的那一句，与 Ctrl+C+C 一致）。加一个动作不用改程序。
- **只走线上 AI**：需要 `ai.enabled` + `ai.use_for.selection` + 有密钥。没配好时窗口会直接说明
  原因（例如「没有配置 API 密钥（设置 → AI）」），而不是悄悄什么都不做。
- **执行中**：窗口显示「正在润色…」，超时或失败显示原因；`Esc` 可取消。
- **长选区**：超过 `translate.max_selection_chars`（默认 32000 字）或该动作的 `max_input_chars`
  时，只处理前面那一段并提示「（只处理了前 N 字）」；要**替换**选区的动作此时改成**只显示**——免得把
  没处理的部分丢了（写在下方的那种不受影响，它本来就不动原文，提示作为最后一行跟着写进去）。
- **撤销**：替换走的是一次编辑（与「划词翻译 → 直接上屏」同一条路），`Ctrl+Z` 一步还原。
- 实现：`shared/src/ai_actions.cc`（读动作）、`shared/src/ai_client.cc`（`run_action`，含去引号/
  去「以下是…」这类包装）、`server/src/ai_action_service.cc`（后台线程跑、结果留内存供分块取回）、
  `tsf/src/text_service_translate.cpp`（菜单与写回）。

### 长文本怎么走 IPC

选中的文字和 AI 的回答都可能很长，而 IPC 的普通字段只有 4096 字节。短文本仍走老路
（`TRANSLATE_SELECTION` / `TRANSLATE_STATUS`，一字未改）；超过一个字段时改用四个追加式命令
（`shared/include/cxxime/ipc_protocol.h`）：

| 命令 | 用途 |
|---|---|
| `TEXT_UPLOAD_BEGIN` (37) | 开始上传：`key_code`=用途、`search_query`=动作 id、`modifiers`=总字节数 |
| `TEXT_UPLOAD_CHUNK` (38) | `candidate_index`=偏移、`translate_text`=一块（≤4095 字节） |
| `TEXT_RESULT_POLL` (39) | 状态 + `text_result_bytes`=已就绪字节数 |
| `TEXT_RESULT_READ` (40) | 按偏移取回一块 |

这样收发都没有 4096 字节的天花板，也没有结构体改动（`IPCRequest`/`IPCResponse` 的
baseline 与 static_assert 全部保持）。**顺带修掉了划词翻译的两个老问题**：选区超过 1000 字
以前会被静默砍掉、并且开着「直接上屏」时会把整段选区替换成那一小段的译文（后面的原文就丢了）；
现在超长会提示且不替换（`docs/settings-guide.md` 的 `translate.max_selection_chars`）。

## 设置界面

设置程序里的「AI」页（`settings/editor_ai_panel.cc`，也可用 `zhiyu-settings.exe --panel ai` 直接打开）：

- **在线 AI**：总开关，以及一行「当前使用：地址 · 模型」——地址或模型留空时用的就是服务商自己的
- **服务商**：内置表里的下拉（约 30 家）
- **接口地址 / 模型**：留空即用服务商自带的，也可以填自建或代理
- **API 密钥**：密码框；填入后点「确定/应用」即加密保存（DPAPI，仅本机本用户可解）并清空输入框，
  下方显示已存几把
- **模式**：优先线上（失败回退本地）/ 仅线上
- **超时（毫秒）**、**哪些功能可以用**（五个用途开关）、**测试**（对一句中文跑一次真实请求并显示结果）

密钥、开关都写进 `default.json` 的 `ai` 段（见上表），所以换机器要重新填密钥。

## 命令行自查

`zhiyu-server.exe --ai-test --key <key> [--provider id] [--base-url url] [--model name] [--text "文本"] [--out 结果文件]`
走的就是输入法用的那条链路（DPAPI 往返 → 提供方表 → WinHTTP → 协议 → 解析），用来确认密钥/网络通不通。

AI 动作也可以这样自查（`--data` 指向带 `ai_actions.json` 的 data 目录；打印结果与耗时）：

```
zhiyu-server.exe --ai-action --action proofread --key <key> --text "我觉得这个方按可以" --out r.txt
```
