"""导出三个剧本接口的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_scripting_golden.py

产出 cpp/tests/golden/endpoints_scripting.json。

和别的导出脚本一样走**真实的 FastAPI 路由**，但多做一件事：
把大模型换成一个吐固定内容的桩。真调模型的话每次输出都不一样，
接口的形状就没法对拍了——而形状恰恰是这里唯一要验的东西。

桩装在 ScriptGenerator._complete 上，也就是发 HTTP 那一层。
提示词的拼接、返回的解析、响应体的组装全都走真实代码。
顺带把每次收到的提示词也录下来，C++ 侧要拿它验自己拼得对不对。
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
from changji.stages import script as sc                            # noqa: E402
from changji.web.server import create_app                          # noqa: E402

GOLDEN = HERE.parent / "golden"

SCRIPT_REPLY = json.dumps({
    "title": "雨夜天台",
    "logline": "等了七年的人今晚来了。",
    "beats": [
        {"kind": "action", "speaker": "", "text": "[0-3秒] 夜。天台。雨。"},
        {"kind": "dialogue", "speaker": "林晚", "text": "“你说过会来的。”"},
        {"kind": "action", "speaker": "", "text": "（陈默从阴影里走出来。）"},
        {"kind": "dialogue", "speaker": "陈默", "text": "我来了。晚了七年。"},
        {"kind": "dialogue", "speaker": "none", "text": "远处传来汽笛声。"},
    ],
}, ensure_ascii=False)

PREMISE_REPLY = json.dumps({
    "ideas": [
        {"title": "《雨夜天台》", "premise": "（林晚在天台等一个七年没出现的人。）",
         "hook": "“他会来吗？”"},
        {"title": "旧照片", "premise": "一张照片牵出十年前的火灾真相。",
         "hook": "照片背面写着她的名字。"},
        {"title": "错单", "premise": "外卖员送错一单，撞破一场骗局。",
         "hook": "那扇门后面不该有人。"},
    ]
}, ensure_ascii=False)


def make_stub(reply: str):
    """做一个替掉 _complete 的桩，返回 (函数, 收集到的提示词列表)。

    必须是**普通的 async 函数**，不能是带 __call__ 的对象。
    赋给类属性时，函数会走描述符协议自动绑定 self，而一个普通对象不会——
    结果是参数整体错位一个，表现为 500 而不是任何有用的报错。

    签名跟着 Python 侧走：_complete(self, prompt, schema=None, name="script")。
    """
    prompts: list[str] = []

    async def stub(_self, prompt, schema=None, name="script"):
        prompts.append(prompt)
        return reply

    return stub, prompts


def pristine_root() -> Path:
    """export_golden.py 建的那个真实项目。**只读**。"""
    exp = json.loads(
        io.open(GOLDEN / "project_expectations.json", encoding="utf-8").read())
    return GOLDEN / exp["root_name"]


def working_copy(tmp: Path, tag: str) -> Path:
    """给每个用例一份干净拷贝。

    这一步不能省。/api/script/write 会把梗概存回 project.json——
    直接对着基准项目跑的话，第一条用例就把语料改了，
    后面所有依赖那份项目的对拍（test_readonly 之类）全跟着挂。
    这个坑我踩过一次，症状是完全不相干的测试开始失败。
    """
    dst = tmp / tag
    shutil.copytree(pristine_root(), dst)
    return dst


def run_cases(tmp: Path) -> list[dict]:
    app = create_app(load_settings())
    client = TestClient(app, raise_server_exceptions=False)

    cases: list[dict] = []
    counter = [0]

    def call(name, url, body, stub_reply):
        # 每条用例一份拷贝，互不影响
        counter[0] += 1
        root = working_copy(tmp, "case%02d" % counter[0])
        if body.get("project") == "__PROJECT__":
            body = dict(body, project=str(root))
        stub, prompts = make_stub(stub_reply)
        orig = sc.ScriptGenerator._complete
        sc.ScriptGenerator._complete = stub
        try:
            r = client.post(url, json=body)
            try:
                payload = r.json() if r.content else None
            except Exception:
                payload = {'__raw__': r.text[:600]}
        finally:
            sc.ScriptGenerator._complete = orig
        # compare 的含义，沿用 endpoints_editing 那套约定：
        #   full        —— body 逐字段深比较
        #   detail_str  —— 只比状态码，外加 detail 是个非空字符串
        #   detail_arr  —— 比 detail 数组第一项的 type 和 loc，msg 文字不比
        #                  （pydantic 的报错文字里嵌着版本号和文档 URL）
        if r.status_code == 200:
            compare = "full"
        elif isinstance(payload, dict) and isinstance(payload.get("detail"), list):
            compare = "detail_arr"
        else:
            compare = "detail_str"

        # C++ 侧预期的状态码。只有一处不一样，见下面 cpp_note。
        cpp_status = r.status_code
        cpp_note = None
        if r.status_code == 500:
            # script._parse 里 _extract_json 抛的是 StoryboardError 不是
            # ScriptError，而这个路由只 catch ScriptError，于是"模型吐了一坨
            # 不是 JSON 的东西"这个最常见的失败变成了 500。
            # 和 /api/bible 是同一个 bug，见方案里那一节。C++ 侧统一回 400。
            cpp_status = 400
            cpp_note = "Python 的 StoryboardError 穿透 bug，C++ 有意回 400"
            compare = "detail_str"

        cases.append({
            "name": name,
            "url": url,
            "body": body,
            "llm_reply": stub_reply,
            "compare": compare,
            "cpp_status": cpp_status,
            "cpp_note": cpp_note,
            # 提示词也录下来。C++ 要验自己拼得和 Python 一样——
            # 只比响应体的话，提示词错了也看不出来，但模型的输出会悄悄变。
            "prompts": prompts,
            "status": r.status_code,
            "response": payload,
        })

    # ---- 选题 ----
    call("选题：默认", "/api/script/premise",
         {"project": "__PROJECT__"}, PREMISE_REPLY)
    call("选题：带关键词", "/api/script/premise",
         {"project": "__PROJECT__", "keywords": "都市 悬疑"}, PREMISE_REPLY)
    call("选题：要五个", "/api/script/premise",
         {"project": "__PROJECT__", "count": 5}, PREMISE_REPLY)
    call("选题：count 越界", "/api/script/premise",
         {"project": "__PROJECT__", "count": 9}, PREMISE_REPLY)
    call("选题：多余字段", "/api/script/premise",
         {"project": "__PROJECT__", "typo": 1}, PREMISE_REPLY)
    call("选题：项目不存在", "/api/script/premise",
         {"project": "Z:/根本没有这个目录"}, PREMISE_REPLY)

    # ---- 写一集 ----
    base = {"project": "__PROJECT__", "premise": "林晚在天台等一个七年没出现的人。"}
    call("写一集：默认", "/api/script/write", dict(base), SCRIPT_REPLY)
    call("写一集：不接前文", "/api/script/write",
         dict(base, continue_from_previous=False), SCRIPT_REPLY)
    call("写一集：不沿用角色", "/api/script/write",
         dict(base, reuse_characters=False), SCRIPT_REPLY)
    call("写一集：指定集号", "/api/script/write",
         dict(base, episode_id="ep_01"), SCRIPT_REPLY)
    call("写一集：短时长", "/api/script/write",
         dict(base, duration_s=15.0), SCRIPT_REPLY)
    call("写一集：时长越界", "/api/script/write",
         dict(base, duration_s=9999.0), SCRIPT_REPLY)
    call("写一集：时长为零", "/api/script/write",
         dict(base, duration_s=0.0), SCRIPT_REPLY)
    call("写一集：缺 premise", "/api/script/write",
         {"project": "__PROJECT__"}, SCRIPT_REPLY)
    call("写一集：多余字段", "/api/script/write",
         dict(base, typo="x"), SCRIPT_REPLY)
    call("写一集：模型吐了垃圾", "/api/script/write",
         dict(base), "抱歉，我无法完成这个请求。")
    call("写一集：模型没写台词", "/api/script/write",
         dict(base), json.dumps(
             {"title": "t", "logline": "l",
              "beats": [{"kind": "action", "speaker": "", "text": "雨。"}]},
             ensure_ascii=False))

    # ---- 预告片 ----
    call("预告：默认", "/api/script/trailer", {"project": "__PROJECT__"}, SCRIPT_REPLY)
    call("预告：指定剧集", "/api/script/trailer",
         {"project": "__PROJECT__", "episode_ids": ["ep_01"]}, SCRIPT_REPLY)
    call("预告：指定不存在的剧集", "/api/script/trailer",
         {"project": "__PROJECT__", "episode_ids": ["ep_99"]}, SCRIPT_REPLY)
    call("预告：不沿用角色", "/api/script/trailer",
         {"project": "__PROJECT__", "reuse_characters": False}, SCRIPT_REPLY)
    call("预告：时长越界", "/api/script/trailer",
         {"project": "__PROJECT__", "duration_s": 999.0}, SCRIPT_REPLY)
    call("预告：多余字段", "/api/script/trailer",
         {"project": "__PROJECT__", "typo": 1}, SCRIPT_REPLY)

    return cases


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="changji_scripting_") as td:
        cases = run_cases(Path(td))
    data = {
        "note": "由 cpp/tests/export_scripting_golden.py 生成，不要手改",
        "cases": cases,
    }
    target = GOLDEN / "endpoints_scripting.json"
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("写好了", target)
    for c in data["cases"]:
        print("  %-24s %s %d" % (c["name"], c["url"], c["status"]))


if __name__ == "__main__":
    main()
