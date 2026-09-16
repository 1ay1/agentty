# Context windows

The context window is the denominator for the `ctx %` gauge and for
auto-compaction, so a wrong number is not cosmetic: understate it and agentty
compacts far sooner than it needs to, overstate it and the turn fails on the
wire.

agentty resolves it automatically. **You should never have to set this.**

## The ladder

Five rungs, strongest evidence first:

| # | rung | source |
|---|---|---|
| 1 | per-model override | what you set for this exact provider+model |
| 2 | **live advertised** | the gateway's `/v1/models` row, or a probe |
| 3 | **models.dev** | a public catalog, 7,824 models deep |
| 4 | id inference | Claude/GPT families, the `[1m]` suffix |
| 5 | env / default | `AGENTTY_MAX_CONTEXT_TOKENS`, else 200k |

Rungs 2–4 are automatic and need no configuration.

### 2. What the gateway says — the API is truth

Read from the `/v1/models` row in whichever spelling the host uses:
`context_length`, `top_provider.context_length` (OpenRouter's per-deployment
figure), `max_model_len` (vLLM), `max_input_tokens` (LiteLLM), `n_ctx` /
`n_ctx_train` (llama.cpp), or nested under `model_info`. Stringified numbers
and floats are accepted — LiteLLM emits `16385.0`.

When a row carries **nothing**, agentty asks the gateway directly, once per
catalog load:

- `GET /v1/model/info` — LiteLLM's management route, per-model
  `max_input_tokens` resolved from its own cost map
- `GET /props` — llama.cpp, whose `n_ctx` is the window the server was
  *started* with
- `POST /api/show` — Ollama's per-model `context_length`

This rung outranks every static source and always will. The same model name
behind two gateways can be served at two different sizes, only the gateway
knows which applies to the request about to be sent, and only the gateway can
reject it for overflowing.

### 3. models.dev

A public catalog of 7,824 models. This is the rung that makes non-Claude
models honest: id inference (rung 4) knows the Claude and GPT families and
answers 0 for everything else, which sent 2,698 models with real windows of
1M or more to the 200k default.

Scoped by provider first, because the same bare id is genuinely served at
different sizes by different hosts — 27 hosts publish `gpt-4.1` and they do
not all agree. A cross-provider disagreement makes the shared key read as *no
declaration* rather than letting one host's figure bleed onto another's.

### Measured coverage

Across the providers agentty ships, how many models come with a declared
window:

```
openrouter  368/368      google      39/39      anthropic  14/14
openai       43/48       mistral     32/34      xai        12/12
groq         14/16       deepseek     4/4       cerebras    2/2
```

## When it can't be automatic

One case is unreachable by construction: a **private gateway** serving a model
id no catalog has published, on a host that advertises nothing. No amount of
discovery fixes that — a URL like `10.0.0.5:4000` is unknowable to any
third-party catalog.

Every serious client keeps an escape hatch for it. Zed *requires* `max_tokens`
in `settings.json` for OpenAI-compatible endpoints; Claude Code has
`CLAUDE_CODE_MAX_CONTEXT_TOKENS`.

agentty's is:

```sh
AGENTTY_MAX_CONTEXT_TOKENS=1000000 agentty
```

It is the **last** rung deliberately: it is global, so a session that reaches
several models would otherwise stamp one number across all of them. Anything
that actually knows a given model — the gateway, models.dev, the id — wins
over it. A value that does not parse, or is zero or negative, is ignored
rather than becoming a tiny window.

## Checking what was resolved

The model picker's `ctx` column shows the window that will actually be used.
A row reading `auto` means nothing declared one and the default is in force —
that is the signal to set the variable above.

## Why there is no keybinding

`^W` used to cycle a per-model override from the picker. It was removed: a
value you set once is a setting, not a navigation action, and a keybinding on
a list row is a poor home for it. With rungs 2–4 covering the large majority
of real deployments, the manual path should be rare enough that an
environment variable and this page are the right surface.
