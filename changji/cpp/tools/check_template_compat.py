"""确认 C++ 生成的配置模板，Python 引擎还读得动。

**这条不变量今天断过一次。** `--init-config` 写出的模板里有 `[models]` 这一节，
而 Python 的 `Settings` 是 `extra="forbid"`——只要用户配置里出现它，
**Python 整份加载失败、后端根本起不来**，报的是
`Extra inputs are not permitted`。

那次的症状离原因很远：对拍里 152 条全变成"Python 侧：连不上"，
看起来像端口或网络的问题，杀进程重跑都不管用，最后是手起 serve_python.py
才看见那行 pydantic 报错。

迁移期间两个后端共用一份用户配置，所以模板里**每一个生效的键**
（不以 # 开头的）都必须是 Python 也认的。C++ 独有的东西一律注释掉，
并在注释里写明取消注释之后会怎样。

这个检查没法放进 C++ 的单元测试里——那边跑不了 Python。所以做成脚本，
由 verify_all.ps1 调。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/check_template_compat.py
"""

from __future__ import annotations

import io
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "src"))


def template() -> str:
    """从 settings.cpp 里把 kDefaultToml 那段原始字符串抠出来。"""
    src = io.open(ROOT / "cpp" / "src" / "config" / "settings.cpp",
                  encoding="utf-8").read()
    start = src.index('kDefaultToml = R"(') + len('kDefaultToml = R"(')
    end = src.index(')"', start)
    return src[start:end]


def main() -> int:
    tpl = template()

    # 顺带看一眼有没有 C++ 独有的节露出来。这是"预检"，
    # 真正的判据是下面那次加载——万一以后 Python 也认了 [models]，
    # 这里的名单会过时，而加载不会。
    active = [ln.strip() for ln in tpl.split("\n")
              if ln.strip() and not ln.strip().startswith("#")]
    for line in active:
        if line.startswith("[") and line.strip("[]") in ("models",):
            print(f"模板里有一节 Python 不认：{line}")
            print("  迁移期间两个后端共用一份用户配置，这一节必须注释掉。")
            return 1

    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        io.open(d / "changji.toml", "w", encoding="utf-8").write(tpl)
        try:
            from changji.config import load_settings
            load_settings(d)
        except Exception as exc:            # noqa: BLE001 —— 什么错都算错
            print("Python 引擎读不了 C++ 生成的配置模板：")
            print(" ", str(exc)[:600])
            print()
            print("迁移期间两个后端共用一份用户配置。模板里每一个生效的键")
            print("都必须是 Python 也认的；C++ 独有的一律注释掉。")
            return 1

    print(f"配置模板 Python 也读得动（{len(active)} 个生效的键）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
