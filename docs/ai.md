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
| `ai.temperature` | `0.7` | 采样温度（0–2） |
| `ai.reasoning_effort` | `""` | 推理档位；空 = 不发这个字段 |
| `ai.timeout_ms` | `8000` | 单次请求超时（1000–120000） |
| `ai.keys` | `[]` | **多把** API 密钥，**DPAPI（当前用户）加密后的 base64**，不是明文。一把被拒（401/429）自动换下一把 |
| `ai.use_for.*` | 全 `false` | 哪些功能允许走线上：`translate`（划词翻译）/ `selection`（选中文字的 AI 动作）/ `english` / `voice` / `glossary` |

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
| 选中文字的 AI 动作（润色/改写/摘要/纠错） | 规划中 |
| 英文补全 / 语音后处理 / 语言包兜底 | 规划中 |
| 设置界面「AI」页 | 规划中（当前直接编辑 `default.json` 即可） |

## 设置界面

规划中的「AI」页会提供：总开关 / 模式 / 提供方（下拉，自动带出地址与模型）/ API Key（多把、掩码）/
模型 / 超时 / 温度 / 推理档位 / 「测试」按钮 / 各功能开关。
