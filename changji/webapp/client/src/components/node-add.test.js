/**
 * 远程机器要能在设置里直接加，不许只能手改配置文件。
 *
 * 用户 2026-09-15 提的。在这之前那张「机器 × 能力」的表上每台机器每个能力
 * 都能点，唯独**这张表从哪儿来**得去翻文档、找到配置文件、记住
 * `[[peer.nodes]]` 这个写法——而那份文档和那个文件名，界面上一个字都没说过
 * （表底下那句说明里才提了一次 `[[peer.nodes]]`，而那是在解释「为什么这一
 * 格点不动」）。
 *
 * 几条硬的：
 *
 *   · 写**全局**配置，不是项目库里那份 nodes.json——机器的属性，换个项目
 *     不该换一套机器。（`off` 那条正相反，它是项目的。）
 *   · 本机那一行没有「不用了」：它不是配置里加进来的一台。
 *   · 加失败时**不关那个框**——地址填错是最常见的失败，关掉的话人得从头
 *     再敲一遍，而他要改的可能只是一个字符。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const MATRIX = fs.readFileSync(
  fileURLToPath(new URL('./NodeMatrix.vue', import.meta.url)),
  'utf8',
)
const API = fs.readFileSync(
  fileURLToPath(new URL('../api/index.js', import.meta.url)),
  'utf8',
)
const SERVER = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/http/server.cpp', import.meta.url)),
  'utf8',
)
const WRITEBACK = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/config/writeback.cpp', import.meta.url)),
  'utf8',
)

function code(text) {
  return text.replace(/<!--[\s\S]*?-->/g, '').replace(/^\s*\/\/.*$/gm, '')
}
const ui = code(MATRIX)

describe('设置里能加远程机器', () => {
  it('引擎那两条接口都在', () => {
    expect(SERVER, '没有加机器那条').toMatch(/"\/api\/nodes\/add"/)
    expect(SERVER, '没有删机器那条').toMatch(/"\/api\/nodes\/remove"/)
  })

  it('写的是全局配置的 [[peer.nodes]]，不是项目库那份', () => {
    expect(SERVER).toMatch(/config::save_peer_nodes/)
    expect(WRITEBACK, '写回那一层不认数组表').toMatch(/\[\[peer\.nodes\]\]/)
  })

  it('写完要让这个进程跟着变，不然表是新的、派活用的是旧的', () => {
    const add = SERVER.slice(SERVER.indexOf('"/api/nodes/add"'))
    expect(add.slice(0, 3000)).toMatch(/runtime\(\)\.replace/)
    expect(add.slice(0, 3000)).toMatch(/node_registry\(\)\.refresh/)
  })

  it('前端两条都接上了', () => {
    expect(code(API)).toMatch(/addNode:\s*\(url,\s*token\)/)
    expect(code(API)).toMatch(/removeNode:\s*\(url\)/)
    expect(ui).toMatch(/api\.addNode\(/)
    expect(ui).toMatch(/api\.removeNode\(/)
  })

  it('本机那一行不给删', () => {
    // **钉的是那颗按钮上的守卫，不是 removeNode 的调用点。**
    // 2026-09-15 删改成了弹窗确认：行里那颗垃圾桶只负责把 confirming 指到
    // 这一台（`confirming = n.url`），真正的 removeNode 由弹窗里的「删除」
    // 调、拿的是 confirmNode——原来那句按 `removeNode(n)` 往前数 600 个
    // 字符找守卫，从此找不到那个串，indexOf 回 -1、slice 切到文件末尾的
    // CSS 上，报的是"删那颗没挡住本机"，而实际上挡得好好的。
    const at = ui.indexOf("confirming = n.url")
    expect(at, '行里那颗删按钮不见了').toBeGreaterThan(0)
    const btn = ui.slice(ui.lastIndexOf('<button', at), at)
    expect(btn, '删那颗没挡住本机').toMatch(/v-if="!n\.local"/)
  })

  it('删要先问一句，而且问的是弹窗', () => {
    // 行内确认会把「机器」那一列挤窄、整张表横着溢出，见 NodeMatrix 里
    // 那段注释。判据两条：有一层遮罩，且遮罩里那颗才叫 removeNode。
    expect(MATRIX, '没有确认弹窗').toMatch(/confirmNode[\s\S]{0,400}class="mask"/)
    expect(MATRIX, '垃圾桶那颗直接就删了').not.toMatch(/@click="removeNode\(n\)"/)
    expect(MATRIX, '弹窗里没有真正执行删除的那颗').toMatch(/removeNode\(confirmNode\)/)
  })

  it('加失败不关那个框', () => {
    const fn = ui.slice(ui.indexOf('async function addNode'))
    const body = fn.slice(0, fn.indexOf('async function removeNode'))
    const cat = body.indexOf('catch')
    expect(cat, 'addNode 没有 catch').toBeGreaterThan(0)
    expect(body.slice(cat), '失败时把框关掉了').not.toMatch(/adding\.value = false/)
  })

  it('那一行挤不下要能折——地址框本来就长', () => {
    expect(MATRIX.slice(MATRIX.indexOf('.add {'), MATRIX.indexOf('.add {') + 260))
      .toMatch(/flex-wrap:\s*wrap/)
  })
})
