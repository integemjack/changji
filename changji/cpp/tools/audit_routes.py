"""拿 Python 的路由表核对 C++ 实现了哪些接口。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/audit_routes.py

为什么要有这个脚本：阶段 3 移植时拿 test_web_editing.py 当清单，
而那个文件不测 /api/script 和 /api/episode，结果那四个接口整片漏掉，
**没有任何编译或运行迹象**，直到阶段 2 判据的实机验证里前端调出 404
才发现。教训是"对拍语料的覆盖面成了移植的覆盖面"。

对策不是下次小心点，是换一份清单：路由表是 Python 侧的事实，
测试文件只是它的一个子集。后面每个阶段开工前跑一遍这个。

清单来自 FastAPI 的 app.routes（不是 grep 源码），所以装饰器怎么写、
路由挂在哪个 router 上都不影响结果。
"""
from __future__ import annotations

import io
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from changji.config import load_settings                           # noqa: E402
from changji.web.server import create_app                          # noqa: E402

CPP_SRC = REPO / "cpp" / "src"

# 按方案的阶段划分，标出每个接口归谁。没列到的算"待归类"，
# 会在报告里单独列出来——那正是最容易漏的一类。
STAGE = {
    "/api/health": "0", "/api/doctor": "0",
    "/api/hardware": "1", "/api/settings": "1",
    # 连接设置：改的是机器级别的东西（推理服务地址、大模型地址和密钥），
    # 写回的是用户全局配置不是项目文件。按内容属于阶段 1。
    "/api/connections": "1",
    "/api/projects": "2", "/api/project": "2", "/api/shots": "2",
    "/api/assets": "2",
    "/api/shot": "3", "/api/character": "3", "/api/location": "3",
    "/api/style": "3", "/api/shots/batch": "3", "/api/shots/reorder": "3",
    "/api/shots/link_locations": "3", "/api/media": "3",
    "/api/character/reference": "3", "/api/location/reference": "3",
    "/api/character/reference/clear": "3", "/api/location/reference/clear": "3",
    "/api/script": "3", "/api/episode": "3", "/api/episode/action": "3",
    "/api/new": "3", "/api/project/delete": "3", "/api/project/premise": "3",
    "/api/script/write": "4", "/api/script/premise": "4",
    "/api/script/trailer": "4", "/api/script/series": "4",
    "/api/script/series/stop": "4", "/api/bible": "4", "/api/plan": "4",
    "/api/plan/all": "4", "/api/run": "4", "/api/stop": "4",
    "/api/llm/providers": "4", "/api/llm/models": "4", "/": "4",
    "/api/run/preview": "5", "/api/outputs": "5",
    "/api/voices": "6",
}


def python_routes() -> list[tuple[str, str]]:
    """(方法, 路径)，去掉 FastAPI 自带的那几个。"""
    app = create_app(load_settings())
    out = set()
    for r in app.routes:
        path = getattr(r, "path", None)
        methods = getattr(r, "methods", None)
        if not path or not methods:
            continue
        if path in ("/openapi.json", "/docs", "/redoc",
                    "/docs/oauth2-redirect"):
            continue
        for m in methods:
            if m in ("HEAD", "OPTIONS"):
                continue
            out.add((m, path))
    return sorted(out)


def cpp_routes() -> set[tuple[str, str]]:
    """扫 C++ 源码里的 CROW_ROUTE。

    这一半只能靠 grep：Crow 的路由是宏展开的，拿不到运行期的表。
    好在写法很固定。
    """
    out = set()
    pat = re.compile(
        r'CROW_ROUTE\(\s*app\s*,\s*"([^"]+)"\s*\)'
        r'(?:\s*\.methods\((.*?)\))?', re.S)
    for f in sorted(CPP_SRC.rglob("*.cpp")):
        text = io.open(f, encoding="utf-8").read()
        for m in pat.finditer(text):
            path = m.group(1)
            methods_src = m.group(2) or ""
            found = re.findall(r'"(\w+)"_method', methods_src)
            for method in (found or ["GET"]):
                out.add((method.upper(), path))
    # WebSocket 单列，它不是 CROW_ROUTE
    for f in sorted(CPP_SRC.rglob("*.cpp")):
        if "CROW_WEBSOCKET_ROUTE" in io.open(f, encoding="utf-8").read():
            out.add(("WS", "/ws"))
    return out


def main() -> None:
    py = python_routes()
    cpp = cpp_routes()

    missing: dict[str, list[str]] = {}
    for method, path in py:
        if (method, path) in cpp:
            continue
        stage = STAGE.get(path, "待归类")
        missing.setdefault(stage, []).append(f"{method:5s} {path}")

    extra = sorted(x for x in cpp if x not in set(py) and x[0] != "WS")

    print("Python 侧路由 %d 条，C++ 侧 %d 条" % (len(py), len(cpp)))
    print("已实现 %d 条，还缺 %d 条\n" % (
        len(py) - sum(len(v) for v in missing.values()),
        sum(len(v) for v in missing.values())))

    for stage in sorted(missing, key=lambda s: (s == "待归类", s)):
        head = "待归类 —— 这些没排进任何阶段，最容易漏" if stage == "待归类" \
            else "阶段 %s" % stage
        print("【%s】%d 条" % (head, len(missing[stage])))
        for line in sorted(missing[stage]):
            print("   ", line)
        print()

    if extra:
        print("【C++ 独有】%d 条（Python 没有，多半是有意的）" % len(extra))
        for method, path in extra:
            print("    %-5s %s" % (method, path))
        print()

    report = {
        "python_total": len(py),
        "cpp_total": len(cpp),
        "missing": {k: sorted(v) for k, v in missing.items()},
        "cpp_only": ["%s %s" % (m, p) for m, p in extra],
        # 完整的路由清单。对拍程序的实时模式靠它决定要打哪些接口——
        # 手写一份清单的话，新加的路由不会自动进对拍，
        # 而"新加的接口没被对拍过"正是最需要对拍的情形。
        "routes": [
            {"method": m, "path": p, "in_cpp": (m, p) in cpp}
            for m, p in sorted(py)
        ],
    }
    target = REPO / "cpp" / "tests" / "golden" / "route_audit.json"
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("报告写到", target)


if __name__ == "__main__":
    main()
