#!/usr/bin/env python3
"""Fake llama-server / LM Studio for verifying the window probe.

Serves the EXACT payload shapes verified from upstream source:
  * llama.cpp tools/server/server-context.cpp get_res_model_info()
      /v1/models -> data[].meta.n_ctx
  * LM Studio native API
      /api/v1/models -> models[].loaded_instances[].config.context_length
"""
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE = sys.argv[1] if len(sys.argv) > 1 else "llama"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

LLAMA_MODELS = {
    "object": "list",
    "data": [
        {
            "id": "qwen2.5-coder-7b",
            "object": "model",
            "created": 1700000000,
            "owned_by": "llamacpp",
            "meta": {
                "vocab_type": 2,
                "n_vocab": 152064,
                # Intentionally omit n_ctx here: the row only carries the
                # larger train-time ceiling. The served window comes from
                # /props below, which is the bug path we want to guard.
                "n_ctx_train": 32768,   # architectural ceiling
                "n_embd": 3584,
                "n_params": 7615616512,
                "size": 4431389696,
                "ftype": 15,
            },
        }
    ],
}

LMSTUDIO_V1 = {
    "object": "list",
    "data": [
        # The /v1 shim: only the ARCHITECTURAL maximum.
        {"id": "qwen/qwen3-coder-30b", "object": "model",
         "max_context_length": 262144},
    ],
}

LMSTUDIO_NATIVE = {
    "models": [
        {
            "key": "qwen/qwen3-coder-30b",
            "type": "llm",
            "max_context_length": 262144,
            "loaded_instances": [
                # Loaded at 16k, far below what the model supports.
                {"instance_id": "qwen/qwen3-coder-30b",
                 "status": "loaded",
                 "config": {"context_length": 16384,
                            "eval_batch_size": 512}},
            ],
        }
    ]
}


# Ollama: /api/tags carries NO window (verified: ollama api/types.go
# ListModelResponse). /api/ps reports the loaded context_length.
OLLAMA_TAGS = {
    "models": [
        {"name": "qwen2.5-coder:7b", "model": "qwen2.5-coder:7b",
         "modified_at": "", "size": 4431389696, "digest": "abc"},
    ]
}
# /api/show — Ollama's per-model capability probe. ModelInfo is Go's
# map[string]any, so encoding/json emits EVERY number as float64: the
# context_length arrives as 32768.0, not 32768. Ollama's own tests
# (cmd/cmd_test.go) use float64 literals for this exact field.
OLLAMA_SHOW = {
    "capabilities": ["completion", "tools"],
    "details": {"family": "qwen2", "parameter_size": "7B"},
    "model_info": {
        "general.architecture": "qwen2",
        "general.parameter_count": 7615616512.0,
        "qwen2.context_length": 32768.0,
        "qwen2.embedding_length": 3584.0,
    },
}
OLLAMA_PS = {
    "models": [
        {"name": "qwen2.5-coder:7b", "model": "qwen2.5-coder:7b",
         "size": 4431389696, "digest": "abc",
         "context_length": 16384},
    ]
}

# LiteLLM: /v1/model/info is a DECLARATION (the proxy's config), not a
# measurement — it must never shrink a larger advertised window.
LITELLM_V1 = {
    "object": "list",
    "data": [{"id": "gpt-4o", "object": "model", "context_length": 128000}],
}
LITELLM_INFO = {
    "data": [
        {"model_name": "gpt-4o",
         "model_info": {"max_input_tokens": 8192}},   # stale/conservative
    ]
}


# llama-server ROUTER mode (multi-model). Verified against
# tools/server/server-models.cpp get_router_props: a BARE /props returns a
# dummy with n_ctx 0 so the web UI doesn't break; the real window needs
# ?model=<name>. And models_autoload defaults to TRUE, so a query without
# autoload=false would LOAD the model as a side effect.
ROUTER_MODELS = {
    "object": "list",
    "data": [
        {"id": "qwen3-coder", "object": "model", "owned_by": "llamacpp",
         "status": {"value": "loaded", "args": [
             "llama-server", "--ctx-size", "32768"]}},
        # A dumber model launched with a much bigger context: the one a
        # user compacts on. Unloaded rows have NO meta block, so the only
        # place its size is written is status.args (server-models.cpp,
        # get_router_models).
        {"id": "gemma3:27b",  "object": "model", "owned_by": "llamacpp",
         "status": {"value": "unloaded", "args": [
             "llama-server", "--ctx-size", "131072"]}},
    ],
}
# What each model allocates when loaded. The router holds ONE at a time
# (--models-max 1): a chat request for another model swaps it in.
ROUTER_LOADED_WINDOW = {"qwen3-coder": 32768, "gemma3:27b": 131072}
ROUTER_STATE = {"loaded": os.environ.get("ROUTER_LOADED", "qwen3-coder")}
ROUTER_PROPS_BARE = {
    "role": "router",
    "max_instances": 1,
    "models_autoload": True,
    "model_alias": "llama-server",
    "model_path": "none",
    "default_generation_settings": {"params": {}, "n_ctx": 0},
}
ROUTER_PROPS_PER_MODEL = {
    "qwen3-coder": 32768,
    # gemma3:27b is NOT loaded -> error, and must be skipped, not defaulted.
}


