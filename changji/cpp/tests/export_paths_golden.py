r"""导出配置文件位置的解析结果，和 Python 比。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tests/export_paths_golden.py

产出 cpp/tests/golden/user_dirs.json。

**为什么这一条是承重的。** 迁移期间两个后端**共用一份用户配置**——
`check_template_compat.py` 整个存在的前提就是这个。要是两边算出来的
配置文件位置不一样，它们读的根本不是同一个文件，那个检查就在验一件
不成立的事。

⚠️ **写这份语料时发现两边取值的机制根本不一样。**

    Python  platformdirs → **Windows 已知文件夹 API**（ctypes）
    C++     直接读 `LOCALAPPDATA` 环境变量

实测：把 `LOCALAPPDATA` 设成 `D:\试试看\Local` 之后，
platformdirs 的 env 版函数确实回 `D:\试试看\Local`，
但它**实际用的是 ctypes 那版**，回的还是 `C:\Users\ultra\AppData\Local`。
也就是说 **Python 根本不看这个环境变量，而 C++ 只看它**。

今天两边一致，纯粹因为这台机器上环境变量和 API 给的是同一个值。
会分叉的情形：

  - `LOCALAPPDATA` 被改过（CI、沙箱、`set LOCALAPPDATA=...`）：
    **C++ 跟着走，Python 不跟**，两边读的就是两个文件了。
  - `LOCALAPPDATA` 没设：C++ 退回 `home/AppData/Local`，
    Python 还是问 API。企业环境里配置了文件夹重定向的话，这两个不一样。

所以这份语料只导**默认环境下的值**（那是唯一能公平比的），
机制差异写进方案，C++ 侧另有用例钉住"它认环境变量"这个行为本身。
"""
from __future__ import annotations

import io
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve()
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "src"))

from platformdirs import user_config_dir, user_data_dir     # noqa: E402

from changji.config import APP_NAME, user_config_path        # noqa: E402

DEST = REPO / "cpp" / "tests" / "golden" / "user_dirs.json"


def fwd(p) -> str:
    """正斜杠归一。两边 Path 打印的分隔符不一定一样，那不是要比的东西。"""
    return str(p).replace("\\", "/")


def main() -> int:
    payload = {
        "app_name": APP_NAME,
        # 文件名两边必须一样，否则读的不是同一个文件
        "config_file_name": "config.toml",
        "project_file_name": "changji.toml",
        # 默认环境下的实际取值。C++ 侧比这个——但只在
        # LOCALAPPDATA 和 API 给的值相同时才该相等，用例里写明了。
        "user_config_dir": fwd(user_config_dir(APP_NAME, appauthor=False)),
        "user_data_dir": fwd(user_data_dir(APP_NAME, appauthor=False)),
        "user_config_path": fwd(user_config_path()),
        "note": (
            "由 cpp/tests/export_paths_golden.py 生成，不要手改。"
            "只有默认环境下的值——Python 走 Windows 已知文件夹 API，"
            "C++ 读 LOCALAPPDATA，机制不同，改了环境变量就没有可比性。"
            "详见脚本开头和方案里那一节。"
        ),
    }
    DEST.parent.mkdir(parents=True, exist_ok=True)
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n")
    print(f"写入 {DEST}")
    for k in ("app_name", "config_file_name", "user_config_dir",
              "user_config_path"):
        print(f"  {k:20} {payload[k]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
