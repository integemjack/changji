"""起 Python 后端，给对拍用。

单独一个文件而不是 `python -c "..."`：那条命令行里有分号和引号，
PowerShell 转义一遍、cmd 再转一遍，很容易被拆坏——实测就被拆成了
一句 `import` 然后语法错误。

跑法（在 changji/ 下）：
    .venv/Scripts/python.exe cpp/tools/serve_python.py [端口]
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "src"))

from changji.config import load_settings          # noqa: E402
from changji.web.server import run_server         # noqa: E402


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8124
    run_server(load_settings(), host="127.0.0.1", port=port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