# llama.cpp's chat_template_caps, verified against common/jinja/caps.h +
# caps::to_map() in caps.cpp. The server RUNS the model's own jinja template
# against probe inputs at load and diffs the output to see what it reacts to,
# so this is a measurement of the template that will actually be applied to
# our request — not metadata anyone typed in.
#
# It is the ONLY capability llama.cpp declares anywhere: its /v1/models rows
# are bare id + meta, which is why every model there used to fall through to
# inference over a GGUF filename.
LLAMA_TEMPLATE_CAPS = {
    "supports_string_content": True,
    "supports_typed_content": False,
    "supports_tools": True,
    "supports_tool_calls": True,
    "supports_parallel_tool_calls": True,
    "supports_system_role": True,
    "supports_preserve_reasoning": False,
    "supports_reasoning_effort": True,
    "supports_object_arguments": False,
}


class H(BaseHTTPRequestHandler):
    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        # /api/show is a POST in ollama's API.
        path = self.path.split("?")[0]
        if MODE == "ollama" and path == "/api/show":
            n = int(self.headers.get("Content-Length") or 0)
            if n:
                self.rfile.read(n)
            return self._send(OLLAMA_SHOW)
        if MODE == "router" and path in ("/v1/chat/completions", "/chat/completions"):
            # Autoload: the requested model replaces the resident one.
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            name = body.get("model", "")
            if name not in ROUTER_LOADED_WINDOW:
                return self._send({"error": {"message": "model not found"}}, 400)
            ROUTER_STATE["loaded"] = name
            return self._send({"id": "x", "object": "chat.completion", "model": name,
                               "choices": [{"index": 0, "finish_reason": "stop",
                                            "message": {"role": "assistant", "content": "ok"}}]})
        self._send({"error": "not found"}, 404)

    def do_GET(self):
        path = self.path.split("?")[0]
        if MODE == "llama":
            if path == "/props":
                return self._send({
                    "default_generation_settings": {"n_ctx": 8192},
                    "total_slots": 1,
                    "model_alias": "qwen2.5-coder-7b",
                    "chat_template_caps": LLAMA_TEMPLATE_CAPS,
                })
            if path in ("/v1/models", "/models"):
                return self._send(LLAMA_MODELS)
            if path == "/health":
                return self._send({"status": "ok"})
        elif MODE == "ollama":
            if path in ("/v1/models", "/models"):
                return self._send({"object": "list", "data": [
                    {"id": "qwen2.5-coder:7b", "object": "model"}]})
            if path == "/api/tags":
                return self._send(OLLAMA_TAGS)
            if path == "/api/ps":
                return self._send(OLLAMA_PS)
        elif MODE == "router":
            if path in ("/v1/models", "/models"):
                rows = json.loads(json.dumps(ROUTER_MODELS))
                for r in rows["data"]:
                    up = r["id"] == ROUTER_STATE["loaded"]
                    r["status"]["value"] = "loaded" if up else "unloaded"
                return self._send(rows)
            if path == "/props":
                from urllib.parse import parse_qs, urlparse
                q = parse_qs(urlparse(self.path).query)
                name = (q.get("model") or [""])[0]
                if not name:
                    return self._send(ROUTER_PROPS_BARE)
                # Refuse to answer unless autoload was explicitly disabled —
                # mirrors the real server, where omitting it LOADS the model.
                autoload = (q.get("autoload") or ["true"])[0]
                if autoload not in ("false", "0"):
                    print("  !! would have AUTOLOADED %s" % name, file=sys.stderr)
                w = ROUTER_LOADED_WINDOW.get(name) if name == ROUTER_STATE["loaded"] else None
                if w is None:
                    return self._send({"error": {"message": "model is not loaded"}}, 400)
                # Per-model caps: on a router each model has its OWN chat
                # template, so the bare /props dummy above describes none of
                # them. Only a LOADED model gets here at all (the branch
                # above 400s otherwise, matching router_validate_model with
                # autoload=false), so an unloaded one declares nothing and
                # must stay UNKNOWN on the client rather than "no reasoning".
                caps = dict(LLAMA_TEMPLATE_CAPS)
                caps["supports_reasoning_effort"] = name == "qwen3-coder"
                return self._send({
                    "default_generation_settings": {"n_ctx": w},
                    "chat_template_caps": caps,
                })
        elif MODE == "litellm":
            if path in ("/v1/models", "/models"):
                return self._send(LITELLM_V1)
            if path == "/v1/model/info":
                return self._send(LITELLM_INFO)
        else:
            if path in ("/v1/models", "/models"):
                return self._send(LMSTUDIO_V1)
            if path == "/api/v1/models":
                return self._send(LMSTUDIO_NATIVE)
        self._send({"error": "not found"}, 404)

    def log_message(self, *a):
        print("  %s %s" % (self.command, self.path), file=sys.stderr)


if __name__ == "__main__":
    print(f"fake {MODE} server on :{PORT}", file=sys.stderr)
    HTTPServer((os.environ.get("BIND", "127.0.0.1"), PORT), H).serve_forever()
