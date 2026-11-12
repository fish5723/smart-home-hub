"""校验 roadmap.html：JS 数据结构 + HTML 结构 + 模板完整性。

用法：python tools/check_roadmap.py
用途：修改 roadmap.html 后跑一次，防止中文引号写成 ASCII 引号把 JS 字符串截断
      （这个坑已经踩过一次）。
"""
import io
import json
import re
import sys

PATH = 'roadmap.html'

h = io.open(PATH, encoding='utf-8').read()


def jsobj_to_json(s):
    """把 JS 对象字面量（裸键名）转成 JSON：只在字符串之外给键名加引号。"""
    out = []
    i = 0
    in_str = False
    n = len(s)
    while i < n:
        c = s[i]
        if in_str:
            out.append(c)
            if c == chr(92):          # 反斜杠转义
                out.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < n and (s[j].isalnum() or s[j] == '_'):
                j += 1
            if j < n and s[j] == ':':
                out.append('"' + s[i:j] + '"')
                i = j
                continue
        out.append(c)
        i += 1
    return ''.join(out)


fails = []

# ---------- 1. PHASES 数据 ----------
m = re.search(r'const PHASES = \[(.*?)\n\];', h, re.S)
if not m:
    print('FAIL: 找不到 PHASES 数据块')
    sys.exit(1)

try:
    data = json.loads('[' + jsobj_to_json(m.group(1)) + ']')
except json.JSONDecodeError as e:
    print('FAIL: PHASES 不是合法 JSON —— %s' % e)
    print('      多半是中文引号被写成了 ASCII 引号，截断了 JS 字符串')
    sys.exit(1)

print('PHASES 解析成功')
print('  阶段数      =', len(data))
print('  任务总数    =', sum(len(p['tasks']) for p in data))
print('  学习点总数  =', sum(len(p['learn']) for p in data))
print('  阶段编号    =', [p['n'] for p in data])
print('  关键路径    = 阶段', [p['n'] for p in data if p['star']])

for field in ('reuse', 'tasks', 'learn', 'res', 'goal', 't', 's', 'dur'):
    missing = [p['n'] for p in data if not p.get(field)]
    if missing:
        fails.append('阶段 %s 缺字段 %s' % (missing, field))
print('  缺字段检查  =', fails or '全部完整')

# 任务 key 唯一性（渲染 id=t{i}_{j} 依赖它）
keys = set()
dup = 0
for i, p in enumerate(data):
    for j in range(len(p['tasks'])):
        k = 't%d_%d' % (i, j)
        if k in keys:
            dup += 1
        keys.add(k)
print('  重复任务key =', dup)
if dup:
    fails.append('存在重复任务 key')

# ---------- 2. HTML 结构 ----------
for t in ('<style>', '</style>', '<script>', '</script>', '<body>', '</body>', '</html>'):
    if h.count(t) != 1:
        fails.append('标签 %s 出现 %d 次（应为 1）' % (t, h.count(t)))

# 属性引号有没有被误替换成中文引号
if '=「' in h:
    fails.append('HTML 属性引号被破坏（出现 =「）')

# ---------- 3. 模板字段引用 ----------
# 注意：模板里是 ${p.tasks.map(...)} / ${p.reuse} 这类形式，所以只查字段名出现与否
for field in ('p.reuse', 'p.tasks', 'p.learn', 'p.res'):
    if field not in h:
        fails.append('模板未引用 %s' % field)

# ---------- 4. script 区括号/引号自洽 ----------
s = h.split('<script>')[1].split('</script>')[0]
if s.count('"') % 2:
    fails.append('script 区双引号数为奇数')
for a, b in (('{', '}'), ('(', ')'), ('[', ']')):
    if s.count(a) != s.count(b):
        fails.append('script 区括号不平衡 %s%s: %d' % (a, b, s.count(a) - s.count(b)))

print('  文件大小    = %.1f KB' % (len(h.encode('utf-8')) / 1024))
print()
if fails:
    print('校验失败：')
    for f in fails:
        print('  -', f)
    sys.exit(1)
print('全部校验通过')
