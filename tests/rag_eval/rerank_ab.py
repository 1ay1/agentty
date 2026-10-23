#!/usr/bin/env python3
# Offline reranker A/B on dumped search_code pools.
#   AGENTTY_RAG_DUMP_POOL=pool.jsonl agentty rag-eval cases.json
#   python3 tests/rag_eval/rerank_ab.py cases.json pool.jsonl http://127.0.0.1:8090 [top_n] [v]
# Prints hit@1 / hit@8 / MRR for the current order, the reranker alone, and the
# reranker with code_rerank's priors, blended with the current score. Needs a
# server with a cohere-style /v1/rerank (llama-server --reranking, TEI, ...).
# A trailing `v` prints each query's reranker top 3.
#
# bge-reranker-v2-m3 (Q8, llama-server) on the qwen3-embedding pools: alone it
# ranks headers/tests/docs first (hit@1 .65 -> .38 main, .80 -> .25 held-out);
# with the priors it lifts hit@8 to 1.0 / .95 but hit@1 doesn't beat the
# current order. Not wired in.
import json, sys, urllib.request

cases = {c["q"]: set(c["gold"]) for c in json.load(open(sys.argv[1]))}
pools = {}
for line in open(sys.argv[2]):
    r = json.loads(line)
    if r["q"] in cases: pools[r["q"]] = r["c"]
url = sys.argv[3].rstrip("/") + "/v1/rerank"
top_n = int(sys.argv[4]) if len(sys.argv) > 4 else 32

def rerank(q, texts):
    body = json.dumps({"query": q, "documents": texts, "top_n": len(texts)}).encode()
    req = urllib.request.Request(url, body, {"content-type": "application/json"})
    out = [0.0] * len(texts)
    for r in json.load(urllib.request.urlopen(req, timeout=120))["results"]:
        out[r["index"]] = r["relevance_score"]
    return out

def norm(xs):
    lo, hi = min(xs), max(xs)
    return [(x - lo) / (hi - lo) if hi > lo else 0.0 for x in xs]

def files_in_order(cands, scores):
    order = sorted(range(len(cands)), key=lambda i: -scores[i])
    seen, out = set(), []
    for i in order:
        p = cands[i]["path"]
        if p not in seen: seen.add(p); out.append(p)
    return out

def metrics(ranked_by_q):
    h1 = h8 = mrr = 0
    for q, files in ranked_by_q.items():
        gold = cases[q]
        rank = next((i + 1 for i, f in enumerate(files[:8]) if f in gold), 0)
        h1 += rank == 1; h8 += rank >= 1; mrr += 1 / rank if rank else 0
    n = len(ranked_by_q)
    return f"hit@1 {h1/n:.3f}  hit@8 {h8/n:.3f}  MRR {mrr/n:.3f}"

def role_weight(path, q):
    # Mirrors code_rerank's role prior: tests/docs/build files count less
    # unless the question asks for them, headers slightly less.
    p, ql = path.lower(), q.lower()
    if ("/test" in p or p.startswith("test")) and "test" not in ql: return 0.55
    if (p.endswith(".md") or p.startswith("docs/")) and not any(w in ql for w in ("doc", "readme", "guide")): return 0.55
    if (p.endswith(".cmake") or "cmakelists" in p) and not any(w in ql for w in ("cmake", "build", "lint")): return 0.6
    if p.endswith((".hpp", ".h")): return 0.9
    return 1.0

def stem(p): return p.rsplit("/", 1)[-1].rsplit(".", 1)[0]

def with_priors(q, cands, scores):
    s = [x * role_weight(c["path"], q) for x, c in zip(scores, cands)]
    hdr = {}
    for x, c in zip(s, cands):
        if c["path"].endswith((".hpp", ".h")): hdr[stem(c["path"])] = max(hdr.get(stem(c["path"]), 0), x)
    return [max(x, hdr[stem(c["path"])] * 1.02) if c["path"].endswith((".cpp", ".cc", ".c")) and stem(c["path"]) in hdr else x
            for x, c in zip(s, cands)]

base, rr = {}, {}
for q, c in pools.items():
    c = c[:top_n]
    pools[q] = c
    base[q] = [x["score"] for x in c]
    # Same text the model sees: path first, so it knows which file it is.
    rr[q] = rerank(q, [f'{x["path"]}\n{x["text"][:2000]}' for x in c])

print(f"{len(pools)} queries, top {top_n}")
if len(sys.argv) > 5:
    for q in pools:
        rf = files_in_order(pools[q], rr[q])[:3]
        print(f"  {q[:60]:60}  gold={sorted(cases[q])[0]}  rr={rf}")
print(f"  current         {metrics({q: files_in_order(pools[q], base[q]) for q in pools})}")
print(f"  reranker only   {metrics({q: files_in_order(pools[q], rr[q]) for q in pools})}")
print(f"  rr + priors     {metrics({q: files_in_order(pools[q], with_priors(q, pools[q], norm(rr[q]))) for q in pools})}")
for w in (0.3, 0.5, 0.7):
    blend = {q: [w * a + (1 - w) * b for a, b in zip(with_priors(q, pools[q], norm(rr[q])), norm(base[q]))] for q in pools}
    print(f"  blend+priors w={w} {metrics({q: files_in_order(pools[q], blend[q]) for q in pools})}")
