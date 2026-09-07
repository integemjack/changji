"""导出 /api/bible 和 /api/plan 的对拍语料。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_planning_golden.py

产出 cpp/tests/golden/endpoints_planning.json。

规矩同 export_scripting_golden.py：走真实路由，大模型换成桩，
每条用例一份项目拷贝（这两个接口都会写盘，不隔离会污染语料）。

这里的桩要分两种：/api/plan 一次请求里会先调角色圣经再调分镜，
两次的返回结构完全不同。所以桩按调用顺序吐。
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
from changji.stages import bible as bb                             # noqa: E402
from changji.stages import storyboard as sb                        # noqa: E402
from changji.web.server import create_app                          # noqa: E402

GOLDEN = HERE.parent / "golden"

BIBLE_REPLY = json.dumps({
    "characters": [
        {"key": "lin_wan", "name": "林晚",
         "identity": "二十七岁女性，外表冷静，内里执拗。",
         "body": "偏瘦，中等身高",
         "face": "黑色长直发，鹅蛋脸，杏眼，眼尾微垂，薄唇。",
         "attire": "白色衬衫，深色西装裤"},
        {"key": "chen_mo", "name": "陈默",
         "identity": "三十出头男性，沉默寡言",
         "body": "高瘦", "face": "短寸黑发，方脸，浓眉，单眼皮",
         "attire": "黑色风衣"},
    ],
    "locations": [
        {"key": "rooftop_night", "name": "夜间天台",
         "space": "水泥地面，锈蚀的护栏", "lighting": "夜间冷调顶光。",
         "palette": "冷蓝为主"},
    ],
    "global_style": "电影感，浅景深，冷色调。",
}, ensure_ascii=False)

# 这份分镜引用的角色 id 要和上面那份圣经对得上，
# 否则 validate_references 会拦下来。
STORYBOARD_REPLY = json.dumps({
    "shots": [
        {"shot_id": "ep01_sh001", "scene_id": "loc_rooftop_night", "order": 0,
         "first_frame_prompt": "夜间天台，雨中，女子背对镜头",
         "shot_size": "MS", "camera_angle": "eye_level", "duration_s": 5.0,
         "characters": [{"char_id": "c_lin_wan", "expression": "落寞",
                         "action": "扶着护栏", "face_pose": "back"}],
         "dialogue": [], "transition_in": "cut"},
        {"shot_id": "ep01_sh002", "scene_id": "loc_rooftop_night", "order": 1,
         "first_frame_prompt": "近景，女子转身",
         "shot_size": "CU", "camera_angle": "eye_level", "duration_s": 3.0,
         "characters": [{"char_id": "c_lin_wan", "expression": "克制",
                         "action": "转身", "face_pose": "front"}],
         "dialogue": [{"char_id": "c_lin_wan", "text": "你说过会来的"}],
         "transition_in": "cut"},
        {"shot_id": "ep01_sh003", "scene_id": "loc_rooftop_night", "order": 2,
         "first_frame_prompt": "男子从阴影里走出",
         "shot_size": "MCU", "camera_angle": "eye_level", "duration_s": 4.0,
         "characters": [{"char_id": "c_chen_mo", "expression": "克制",
                         "action": "握伞", "face_pose": "front"}],
         "dialogue": [{"char_id": "c_chen_mo", "text": "我来了。晚了七年。"}],
         "transition_in": "cut"},
    ]
}, ensure_ascii=False)

# 引用语料项目里**已有**的那个角色。不重出圣经的用例要用它——
# 用上面那份的话 validate_references 会拦下来，测到的就是引用校验
# 而不是"沿用已有资产"这条路径。
STORYBOARD_EXISTING = json.dumps({
    "shots": [
        {"shot_id": "ep01_sh001", "scene_id": "loc_rooftop", "order": 0,
         "first_frame_prompt": "夜间天台，雨中",
         "shot_size": "MS", "camera_angle": "eye_level", "duration_s": 5.0,
         "characters": [{"char_id": "c_lin_yuan", "expression": "落寞",
                         "action": "扶着护栏", "face_pose": "back"}],
         "dialogue": [], "transition_in": "cut"},
        {"shot_id": "ep01_sh002", "scene_id": "loc_rooftop", "order": 1,
         "first_frame_prompt": "近景，转身",
         "shot_size": "CU", "camera_angle": "eye_level", "duration_s": 3.0,
         "characters": [{"char_id": "c_lin_yuan", "expression": "克制",
                         "action": "转身", "face_pose": "front"}],
         "dialogue": [{"char_id": "c_lin_yuan", "text": "你说过会来的"}],
         "transition_in": "cut"},
    ]
}, ensure_ascii=False)

SCRIPT = """林晚站在天台边缘，雨水打湿了她的白衬衫。
林晚：你说过会来的。
陈默从阴影里走出来。
陈默：我来了。晚了七年。"""


def make_stubs(replies: dict):
    """按阶段装桩，返回收集到的提示词。

    必须是普通的 async 函数，不能是带 __call__ 的对象——赋给类属性时
    函数会走描述符协议自动绑定 self，普通对象不会，参数会整体错位一个。
    """
    prompts: list[dict] = []

    async def bible_stub(_self, prompt):
        prompts.append({"stage": "bible", "prompt": prompt})
        return replies["bible"]

    async def sb_stub(_self, prompt, schema=None):
        prompts.append({"stage": "storyboard", "prompt": prompt})
        return replies["storyboard"]

    return bible_stub, sb_stub, prompts


def pristine_root() -> Path:
    exp = json.loads(
        io.open(GOLDEN / "project_expectations.json", encoding="utf-8").read())
    return GOLDEN / exp["root_name"]


def run_cases(tmp: Path) -> list[dict]:
    app = create_app(load_settings())
    client = TestClient(app, raise_server_exceptions=False)
    cases: list[dict] = []
    counter = [0]

    def call(name, url, body, *, bible=BIBLE_REPLY, storyboard=STORYBOARD_REPLY,
             clear_assets=False):
        counter[0] += 1
        root = tmp / ("case%02d" % counter[0])
        shutil.copytree(pristine_root(), root)
        if clear_assets:
            # 有些用例要验"资产库是空的时候会先出角色圣经"。
            # 只清 characters 和 locations，style 原样留着——
            # AssetLibrary 是 extra="forbid"，多写一个键整份加载失败，
            # 而那个失败会以 400 的形式出现在一个和资产库无关的接口上。
            a = json.loads(io.open(root / "assets.json", encoding="utf-8").read())
            a["characters"] = {}
            a["locations"] = {}
            io.open(root / "assets.json", "w", encoding="utf-8").write(
                json.dumps(a, ensure_ascii=False))
        if body.get("project") == "__PROJECT__":
            body = dict(body, project=str(root))

        bs, ss, prompts = make_stubs({"bible": bible, "storyboard": storyboard})
        ob, osb = bb.BibleGenerator._complete, sb.StoryboardGenerator._complete
        bb.BibleGenerator._complete = bs
        sb.StoryboardGenerator._complete = ss
        try:
            r = client.post(url, json=body)
            try:
                payload = r.json() if r.content else None
            except Exception:                             # noqa: BLE001
                payload = {"__raw__": r.text[:600]}
        finally:
            bb.BibleGenerator._complete = ob
            sb.StoryboardGenerator._complete = osb

        if r.status_code == 200:
            compare = "full"
        elif isinstance(payload, dict) and isinstance(payload.get("detail"), list):
            compare = "detail_arr"
        else:
            compare = "detail_str"

        cpp_status, cpp_note = r.status_code, None
        if r.status_code == 500:
            # _extract_json 抛的是 StoryboardError，而 /api/bible 只
            # catch BibleError。和 /api/script/write 是同一个 bug。
            cpp_status, compare = 400, "detail_str"
            cpp_note = "Python 的 StoryboardError 穿透 bug，C++ 有意回 400"

        # 落盘之后的资产库和项目也要比——这两个接口都写盘
        assets_after = None
        project_after = None
        if r.status_code == 200:
            assets_after = json.loads(
                io.open(root / "assets.json", encoding="utf-8").read())
            project_after = json.loads(
                io.open(root / "project.json", encoding="utf-8").read())

        cases.append({
            "name": name, "url": url, "body": body,
            "clear_assets": clear_assets,
            "llm_bible": bible, "llm_storyboard": storyboard,
            "prompts": prompts,
            "status": r.status_code, "response": payload,
            "compare": compare, "cpp_status": cpp_status, "cpp_note": cpp_note,
            "assets_after": assets_after,
            "project_after": project_after,
        })

    P = "__PROJECT__"

    # ---- /api/bible ----
    call("圣经：只补新的", "/api/bible", {"project": P, "script": SCRIPT})
    call("圣经：覆盖", "/api/bible",
         {"project": P, "script": SCRIPT, "overwrite": True})
    call("圣经：用已存的剧本", "/api/bible", {"project": P})
    call("圣经：指定集号", "/api/bible", {"project": P, "episode_id": "ep01"})
    call("圣经：指定没剧本的集", "/api/bible",
         {"project": P, "episode_id": "ep02"})
    call("圣经：多余字段", "/api/bible", {"project": P, "typo": 1})
    call("圣经：项目不存在", "/api/bible", {"project": "Z:/没有这个目录"})
    call("圣经：模型吐了垃圾", "/api/bible", {"project": P},
         bible="抱歉，我做不到。")
    call("圣经：模型没产出角色", "/api/bible", {"project": P},
         bible=json.dumps({"characters": [], "locations": [],
                           "global_style": "x"}, ensure_ascii=False))

    # ---- /api/plan ----
    call("出分镜：资产库已有角色", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01"},
         storyboard=STORYBOARD_EXISTING)
    call("出分镜：资产库是空的", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01"},
         clear_assets=True)
    call("出分镜：强制重出圣经", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01",
          "regenerate_bible": True})
    call("出分镜：新建一集", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep09"},
         clear_assets=True)
    call("出分镜：自定时长", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01",
          "duration_s": 30.0}, clear_assets=True)
    call("出分镜：空剧本", "/api/plan",
         {"project": P, "script": "   ", "episode_id": "ep01"})
    call("出分镜：缺 script", "/api/plan", {"project": P, "episode_id": "ep01"})
    call("出分镜：多余字段照收", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01", "typo": 1},
         clear_assets=True)
    call("出分镜：引用了没注册的角色", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01"})
    call("出分镜：模型吐了垃圾", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01"},
         storyboard="抱歉，我做不到。")
    call("出分镜：分镜里没台词", "/api/plan",
         {"project": P, "script": SCRIPT, "episode_id": "ep01"},
         storyboard=json.dumps({"shots": [
             {"shot_id": "ep01_sh001", "scene_id": "s", "order": 0,
              "first_frame_prompt": "x", "shot_size": "MS", "duration_s": 5.0,
              "characters": [{"char_id": "c_lin_yuan", "expression": "x",
                              "action": "x", "face_pose": "front"}],
              "dialogue": []}]}, ensure_ascii=False))

    return cases


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="changji_planning_") as td:
        cases = run_cases(Path(td))
    data = {
        "note": "由 cpp/tests/export_planning_golden.py 生成，不要手改",
        "cases": cases,
    }
    target = GOLDEN / "endpoints_planning.json"
    with io.open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("写好了", target)
    for c in data["cases"]:
        note = "  <<< %s" % c["cpp_note"] if c["cpp_note"] else ""
        print("  %-22s %-14s %3d%s" % (c["name"], c["url"], c["status"], note))


if __name__ == "__main__":
    main()
