/**
 * 设置页顶上那个「写回配置文件」勾，**得管得到这一页上每一颗保存**。
 *
 * 它的说明写着「不勾：只对本次进程生效，重启就没了」，而它挂在小节导航
 * 那一排的最右边——也就是整页的位置。可它原来只管得到三节（引擎 / 配音 /
 * 装配与闸门，那三节走 `/api/settings` 和 `/api/connections`，两条接口一直
 * 收 `persist`）；「模型目录和下载」那一节走的是 `/bff/setup/download`，
 * 不勾也照样往盘上写，还回一句「存好了」。
 *
 * 漏一处不报错、不崩：人以为自己在试一个临时目录，配置文件已经被改了，
 * 而这件事要到下次重启才看得出来。所以直接读源码钉住，做法同
 * stopped-by-hand / upload-formats 那几条。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const read = (rel) => fs.readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8')

const VIEW = read('./SettingsView.vue')

/**
 * 这一页上所有"改引擎那份配置"的调用，连它后面那一小段。
 *
 * **`saveNodeConfig` 不在里头，那是另一回事**：它写的是 Node 转发层自己的
 * 地址和超时（`/bff/settings/config`），不是引擎的 changji.toml；那一层
 * 2026-09-12 删了，它那颗按钮现在挂在 `v-if="!embedded"` 上，由引擎自己
 * 答的时候根本不渲染。
 */
function saveCalls() {
  const out = []
  const re = /api\.(saveConnections|saveEngineSettings|startSetupDownload)\(/g
  for (let m = re.exec(VIEW); m; m = re.exec(VIEW)) {
    out.push({ name: m[1], near: VIEW.slice(m.index, m.index + 400) })
  }
  return out
}

describe('「写回配置文件」那个勾', () => {
  it('这一页上每一颗保存都要带上它', () => {
    const calls = saveCalls()
    // 少于三处多半是谁把调用改写成了别的形状，那时候这条用例就白站着了
    expect(calls.length).toBeGreaterThanOrEqual(3)
    for (const c of calls) {
      expect(c.near, `api.${c.name} 没带 persist`).toContain('persist')
    }
  })

  it('勾是一个变量，不是写死的 true', () => {
    // 写死的话这个勾就成了摆设，而它看着一直在工作
    expect(VIEW).toContain('const persist = ref(')
    expect(VIEW).not.toMatch(/persist:\s*true/)
  })
})

describe('引擎那头真的在看这个字段', () => {
  it('/bff/setup/download 会读 persist', () => {
    // 前端发了、引擎不看的话，屏幕上会说「已生效（重启后失效）」，
    // 而配置文件已经改了——比原来那条还糟，因为它还多说了一句假话。
    const src = read('../../../../cpp/src/http/setup_api.cpp')
    expect(src).toMatch(/body\.find\("persist"\)/)
    // 写回那一下要真的受它管
    expect(src).toMatch(/persist\(immediate,\s*want_persist\)/)
  })

  it('真要下模型时不认这个字段，而且是明说，不是偷偷写', () => {
    // 文件下完之后 on_item_done 必须把这一组写回配置文件（配置指着老模型
    // 而文件是新的，sd.cpp 不报错、只出一段花屏）。收下再反悔比当场说清楚糟。
    const src = read('../../../../cpp/src/http/setup_api.cpp')
    expect(src).toMatch(/if \(!want_persist && want_download\)/)
  })
})
