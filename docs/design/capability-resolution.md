# Capability Resolution — the design contract

**The problem.** Every provider exposes a different contract for the *same*
knob. `reasoning_effort` alone: Claude takes `low…max` (family-dependent),
gpt-5.x takes `low…xhigh|max` (revision-dependent), Mistral takes exactly
`{none, high}` and 400s on everything else, DeepSeek takes `low|medium|high`,
Magistral flipped from *rejecting* the parameter to *requiring* the binary
form between model revisions, and a llama.cpp box takes nothing. Multiply by
every other parameter and every provider, and "support all providers" by
hand-writing rules is unwinnable: **authored knowledge rots**.

**The goal.** The user never sees any of this. One effort chip, one ladder
that only ever shows values that will work, on every provider — including
ones that don't exist yet. A wrong guess costs one silent retry, once, ever.

## The principle: capabilities are data with a freshness gradient

A capability fact can come from six places. They are ordered by *freshness*
(how directly the source reflects the provider's live dispatch table), and
the freshest available fact wins:

| # | Layer | Source | Example |
|---|-------|--------|---------|
| 1 | User override | `^E` in the picker, persisted | "I know better" |
| 2 | Env override | `AGENTTY_FORCE_EFFORT` | CI / debugging |
| 3 | **Learned from rejection** | the provider's own 4xx body | `"supported values: [high, none]"` |
| 4 | Declared (metadata) | models.dev snapshot (24h cache) | `reasoning_options.values` |
| 5 | Declared (live) | provider `/v1/models` flags | Mistral `capabilities.reasoning` |
| 6 | Inferred | `from_id` substring decode | offline seed, frame-1 guess |

Layer 3 is the safety net that makes the whole stack correct-by-construction:
when any provider rejects a parameter value, `parse_effort_rejection()`
extracts what it *does* accept from the error body, the fact is recorded and
persisted (`Settings.learned_effort_sets`), the setting is clamped, and the
turn silently relaunches. The user sees one dim toast
(`reasoning medium → high (all this model supports)`) and their answer. This
is feature *detection*, not feature *enumeration* — the same reason browsers
dropped UA sniffing and terminals use terminfo.

## The representation: exact value sets, not booleans

The capability is not "supports effort: yes/no" plus quirk flags. It is
**the exact set of values the model's API accepts**, as a bitmask
(`ModelCapabilities::effort_set` + `effort_set_known`). Everything derives
from one function:

```
effort_set_of(caps)      → the mask (exact when known, derived from
                           family/compat gates when not)
available_efforts(caps)  → the picker ladder: off + exactly the ON levels
nearest_effort(e, set)   → intent → nearest wire value (at-or-below first:
                           never think harder than asked)
clamp_effort / effort_wire_for → one-liners over nearest_effort
```

Mistral's binary `{high}`, Claude's `…max`, gpt-5.4's xhigh cap, a
toggle-only model, and "no effort at all" are all just different masks.
**Adding a new heterogeneity case is data, never a new code path.**

## Keying: provider-scoped, bare fallback

The same bare model id can live on several providers with different
contracts (`gpt-oss-120b` on Groq vs Cerebras vs local Ollama). Facts are
recorded under `"provider/model"` (the active provider label is published to
the domain layer via `set_caps_provider_scope` on every `provider::select`)
and looked up scoped-first, bare-second. A fact learned on one host never
bleeds to another.

## The UX contract

1. **Controls are intents, not wire parameters.** The user picks a tier;
   each model maps it to its nearest supported value silently.
2. **The ladder shows only what works.** `available_efforts` renders from
   the resolved mask, so an invalid pick is unrepresentable in the UI.
3. **Errors are negotiation, not failures.** A parameter rejection is
   handled by the learn-clamp-retry arm (sibling of the 1M-beta self-heal
   in `stream.cpp`); the user's request still completes.
4. **Degradation is informative, not modal.** One dim status line names
   what changed and why; nothing interrupts the turn.
5. **Zero configuration required, full override available.** Every layer
   below the user's explicit override is automatic; `^E` and
   `AGENTTY_FORCE_EFFORT` remain for the 1% who need them.

## Where things live

| Concern | Location |
|---------|----------|
| Capability model + registries + resolution | `include/agentty/domain/catalog.hpp` |
| Rejection parser | `include/agentty/provider/error_class.hpp` (`parse_effort_rejection`) |
| Learn-clamp-retry arm | `src/runtime/app/update/stream.cpp` (StreamError, next to the 1M self-heal) |
| models.dev snapshot | `src/util/modelsdev.cpp` (24h cache in `config_dir()/modelsdev.json`) |
| Live catalog recording | `src/provider/openai/transport.cpp` (`list_models`) |
| Persistence | `Settings.learned_effort_sets`, hydrated in `init()` |

## Extending to the next parameter

When the next heterogeneous knob appears (sampling params, tool-call style,
max_tokens ceilings), follow the same recipe:

1. Represent the capability as **data** (a set/range, not booleans).
2. Resolve through the **freshness gradient** (override > learned >
   declared > inferred).
3. Add a **rejection parser** for the provider error shapes and wire it to
   the learn-clamp-retry arm.
4. Derive the UI from the resolved value so invalid choices are
   unrepresentable.

What we deliberately do NOT do: add per-provider `if` chains in transports,
surface provider vocabulary in the UI, or fail a turn on a capability
mismatch that a retry can absorb.
