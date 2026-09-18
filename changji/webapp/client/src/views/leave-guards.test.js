/**
 * 「走开就没了」的东西，**两条出路都要拦**。
 *
 * 浏览器的 `beforeunload` 只在真的要离开这个文档时才问：关标签页、刷新、
 * 输地址。**点顶栏那排「项目 / 故事 / 设定 / 这一章」它一声不吭**——而那是
 * 这套界面里最常走的一条路，四步流程本来就要在这几页之间来回。
 *
 * 这几页各自都挂过 `beforeunload`，每一页都漏了另一半，注释里还都写着
 * "刷新和关标签页"这半句，像是已经想全了（原来还有一行「设定/分集：剪好
 * 还没存的那条预告片」，那一格 2026-09-18 连文件一起下了）：
 *
 *     剧本页       写出来还没采用的那一篇、正在写的那一篇
 *     设定/角色    抽屉里改了没存的外观描述
 *     设定/场景    同上
 *     镜头页       抽屉里改了没存的那一镜
 *
 * 这条不报错、不崩，丢的时候屏幕上一个字都没有。所以直接读源码钉住：
 * 谁挂了 `beforeunload`，谁就得有 `onBeforeRouteLeave`。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('..', import.meta.url))

function vueFiles(dir = SRC, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name)
    if (e.isDirectory()) vueFiles(p, out)
    else if (e.name.endsWith('.vue')) out.push(p)
  }
  return out
}

const files = vueFiles().map((p) => ({
  rel: path.relative(SRC, p),
  src: fs.readFileSync(p, 'utf8'),
}))

describe('走开之前拦一下', () => {
  const guarded = files.filter((f) => f.src.includes("'beforeunload'"))

  it('挂了 beforeunload 的页一个都不少（少了说明有人把它删了，那条用例就白站着）', () => {
    expect(guarded.length).toBeGreaterThanOrEqual(5)
  })

  /**
   * 故事页是例外，而且是**更好的那种**：它的字冲得出去。
   *
   * `onUnmounted` 里那句 `flushAll()` 把排着的自动保存全发出来，所以走开
   * 不用问——注释就在那儿（「冲出去是安全的：请求已经发出，闭包还活着」）。
   * 别的几页丢的东西引擎那头没有收的地方，只能问。
   */
  const FLUSHED = 'StoryView.vue'

  it('每一页都要同时拦住站内换页', () => {
    for (const f of guarded) {
      if (f.rel.endsWith(FLUSHED)) continue
      // 认的是**调用**，不是那句 import：只 import 不用等于没拦
      expect(f.src, `${f.rel} 只拦了关标签页和刷新`).toContain('onBeforeRouteLeave(')
    }
  })

  it('那个例外得一直是例外：故事页走开时要把字冲出去', () => {
    const story = guarded.find((f) => f.rel.endsWith(FLUSHED))
    expect(story).toBeTruthy()
    // 冲那一下必须在卸载里，不是随手摆在哪儿
    const at = story.src.indexOf('onUnmounted(')
    expect(at).toBeGreaterThan(0)
    expect(story.src.slice(at, at + 2000)).toContain('flushAll()')
  })

  it('拦下来之后不能让标签页名字说谎', () => {
    // afterEach 连没走成的导航也会走到，照着 to 改标题的话，标签页上
    // 写着「故事」而屏幕上还是「这一章」。见 router/page-title。
    const router = fs.readFileSync(fileURLToPath(new URL('../router/index.js', import.meta.url)), 'utf8')
    expect(router).toMatch(/afterEach\(\(to, _from, failure\)/)
    expect(router).toContain('applyTitle(to, failure)')
  })
})
