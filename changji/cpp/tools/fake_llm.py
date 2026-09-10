"""一个会录音的假大模型。

它做两件事：
  **把收到的提示词原样记下来**（每条一行，写进 prompts.jsonl）；
  **回一个事先放好的答案**（读 reply.txt，不消耗，同一个答案能发两遍）。

原来是给对拍用的：夹在两个后端和模型之间，同一个请求让两边各发一次，
然后逐字节比两条提示词。对拍随 Python 引擎一起删了，它留下来是因为
**手工看"这一版到底发了什么出去"仍然只有它做得到**——
`[llm].backend = "remote"` 加上一个 base_url 指到它，跑一遍
就能把真实请求体捞出来。单元测试比的是录好的串，看不见当下发的。

跑法（在 changji/ 下）：
    python cpp/tools/fake_llm.py [端口] [工作目录]
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

        reply = self._next_reply()
        self._json(200, {
            "id": "duiping",
            "object": "chat.completion",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": reply},
                "finish_reason": "stop",
            }],
        })


    def _next_reply(self) -> str:
        """按顺序取下一个答案。

        replies.jsonl 一行一个（JSON 字符串）。**用光了重复最后一条**，
        不是报错：某一侧可能比另一侧多问一次模型（比如它在别的地方
        又校验了一遍），那时候报错会把一次正常的对拍变成失败。

        /api/plan 一次请求要问两遍（圣经 + 分镜），所以必须排队；
        而**每一侧各起一个假模型**，两边不共用队列——共用的话
        第二个后端拿到的是第一个后端剩下的，永远错位。
        """
        queue = WORK / "replies.jsonl"
        if not queue.is_file():
            single = WORK / "reply.txt"
            return single.read_text(encoding="utf-8") if single.is_file() else "{}"

        lines = [ln for ln in queue.read_text(encoding="utf-8").splitlines() if ln]
        if not lines:
            return "{}"
        cursor_file = WORK / "cursor.txt"
        try:
            cursor = int(cursor_file.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            cursor = 0
        index = min(cursor, len(lines) - 1)
        cursor_file.write_text(str(cursor + 1), encoding="utf-8")
        try:
            return json.loads(lines[index])
        except json.JSONDecodeError:
            return lines[index]


def main() -> int:
    global WORK
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8125
    if len(sys.argv) > 2:
        WORK = Path(sys.argv[2])
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "prompts.jsonl").write_text("", encoding="utf-8")
    (WORK / "cursor.txt").write_text("0", encoding="utf-8")
    print(f"假大模型在 127.0.0.1:{port}，工作目录 {WORK.resolve()}", flush=True)
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
