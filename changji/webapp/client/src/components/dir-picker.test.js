/**
 * 挑模型目录那个浏览器。
 *
 * 两处是踩出来的，别人（和以后的我）会照样踩：
 *
 * 1. **遮罩要用全局那个 `.modal`。** base.css 里那段警告写得明明白白：
 *    scoped 样式里没写规则的类渲染出来是 `position: static`，整块落到文档
 *    最底下——**不报错，只是看不见**。第一版我写成 `.mask` 当场中招：弹窗
 *    在 DOM 里，屏幕上没有。
 * 2. **起步那个路径十有八九不存在。** 模型目录的默认值是 `<项目库>/models`，
 *    而那个目录要等第一次下模型才会被建出来。打开就报「这个目录不在」的话，
 *    这个浏览器等于没法用——第一次打开正是最需要它的时候。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fs.readFileSync(
  fileURLToPath(new URL('./DirPicker.vue', import.meta.url)),
  'utf8',
)
const BASE = fs.readFileSync(
  fileURLToPath(new URL('../styles/base.css', import.meta.url)),
  'utf8',
)
const SETTINGS = fs.readFileSync(
  fileURLToPath(new URL('../views/SettingsView.vue', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

const body = code(SRC)

describe('挑文件夹那个弹窗', () => {
  it('遮罩用的是全局那个类，而且那个类真的有规则', () => {
    expect(body, '又写成没规则的自定义类了').toMatch(/class="modal"/)
    expect(code(BASE), '.modal 的规则不见了').toMatch(/\.modal\s*\{[^}]*position:\s*fixed/)
  })

  it('打开时那一趟是 soft 的：进不去就退回起点，不弹红字', () => {
    expect(body).toMatch(/go\(props\.start \|\| '', true\)/)
    const at = body.indexOf('async function go(')
    const fn = body.slice(at, at + 400)
    expect(fn, '没有 soft 这条路').toMatch(/soft/)
    expect(fn, 'soft 时没退回起点').toMatch(/return go\(''/)
    expect(fn, 'soft 那一趟还在弹红字').toMatch(/quiet: soft/)
  })

  it('人自己点进去的不 soft——敲错了得看得见', () => {
    // 模板里的导航调用都不传第二个参数
    expect(body).toMatch(/@click="go\(parent\)"/)
    expect(body).toMatch(/@click="go\(r\.path\)"/)
    expect(body).toMatch(/@click="go\(e\.path\)"/)
  })

  it('没进到任何目录时「用这个」按不动', () => {
    const at = body.indexOf("emit('pick', here)")
    const btn = body.slice(body.lastIndexOf('<button', at), at)
    expect(btn).toMatch(/:disabled="!here"/)
  })

  it('挑完只填进输入框，不直接存', () => {
    // 这一节有自己的「保存」，上面还有个「写回配置文件」的勾管着存到哪儿。
    // 挑完就存的话那个勾被绕过去了。
    const s = code(SETTINGS)
    const at = s.indexOf('function pickDir(')
    expect(at).toBeGreaterThan(0)
    const fn = s.slice(at, at + 200)
    expect(fn).toContain('modelsDir.value = path')
    expect(fn, '挑完直接存了').not.toMatch(/startSetupDownload|saveModelsDir|run\(/)
  })
})
