#!/usr/bin/env python3
"""Fake llama-server / LM Studio for verifying the window probe.

Serves the EXACT payload shapes verified from upstream source:
  * llama.cpp tools/server/server-context.cpp get_res_model_info()
      /v1/models -> data[].meta.n_ctx
  * LM Studio native API
      /api/v1/models -> models[].loaded_instances[].config.context_length
"""
import json
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
                "n_ctx": 8192,          # served window (-c 8192)
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


class H(BaseHTTPRequestHandler):
    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if MODE == "llama":
            if path in ("/v1/models", "/models"):
                return self._send(LLAMA_MODELS)
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
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
