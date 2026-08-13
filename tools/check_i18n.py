#!/usr/bin/env python3
"""检查每个 T(...) / Tr.t(...) 的中文原文都在 src/I18n.cpp 的翻译表里。

    python tools/check_i18n.py

漏一条不会崩、不会有编译错误 —— 只是把界面切到英文时那一处仍显示中文，而且要
正好走到那条分支才看得见（"配对失败"、"证书不匹配"这类文案恰好都在少见分支上）。
所以用脚本扫，别靠眼睛。

反过来也报：表里有、代码里没有的条目，多半是改文案时漏了一头。
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# 相邻字符串字面量会被编译器拼起来，所以这里也得把它们拼起来再比。
# 只认双引号串，转义序列原样保留（表里也是原样写的）。
STRING = r'"(?:[^"\\]|\\.)*"'
ADJACENT = rf'{STRING}(?:\s*{STRING})*'

CALL_CPP = re.compile(rf'\bT\(\s*QStringLiteral\(\s*({ADJACENT})\s*\)')
CALL_QML = re.compile(rf'\bTr\.t\(\s*({ADJACENT})\s*\)')
ENTRY = re.compile(rf'^\s*\{{\s*({ADJACENT})\s*,', re.M)


def join_literals(text):
    """把相邻的字面量拼成一个 Python 字符串。"""
    out = []
    for piece in re.findall(STRING, text):
        body = piece[1:-1]
        out.append(body.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\'))
    return ''.join(out)


def has_cjk(s):
    return any('一' <= ch <= '鿿' for ch in s)


COMMENT = re.compile(rf'//[^\n]*|/\*.*?\*/|{STRING}', re.S)


def strip_comments(text):
    """去掉注释，保留字符串字面量。

    注释里出现 Tr.t("中文原文") 这类示例是常事（Tr.qml 的文档注释就有一个），
    照单全收会报一堆不存在的缺漏。字符串一起参与匹配是为了不被字符串里的 // 骗到。
    """
    return COMMENT.sub(lambda m: m.group(0) if m.group(0).startswith('"') else ' ', text)


def main():
    table_src = (ROOT / "src" / "I18n.cpp").read_text(encoding="utf-8")
    # 只取翻译表那一段，免得把别处的花括号也当成条目
    start = table_src.index("const Entry kEnglish[]")
    end = table_src.index("};", start)
    keys = {join_literals(m.group(1)) for m in ENTRY.finditer(table_src[start:end])}

    used = {}
    for path in sorted(ROOT.glob("src/*.cpp")) + sorted(ROOT.glob("qml/*.qml")):
        text = strip_comments(path.read_text(encoding="utf-8"))
        pattern = CALL_QML if path.suffix == ".qml" else CALL_CPP
        for m in pattern.finditer(text):
            used.setdefault(join_literals(m.group(1)), set()).add(path.name)

    missing = {k: v for k, v in used.items() if k not in keys and has_cjk(k)}
    unused = {k for k in keys if k not in used and has_cjk(k)}

    for key, where in sorted(missing.items()):
        print(f"缺翻译  {key!r}  ← {', '.join(sorted(where))}")
    for key in sorted(unused):
        print(f"表里多余 {key!r}")

    total = len(used)
    print(f"\n用到 {total} 条，表里 {len(keys)} 条，缺 {len(missing)} 条，多 {len(unused)} 条")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
