/**
 * 挑哪一档模型 = **这部剧**的设置，记进项目的 changji.toml。
 *
 * 用户 2026-09-15 定的：模型配置跟项目走，派到远端也照这份来，冲突时项目
 * 优先。这个文件钉住前端这一半——**两条路都得把项目带上**：
 *
 *   · 存这一档（`saveGroup`，「只存不下」那条）
 *   · 下这一档（`downloadGroup`）
 *   · 读回来（`setupState`）
 *
 * 少了最后一条尤其阴：挑完写进了项目，再打开那个窗口读的却是全局那一份
 * ——**看着像没保存上**，而人多半会再挑一次。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const STORE = fs.readFileSync(
  fileURLToPath(new URL('./models.js', import.meta.url)),
  'utf8',
)
const API = fs.readFileSync(
  fileURLToPath(new URL('../api/index.js', import.meta.url)),
  'utf8',
)
/** 引擎那一头：项目那份到底写没写。 */
const SETUP = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/http/setup_api.cpp', import.meta.url)),
  'utf8',
)
/** 挑档位的那个窗口。引擎算出来的话得有人摆出来。 */
const DIALOG = fs.readFileSync(
  fileURLToPath(new URL('../components/ModelDialog.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '')
}

const store = code(STORE)

describe('挑哪一档归这部剧', () => {
  it('有一个统一的"这趟属于哪部剧"，不是每处各写一遍', () => {
    expect(store).toMatch(/function owner\(\)/)
    const at = store.indexOf('function owner()')
    const fn = store.slice(at, at + 200)
    expect(fn).toContain('projectPath')
    // 没有项目时不带——设置页那一节和给对等机装模型都不属于任何一部剧
    expect(fn).toMatch(/\?\s*\{ project: p \}\s*:\s*\{\}/)
  })

  it('存和下两条路都带上项目', () => {
    for (const fnName of ['saveGroup', 'downloadGroup']) {
      const at = store.indexOf(`async function ${fnName}(`)
      expect(at, `${fnName} 挪走了？`).toBeGreaterThan(0)
      const body = store.slice(at, at + 400)
      expect(body, `${fnName} 没带项目`).toContain('...owner()')
    }
  })

  it('读回来那一趟也要带，不然看着像没保存上', () => {
    expect(store).toMatch(/api\.setupState\(useSession\(\)\.projectPath\)/)
    expect(code(API), 'setupState 不收项目参数').toMatch(
      /setupState:\s*\(project\)\s*=>/,
    )
  })

  it('引擎那头确实把它写进项目，而不是全局', () => {
    // 哪天这条改回只写全局，前端这几处就成了摆设。
    expect(SETUP, '不再写 models.pick 了？').toMatch(/"models\.pick"/)
    expect(SETUP, '不再认 body 里的 project 了？').toMatch(
      /body\.find\("project"\)/,
    )
  })

  it('认不出的档位 id 要说话，不许悄悄退回按文件名反推', () => {
    // 这一节是手写得到的（拷项目、直接改 toml）。写错一个字母时悄悄退回，
    // 界面显示的和跑的就是另一档，而没有任何一处提过。
    expect(SETUP).toMatch(/pick_problem/)
    expect(SETUP).toMatch(/pickProblem/)
  })

  it('那句话要真摆在挑档位的那个窗口上', () => {
    // 引擎算了、发了、界面一个字都不读——这条断言就是为那件事加的：
    // 那时候「显示的是一档、跑的是另一档」这件事仍然没有任何一处提过，
    // 只不过现在连排查的人翻接口返回都能看见它，界面上还是看不见。
    expect(code(DIALOG), 'ModelDialog 没把 pickProblem 摆出来').toMatch(
      /v-if="g\.pickProblem"/,
    )
    expect(code(DIALOG), '摆出来了却不显示内容').toMatch(/{{ g\.pickProblem }}/)
  })
})

/** 那张「机器 × 能力」的表。给别的机器装模型走的是它。 */
const MATRIX = fs.readFileSync(
  fileURLToPath(new URL('../components/NodeMatrix.vue', import.meta.url)),
  'utf8',
)
/** 引擎那一头：转发给别的机器那几条。 */
const SERVER = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/http/server.cpp', import.meta.url)),
  'utf8',
)

describe('给别的机器装模型，标准是这部剧挑的那一档', () => {
  // 「装成和本机同一套」拿本机的 selected 原样发过去。而派活时带给对面的
  // 是**项目**里那一档（worker_pool 的 pick）。问本机那一份时不带项目的
  // 话，两者就是两个东西——给对面装 A、真要用 B，而表现要到那一镜被对面
  // 拒了才看得出来（「这台装的是别的档」）。
  it('问本机那一份要带上项目', () => {
    expect(code(API), 'nodeSetup 不收项目').toMatch(
      /nodeSetup:\s*\(url,\s*project\)/,
    )
    expect(code(API), '收了却没发出去').toMatch(/path:\s*project/)
    expect(code(MATRIX), '表这边没把项目传进去').toMatch(
      /nodeSetup\((?:url|'local'),\s*session\.projectPath\)/,
    )
  })

  it('那颗按钮要说清照的是谁', () => {
    // 开着项目照这部剧那一档、没开照本机全局那一档——同一颗按钮两个意思。
    // 不说的话，用户以为一直照的是"本机"，而派活带过去的是项目那一档。
    expect(code(MATRIX), '按钮文案写死了').toMatch(/matchLabel/)
    expect(code(MATRIX)).toMatch(/装成这部剧要的那一套/)
    expect(code(MATRIX)).toMatch(/装成和本机同一套/)
  })

  it('引擎那条接口认 path', () => {
    const route = SERVER.slice(SERVER.indexOf('"/api/nodes/setup")'))
    expect(route.slice(0, 900), '/api/nodes/setup 不读 path 了？').toMatch(
      /query\(req, "path"\)/,
    )
  })
})
