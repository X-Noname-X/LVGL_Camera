#!/bin/bash
#
# 生成中文字体子集。
#
# 为什么需要这个：
#   LVGL 内置的 lv_font_source_han_sans_sc_*_cjk 并不是完整的思源黑体，
#   实际只覆盖 1450 个码点、其中汉字仅 297 个，本项目的 UI 用字一个都不在
#   里面，所以必须在板子上自己生成一份。
#
# 为什么用两个源字体：
#   DroidSansFallbackFull.ttf 是 CJK「回退」字体，只含汉字和中文标点，
#   完全没有拉丁字母（fc-query 的 charset 从 0x20 直接跳到 0xE3F）。
#   所以 ASCII 必须另外从 DejaVuSans 取，两个 --font 合并。
#
# 加新 UI 文案后如果出现方框：把新字补进下面的 SYMBOLS，重跑本脚本即可。
# 脚本末尾会自动校验，缺字会直接报错退出，不会让你到板子上才发现。
#
# 用法：tools/gen_font.sh
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

FONT_LATIN=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
FONT_CJK=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf
OUT=app/font_zh_16.c
SIZE=16
BPP=4

# ---- 字符集 ----
# 界面用字 + 阶段 3~5 会用到词 + 中文数字与日期 + 中文标点。
# 宁可多带一些：一个字在 16px/4bpp 下约 128 字节，多带 100 个也才 13KB。
#
# 【重要】这里只放 DroidSansFallback 确实拥有的字——汉字和中文标点。
#   绝对不要把 — … · “ ” ‘ ’ 这类排印标点写进来，它们是 DejaVu 那边
#   用 --range 提供的。原因见下方 --range 处的注释，这是个会静默丢字形的坑。
SYMBOLS="\
预览区阶段接入摄像头拍照相册删除返回上一张下第共确认取消保存成功失败\
正在加载中错误无文件目录打开初始化编号数时间日期大小剩余空间内存卡\
已的了个是有在不一个这那什么为以到说会能可以好还没有新记录显示选择全部播放查看\
零一二三四五六七八九十百千万年月日时分秒\
，。、：；！？（）【】《》"

for f in "$FONT_LATIN" "$FONT_CJK"; do
    if [ ! -f "$f" ]; then
        echo "错误: 找不到源字体 $f" >&2
        exit 1
    fi
done

mkdir -p app

echo "生成 $OUT ..."
# DejaVu 除了 ASCII，还要独占负责破折号/省略号/中点/弯引号这些排印标点。
#
# 【坑】这些码点只能在这里要，不能同时出现在上面 CJK 的 --symbols 里。
#   lv_font_conv 合并多字体时，若某个字符被后声明的字体请求、而那个字体
#   并没有该字形，它会把先声明字体已经提供的同一个字形一并丢弃——
#   是静默丢，不报错。实测：U+2013 和 U+201a/201b 恰好 Droid 有，留下；
#   U+2014、U+201C/D、U+2026 等 Droid 没有，就全没了。
#   所以分工必须干净：汉字+中文标点走 CJK，ASCII+排印标点走 DejaVu。
npx --yes lv_font_conv@1.5.3 \
    --font "$FONT_LATIN" \
    --range 0x20-0x7E,0xB7,0x2013-0x2014,0x2018-0x201D,0x2026 \
    --font "$FONT_CJK" \
    --symbols "$SYMBOLS" \
    --size "$SIZE" \
    --bpp "$BPP" \
    --format lvgl \
    --no-compress \
    --lv-include lvgl.h \
    --lv-font-name font_zh_16 \
    -o "$OUT"

# ---- 生成后校验：缺字就报错，别等到板子上看到方框才发现 ----
SYMBOLS="$SYMBOLS" python3 - "$OUT" <<'PY'
import os, re, sys
path = sys.argv[1]
src = open(path, encoding='utf-8', errors='replace').read()
have = {int(m, 16) for m in re.findall(r'/\* U\+([0-9A-Fa-f]{4,6}) ', src)}

problems = []
# 1) ASCII 必须齐（0x20-0x7E），缺了就说明拉丁源字体没生效
miss_ascii = [chr(c) for c in range(0x20, 0x7F) if c not in have]
if miss_ascii:
    problems.append(f"ASCII 缺 {len(miss_ascii)} 个: {''.join(miss_ascii[:40])}")
# 2) SYMBOLS 里每个字都必须有
need = [c for c in os.environ['SYMBOLS'] if not c.isspace()]
miss = [c for c in need if ord(c) not in have]
if miss:
    problems.append(f"设定字符缺 {len(miss)} 个: {''.join(miss)}")

han = len([c for c in have if 0x4E00 <= c <= 0x9FFF])
print(f"  字形总数 {len(have)}（汉字 {han}）")

if problems:
    print("\n校验失败:", file=sys.stderr)
    for p in problems:
        print("  - " + p, file=sys.stderr)
    sys.exit(1)
print("  校验通过: ASCII 与全部设定字符均已覆盖")
PY

echo "完成: $(ls -lh "$OUT" | awk '{print $5}')"
