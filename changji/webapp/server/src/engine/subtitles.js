/**
 * 中文字幕断行。
 *
 * 断行自己算，不依赖渲染库的自动换行。libass 对中文只按字符断不按语义断，
 * 一句话会在词中间折断，观感很差。自己算好断点插换行符才对。
 */

// 断在这些标点之后是自然的
const BREAK_AFTER = '，。？！；：、…—'
// 这些不能出现在行首
const NO_LINE_START = '，。？！；：、）】》」』…—·%'
// 这些不能出现在行尾
const NO_LINE_END = '（【《「『'

/**
 * 东亚全角字符的码位范围（Unicode East_Asian_Width 的 W 和 F 两类）。
 *
 * JS 没有 unicodedata.east_asian_width，只能自己列范围。有意不包含
 * U+2014 破折号和 U+2026 省略号——它们在 Unicode 里是 Ambiguous 而不是
 * Wide，Python 版按 0.5 算，这里得跟它一致，否则同一句话两边算出的
 * 行宽不同，断行位置就对不上了。
 */
const WIDE = [
  [0x1100, 0x115f], // 韩文字母
  [0x2e80, 0x303e], // CJK 部首、中日韩符号与标点（、。「」等）
  [0x3041, 0x33ff], // 假名、注音、CJK 兼容
  [0x3400, 0x4dbf], // CJK 扩展 A
  [0x4e00, 0x9fff], // CJK 基本区
  [0xa000, 0xa4cf], // 彝文
  [0xac00, 0xd7a3], // 韩文音节
  [0xf900, 0xfaff], // CJK 兼容表意
  [0xfe30, 0xfe6f], // CJK 兼容形式
  [0xff00, 0xff60], // 全角形式（，？！等）
  [0xffe0, 0xffe6], // 全角符号
  [0x20000, 0x2fffd], // CJK 扩展 B 及以后
  [0x30000, 0x3fffd],
]

export function isWide(codePoint) {
  return WIDE.some(([lo, hi]) => codePoint >= lo && codePoint <= hi)
}

/** 按显示宽度算长度。全角算一，半角算半。 */
export function displayWidth(text) {
  let total = 0
  for (const ch of text) {
    total += isWide(ch.codePointAt(0)) ? 1 : 0.5
  }
  return total
}

/**
 * 找一个断点。返回切分位置。
 *
 * 注意用的是 UTF-16 下标而不是码位序号：调用方是拿它去 slice 字符串的，
 * 两者在有代理对时会差开。中文常用字都在基本平面，但字幕里出现一个
 * 生僻字或 emoji 就会踩到。
 */
export function findBreak(text, maxWidth) {
  let width = 0
  let limitIdx = text.length
  for (let i = 0; i < text.length; i += 1) {
    const cp = text.codePointAt(i)
    width += isWide(cp) ? 1 : 0.5
    if (width > maxWidth) {
      limitIdx = i
      break
    }
    if (cp > 0xffff) i += 1 // 代理对占两个下标
  }

  // 在宽度范围内从后往前找标点，只往回看 8 个字。
  //
  // 边界是「大于」不是「大于等于」：往回第 8 个字本身不算。
  // 这一格之差会让「你到底想说什么？我等了整整五年时间了啊」断成
  // 8+11 而不是 15+4——两行长短差一倍，看着就不对。
  const floor = Math.max(0, limitIdx - 8)
  for (let i = limitIdx - 1; i > floor; i -= 1) {
    if (BREAK_AFTER.includes(text[i])) return i + 1
  }

  // 没有标点就在宽度上限处断，但避开非法位置
  let cut = limitIdx
  let guard = 0
  while (cut > 1 && guard < 6) {
    guard += 1
    if (cut < text.length && NO_LINE_START.includes(text[cut])) {
      cut -= 1
      continue
    }
    if (NO_LINE_END.includes(text[cut - 1])) {
      cut -= 1
      continue
    }
    break
  }
  return Math.max(1, cut)
}

/**
 * 中文断行。
 *
 * 优先在标点后断，其次在宽度上限处断，但要避开标点不能在行首行尾的情况。
 * 超过行数上限就把剩下的并进最后一行——宁可最后一行长一点，也不丢字。
 */
export function wrapChinese(text, maxWidth = 15, maxLines = 2) {
  const trimmed = String(text).trim()
  if (!trimmed) return []
  if (displayWidth(trimmed) <= maxWidth) return [trimmed]

  const lines = []
  let rest = trimmed
  while (rest && lines.length < maxLines) {
    const cut = findBreak(rest, maxWidth)
    if (cut <= 0 || cut >= rest.length) {
      lines.push(rest)
      rest = ''
      break
    }
    lines.push(rest.slice(0, cut).trim())
    rest = rest.slice(cut).trim()
  }

  if (rest) {
    lines[lines.length - 1] = (lines[lines.length - 1] + rest).trim()
  }
  return lines.filter(Boolean)
}
