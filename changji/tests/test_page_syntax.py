# -*- coding: utf-8 -*-
"""界面脚本的语法体检。

整个界面是一个文件，那段 JS 住在 Python 的三引号字符串里。
反斜杠先被 Python 吃一道，稍不留神 "\\n" 就变成真的换行，
字符串没闭合，整段脚本一行都跑不起来——不是某个按钮坏了，
是所有按钮一起坏。

这种错误后端测试一个都发现不了，页面照样返回 200，
只有真去浏览器里点才知道。所以在这里挡一道。
"""
import re
import shutil
import subprocess

import pytest

from changji.web.page import render_page


def _script() -> str:
    html = render_page("http://x:8188", "/p")
    m = re.search(r"<script>(.*)</script>", html, re.S)
    assert m, "页面里找不到脚本"
    return m.group(1)


class TestNoRawNewlineInStrings:
    """不依赖 node 也要能挡住最常见的那一种。

    数引号个数会被正则字面量和引号套引号骗到，所以只认一个特征：
    行尾是一个刚被打开的引号。那必然是本该写转义换行、结果写成了
    真的换行。
    """

    def test_没有行尾开着的字符串(self):
        bad = []
        for i, line in enumerate(_script().splitlines(), 1):
            stripped = line.rstrip()
            # 前面是括号逗号加号等号，后面就是行尾，这个引号是打开的
            if re.search(r"""[(,+=]\s*["']$""", stripped):
                bad.append((i, stripped[-50:]))
        assert not bad, (
            "这些行的字符串在行尾开着没闭合，"
            f"多半是换行被写成了真的换行：{bad}")


class TestNodeCheck:
    """有 node 就交给它做一遍真正的语法解析。"""

    def test_脚本能通过语法检查(self, tmp_path):
        node = shutil.which("node")
        if not node:
            pytest.skip("这台机器上没有 node，跳过真解析")
        f = tmp_path / "page.js"
        f.write_text(_script(), encoding="utf-8")
        r = subprocess.run([node, "--check", str(f)],
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, f"界面脚本语法有问题：\n{r.stderr[:800]}"


class TestPageShape:
    def test_八个标签页都在(self):
        html = render_page()
        for name in ("环境", "项目", "角色场景", "剧本",
                     "分镜", "运行", "成片", "参数"):
            assert f">{name}<" in html, f"少了 {name} 这一页"

    def test_每个_onclick_都指向定义过的函数(self):
        """按钮写了 onclick 却没定义那个函数，点下去只会静悄悄什么都不发生。"""
        script = _script()
        html = render_page()
        defined = set(re.findall(r"(?:async\s+)?function\s+([A-Za-z_$][\w$]*)",
                                 script))
        defined |= set(re.findall(r"(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=",
                                  script))
        called = set(re.findall(r'onclick="([A-Za-z_$][\w$]*)\(', html))
        missing = sorted(called - defined)
        assert not missing, f"这些按钮点了没反应：{missing}"
