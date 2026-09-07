"""一个会录音的假大模型，给 LLM 阶段对拍用。

它做两件事：
  **把收到的提示词原样记下来**（每条一行，写进 prompts.jsonl）；
  **回一个事先放好的答案**（读 reply.txt，不消耗，同一个答案能发两遍）。

为什么要它：方案第三节写着"提示词的拼接必须逐字一致"。对拍程序拿它
夹在两个后端和模型之间，同一个请求让两边各发一次，然后**逐字节比两条
提示词**——这是唯一能真正验证那条要求的办法。单元测试比的是
C++ 自己拼出来的串和录好的串，验不了"Python 现在还是这么拼的"。

不消耗答案是有意的：两个后端会先后各请求一次，弹出式的队列会让第二个
拿到下一条答案，于是永远错位一条。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/fake_llm.py [端口] [工作目录]
"""

from __future__ import annotations

import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

WORK = Path("fake_llm")


class Handler(BaseHTTPRequestHandler):
    # 默认的日志会往 stderr 刷每一条请求，对拍时那是纯噪音。
    def log_message(self, fmt, *args):  # noqa: A003
        pass

    def _json(self, code: int, payload) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        # /v1/models：两个后端的"连不连得上"检查都问它。
        if self.path.rstrip("/").endswith("/models"):
            self._json(200, {"data": [{"id": "duiping-fake"}]})
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            req = {"_unparsed": raw}

        # 把提示词单独记一行。**记的是拼好的完整串**，不是整个请求体——
        # 请求体里还有 model、temperature 之类，那些两边本来就可能不同，
        # 混在一起比会淹掉真正要看的东西。
        prompt = ""
        for msg in req.get("messages", []) or []:
            prompt += msg.get("content", "")
        with (WORK / "prompts.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps({"path": self.path, "prompt": prompt,
                                "request": req}, ensure_ascii=False) + "\n")

        reply_file = WORK / "reply.txt"
        reply = reply_file.read_text(encoding="utf-8") if reply_file.is_file() else "{}"
        self._json(200, {
            "id": "duiping",
            "object": "chat.completion",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": reply},
                "finish_reason": "stop",
            }],
        })


def main() -> int:
    global WORK
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8125
    if len(sys.argv) > 2:
        WORK = Path(sys.argv[2])
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "prompts.jsonl").write_text("", encoding="utf-8")
    print(f"假大模型在 127.0.0.1:{port}，工作目录 {WORK.resolve()}", flush=True)
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
