#pragma once
// agentty::modelsdev — background snapshot of the models.dev community
// capability database (https://models.dev/api.json).
//
// models.dev is the open, community-maintained model-metadata DB (used in
// production by OpenCode). Its schema names exactly the fact agentty needs
// for heterogeneous providers: per-model `reasoning` plus
// `reasoning_options: [{type:"effort", values:[...]}]` — the EXACT effort
// enum a model accepts, which varies wildly (full six-tier ladders, binary
// {high,max}, toggle-only, none).
//
// refresh() fetches api.json (24h disk cache under config_dir()), parses it,
// and pushes per-model facts into the catalog registries:
//   set_catalog_reasoning(id, bool)        — reasoning on/off declaration
//   set_catalog_effort_set(id, bitmask)    — the declared effort enum
// Ids are recorded BARE (models.dev keys are provider-scoped; agentty model
// ids are bare wire ids) — both the "provider/model" key and its tail after
// the last '/' are registered so either spelling resolves.
//
// The precedence pipeline in resolved_caps() places these declarations below
// user overrides and below learned-from-rejection facts, above from_id
// inference. So: a brand-new model ships → the community updates models.dev →
// agentty picks it up within a day, no release needed. Failures are silent
// (a metadata refresh must never surface as an error).

#include <string>

namespace agentty::modelsdev {

// Load the cached snapshot from disk (if any) into the catalog registries.
// Cheap; call at startup before the first render.
void load_cached();

// Fetch a fresh snapshot if the cache is older than ~24h, then (re)load it.
// Blocking — run on a background worker (Cmd::task). Returns the number of
// models registered (0 on any failure).
int refresh();

} // namespace agentty::modelsdev
