/**
 * 章模式的台词行带着「怎么说的」那个括号，阅读器要认得出说话人。
 *
 * 引擎把 `delivery` 渲染成名字后面的括号（prompts.toml 的 seg2_chapter
 * 第 8 条：「这句是怎么说的…写进 delivery，不写进 text」）：
 *
 *     曾老板（低声）：宋律师，你也在其中。
 *
 * 冒号前是「曾老板（低声）」，不在人物名单里——2026-09-17 之前整行会被判成
 * 描写。后果两层：主审阅页把每一份章剧本都报成「0 句台词 · 56 段描写」
 * （而那一份通篇是对白），台词也拿不到说话人配色。
 *
 * **更糟的是它会骗人**：我上一轮正是拿那个 0 当证据，判定一次提示词改动
 * 「让整集变默片」，把一份本来没问题的改动整份退了回去。所以这一条钉的
 * 不只是个显示 bug，是一个**会导致错误结论的读数**。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VUE = fs.readFileSync(
  fileURLToPath(new URL('./ScriptReader.vue', import.meta.url)),
  'utf8',
)

/** 把组件里那一行摘人名的逻辑取出来，真的跑一遍。 */
function speakerOf(line) {
  const src = VUE.match(/const name = nameRaw\.replace\((.+?)\)\.trim\(\)/)
  expect(src, '摘括号那一行还在吗？').toBeTruthy()
  const at = line.search(/[：:]/)
  const nameRaw = at > 0 ? line.slice(0, at).trim() : ''
  // eslint-disable-next-line no-new-func
  return new Function('nameRaw', `return nameRaw.replace(${src[1]}).trim()`)(nameRaw)
}

describe('台词行：名字后面那个括号要摘掉再认人', () => {
  it('章模式带 delivery 的那种', () => {
    expect(speakerOf('曾老板（低声）：宋律师，你也在其中。')).toBe('曾老板')
    expect(speakerOf('郑哥（声音嘶哑）：我杀了李明。')).toBe('郑哥')
    expect(speakerOf('宋律师（笑着对镜头说）：资金转移合法。')).toBe('宋律师')
  })

  it('半角括号也认——手改的剧本常打成半角', () => {
    expect(speakerOf('曾老板(平静)：仓库角落找到的。')).toBe('曾老板')
  })

  it('没有括号的照旧', () => {
    expect(speakerOf('曾老板：快走，从后门。')).toBe('曾老板')
  })

  it('**名字里本来就带括号的不能误伤**', () => {
    // 只摘**结尾**那一个括号。中间的括号是名字的一部分，摘了就认不出人了。
    expect(speakerOf('老王（老板）的司机：车在外面。')).toBe('老王（老板）的司机')
  })
})
