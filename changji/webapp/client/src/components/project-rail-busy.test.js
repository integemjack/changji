/**
 * 项目库那条栏上的「改名」「删掉」，**正在跑的时候要拦住**。
 *
 * 这道闸原来是"开 ⋯ 菜单那一下现问一次 `/api/system`"，而前面还挡着一句
 *
 *     if (!(runner.running || writer.running)) { busyPaths = 空集; return }
 *
 * 那一句是个 **fail-open 的短路**：`useRun` 只有镜头页在驱动、`useWriter`
 * 只有故事页和设定页那一格在驱动，而这条栏在**每一页**上。刚打开浏览器、
 * 或者压根没去过镜头页的时候两个旗子都是假的——于是它不问就断言"谁都没在
 * 跑"，而引擎那头可能正在给这部剧写分镜。实测过：假引擎报着「正在给 ep02
 * 出分镜」，那两颗照样全亮。
 *
 * 反方向那个坑也栽过（旗子卡在真上 → 每一条都变灰，提示还写着「正在跑，
 * 跑完再删」），所以三态必须分清：null = 还不知道（fail-closed），
 * 空集 = 确定谁都没在跑，有内容 = 就这几个。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const RAIL = fs.readFileSync(
  fileURLToPath(new URL('./ProjectRail.vue', import.meta.url)),
  'utf8',
)

describe('谁在跑这一问', () => {
  it('从顶栏那份系统表读，不再自己现问一次', () => {
    expect(RAIL).toContain('useSystemFeed')
    const at = RAIL.indexOf('const busyPaths = computed(')
    expect(at, 'busyPaths 不再是个 computed 了').toBeGreaterThan(0)
    expect(RAIL.slice(at, at + 300)).toContain('stat.value?.jobs')
  })

  it('**那句 fail-open 的短路不许回来**', () => {
    // 旗子是假的不等于没在跑——这条栏在每一页上，而驱动那两个 store 的
    // 只有镜头页 / 故事页。
    // （认的是**代码**：那段话本身还留在注释里，记着当初为什么错。）
    expect(RAIL).not.toContain('function refreshBusy')
    expect(RAIL).not.toContain('api.system()')
    // 赋值（`= ` 后面不是另一个 `=`）——`=== null` 那句不算
    expect(RAIL).not.toMatch(/busyPaths\.value = [^=]/)
  })

  it('三态要分清：不知道的时候一律挡住', () => {
    const at = RAIL.indexOf('const busyPaths = computed(')
    expect(RAIL.slice(at, at + 300)).toContain('if (!jobs) return null')
    const gate = RAIL.indexOf('function busyProject(')
    expect(gate).toBeGreaterThan(0)
    expect(RAIL.slice(gate, gate + 260)).toContain('=== null) return true')
  })
})

/*
 * 「跑完了重拉一次」那条下降沿原来也钉在这儿。后来查出来那是**四处同一个
 * 根**（项目库、这一集那排标签、设定页那行标签、成片页），判据一起收进了
 * `useLongRunning`，用例也跟着搬到 `src/undriven-stores.test.js` 一处去钉
 * ——留在这儿的话四处各钉一遍，而它们只有一条规矩。
 */
