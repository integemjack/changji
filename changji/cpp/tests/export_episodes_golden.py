"""导出剧本读写和剧集增删改的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_episodes_golden.py

产出 cpp/tests/golden/endpoints_episodes.json。

这几个接口属于阶段 3，当时按 test_web_editing.py 的覆盖面移植而漏掉了。
补的时候把语料也补上，免得下次又靠"前端调出 404"才发现。
"""
from __future__ import annotations

import io
import json
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from fastapi.testclient import TestClient                          # noqa: E402

from changji.config import load_settings                           # noqa: E402
from changji.web.server import create_app                          # noqa: E402

GOLDEN = HERE.parent / "golden"


def pristine_root() -> Path:
    exp = json.loads(
        io.open(GOLDEN / "project_expectations.json", encoding="utf-8").read())
    return GOLDEN / exp["root_name"]


def run_cases(tmp: Path) -> list[dict]:
    app = create_app(load_settings())
    client = TestClient(app, raise_server_exceptions=False)
    cases: list[dict] = []
    counter = [0]

    def call(name, method, url, *, body=None, query=None, prep=None):
        """跑一条。prep 拿到项目根目录，可以先改点东西。"""
        counter[0] += 1
        root = tmp / ("case%02d" % counter[0])
        shutil.copytree(pristine_root(), root)
        if prep:
            prep(root)

        q = dict(query or {})
        if q.get("path") == "__PROJECT__":
            q["path"] = str(root)
        b = dict(body) if body else None
        if b and b.get("project") == "__PROJECT__":
            b["project"] = str(root)

        if method == "GET":
            r = client.get(url, params=q)
        else:
            r = client.post(url, json=b)
        try:
            payload = r.json() if r.content else None
        except Exception:                                 # noqa: BLE001
            payload = {"__raw__": r.text[:400]}

        if r.status_code == 200:
            compare = "full"
        elif isinstance(payload, dict) and isinstance(payload.get("detail"), list):
            compare = "detail_arr"
        else:
            compare = "detail_str"

        cases.append({
            "name": name, "method": method, "url": url,
            "query": q if method == "GET" else None,
            "body": b,
            "prep": getattr(prep, "tag", None),
            "status": r.status_code, "response": payload, "compare": compare,
            # 这几个接口都写盘（GET 除外），落盘结果也要比
            "project_after": (
                json.loads(io.open(root / "project.json", encoding="utf-8").read())
                if method == "POST" and r.status_code == 200 else None),
        })

    P = "__PROJECT__"

    # ---- GET /api/script ----
    call("读剧本：ep01", "GET", "/api/script",
         query={"path": P, "episode_id": "ep01"})
    call("读剧本：没剧本的那集", "GET", "/api/script",
         query={"path": P, "episode_id": "ep02"})
    call("读剧本：不存在的集", "GET", "/api/script",
         query={"path": P, "episode_id": "ep99"})
    call("读剧本：项目不存在", "GET", "/api/script",
         query={"path": "Z:/没有这个目录", "episode_id": "ep01"})
    call("读剧本：缺参数", "GET", "/api/script", query={"path": P})

    # ---- POST /api/script ----
    call("改剧本", "POST", "/api/script",
         body={"project": P, "episode_id": "ep01", "script": "改过的剧本"})
    call("改剧本：顺带改时长", "POST", "/api/script",
         body={"project": P, "episode_id": "ep01", "script": "x",
               "duration_s": 90.0})
    call("改剧本：时长给 0", "POST", "/api/script",
         body={"project": P, "episode_id": "ep01", "script": "x",
               "duration_s": 0})
    call("改剧本：不存在的集", "POST", "/api/script",
         body={"project": P, "episode_id": "ep99", "script": "x"})
    call("改剧本：缺 script", "POST", "/api/script",
         body={"project": P, "episode_id": "ep01"})
    call("改剧本：多余字段照收", "POST", "/api/script",
         body={"project": P, "episode_id": "ep01", "script": "x", "typo": 1})

    # ---- POST /api/episode ----
    call("新建一集：自动编号", "POST", "/api/episode",
         body={"project": P, "title": "第三集"})
    call("新建一集：指定 id", "POST", "/api/episode",
         body={"project": P, "episode_id": "special", "title": "特别篇"})
    call("新建一集：id 非法", "POST", "/api/episode",
         body={"project": P, "episode_id": "EP-03"})
    call("新建一集：id 已存在", "POST", "/api/episode",
         body={"project": P, "episode_id": "ep01"})
    call("新建一集：自定时长", "POST", "/api/episode",
         body={"project": P, "target_duration_s": 120.0})

    def add_trailer(root):
        p = json.loads(io.open(root / "project.json", encoding="utf-8").read())
        p["episodes"].append({"episode_id": "trailer", "title": "预告",
                              "synopsis": "", "target_duration_s": 20.0,
                              "script": "", "shots": []})
        io.open(root / "project.json", "w", encoding="utf-8").write(
            json.dumps(p, ensure_ascii=False))
    add_trailer.tag = "add_trailer"

    call("新建一集：项目里有预告，不该跳号", "POST", "/api/episode",
         body={"project": P}, prep=add_trailer)

    # ---- POST /api/episode/action ----
    call("删一集", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep02", "action": "delete"})
    call("改名", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "rename",
               "new_title": "新标题"})
    call("改名：不给新名字", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "rename"})
    call("复制：带分镜的那集", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "duplicate"})
    call("复制：项目里有预告", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "duplicate"},
         prep=add_trailer)
    call("不认识的操作", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "explode"})
    call("操作不存在的集", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep99", "action": "delete"})

    def only_one(root):
        p = json.loads(io.open(root / "project.json", encoding="utf-8").read())
        p["episodes"] = p["episodes"][:1]
        io.open(root / "project.json", "w", encoding="utf-8").write(
            json.dumps(p, ensure_ascii=False))
    only_one.tag = "only_one"

    call("删最后一集要拒绝", "POST", "/api/episode/action",
         body={"project": P, "episode_id": "ep01", "action": "delete"},
         prep=only_one)

    return cases


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="changji_episodes_") as td:
        cases = run_cases(Path(td))
    data = {
        "note": "由 cpp/tests/export_episodes_golden.py 生成，不要手改",
        "cases": cases,
    }
    target = GOLDEN / "endpoints_episodes.json"
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("写好了", target)
    for c in data["cases"]:
        print("  %-28s %-4s %-22s %3d" % (c["name"], c["method"], c["url"],
                                          c["status"]))


if __name__ == "__main__":
    main()
