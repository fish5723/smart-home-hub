#!/usr/bin/env node
/* Generate LVGL 9 binfonts using the same options as the checked-in fallback fonts.
 *
 * Character set = 内置字体 ui_font_cjk_18.c 的 --symbols 基线
 *              ∪ 脚本内固定标点 extras
 *              ∪ 固件 main/ 下 C 源码**字符串字面量**里出现的汉字
 *              ∪ HTML 原型 zh 文案里的汉字
 * 这样新增状态词只要写进 C 源码，重跑本脚本即可自动补字形，避免实机缺字。
 */
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = path.resolve(import.meta.dirname, '..');
const sourceFont = 'E:/SourceHanSansCN-Normal.otf';
const converter = 'C:/Users/A/AppData/Roaming/npm/lv_font_conv.cmd';
const outputDir = path.join(root, 'ui', 'assets', 'generated_cn_font');
const simulatorDir = path.join(root, 'simulator', 'fonts', 'cn_font');
const sourceDir = path.join(root, 'firmware', 'p4-hub', 'main');
const prototypeFile = path.join(root, 'ui', 'prototype', 'stage4-ui.html');

const cjkSource = fs.readFileSync(
  path.join(root, 'firmware', 'p4-hub', 'main', 'ui', 'fonts', 'ui_font_cjk_18.c'),
  'utf8'
);
const symbolsMatch = cjkSource.match(/--symbols\s+([^\s]+)\s+--output/);
if (!symbolsMatch) throw new Error('Could not find the existing runtime character list');

const extras = '、。，：；？！“”‘’（）【】《》〈〉—…·°℃℉％±×÷≈≤≥←↑→↓↔↗↘✓✗★☆●○■□▲▼◆①②③④⑤⑥⑦⑧⑨⑩§¶†';

/* 只取字符串字面量里的汉字，避免把注释里的中文全部塞进字体（体积/加载）。 */
function scanStringLiterals(text) {
  const stripped = text.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
  const found = new Set();
  const re = /"((?:\\.|[^"\\])*)"/g;
  let m;
  while ((m = re.exec(stripped)) !== null) {
    for (const ch of m[1]) {
      const cp = ch.codePointAt(0);
      if ((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xff00 && cp <= 0xffef) ||
          (cp >= 0x3000 && cp <= 0x303f)) {
        found.add(ch);
      }
    }
  }
  return found;
}

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) walk(full, out);
    else if (/\.(c|h)$/.test(entry.name)) out.push(full);
  }
  return out;
}

const scanned = new Set();
for (const file of walk(sourceDir, [])) {
  for (const ch of scanStringLiterals(fs.readFileSync(file, 'utf8'))) scanned.add(ch);
}

if (fs.existsSync(prototypeFile)) {
  const html = fs.readFileSync(prototypeFile, 'utf8');
  const re = /zh\s*:\s*"((?:\\.|[^"\\])*)"/g;
  let m;
  while ((m = re.exec(html)) !== null) {
    for (const ch of m[1]) {
      const cp = ch.codePointAt(0);
      if ((cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0xff00 && cp <= 0xffef)) scanned.add(ch);
    }
  }
}

const symbols = [...new Set([...symbolsMatch[1], ...extras, ...scanned])].join('');
fs.mkdirSync(outputDir, { recursive: true });
fs.mkdirSync(simulatorDir, { recursive: true });

for (const size of [14, 16, 18, 24, 32, 48]) {
  const output = path.join(outputDir, `fontcn${size}.bin`);
  const args = [
    '--font', sourceFont,
    '--size', String(size),
    '--bpp', '4',
    '--no-compress',
    '--no-kerning',
    '--format', 'bin',
    '--range', '0x20-0x7E,0xB0,0x2022,0x2192',
    '--symbols', symbols,
    '--output', output,
  ];
  const result = spawnSync(converter, args, { encoding: 'utf8', windowsHide: true, shell: true });
  process.stdout.write(result.stdout || '');
  process.stderr.write(result.stderr || '');
  if (result.status !== 0) throw new Error(`lv_font_conv failed for ${size}px (exit ${result.status})`);
  fs.copyFileSync(output, path.join(simulatorDir, path.basename(output)));
  const sizeBytes = fs.statSync(output).size;
  console.log(`generated ${path.basename(output)}: ${sizeBytes} bytes`);
}

console.log(`characters: ${symbols.length} (baseline ${symbolsMatch[1].length} + extras + scanned ${scanned.size})`);
