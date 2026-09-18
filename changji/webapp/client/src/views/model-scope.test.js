/**
 * 项目页那一行「模型」是**这台机器**的设置，不是这部电影的——得说出来。
 *
 * 用户 2026-09-15 就是这么理解错的：「就是现在模型都在项目里设置，本地我只
 * 设置了不下载，远程如果用…」。怪不得他——那一行紧挨着「这部电影」和
 * 「片尾」，而那两行确确实实是这部电影的属性（写进项目里的 changji.toml）。只有
 * 「模型」这一行改的是全局配置。
 *
 * 项目配置模板自己把规矩写死了：
 *
 *   「机器的属性（模型文件、显存、端口、大模型地址）在全局配置里，
 *     不要写到这儿——否则项目目录拷到另一台机器就跑不起来」
 *
 * **这个误会在跨机那套里代价很实**：派到别的机器上的活，用的是那台自己的
 * 模型配置（`Task` 里一个模型路径都没有），和这儿挑的没关系。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./ProjectView.vue', import.meta.url)),
  'utf8',
)
const DLG = fs.readFileSync(
  fileURLToPath(new URL('../components/ModelDialog.vue', import.meta.url)),
  'utf8',
)
/** 引擎那一头：挑完写到哪个文件去。 */
const SETUP = fs.readFileSync(
  fileURLToPath(new URL('../../../../cpp/src/http/setup_api.cpp', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对——不然断言会被解释这个坑的那段注释本身骗过去。 */
function code(text) {
  return text
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, '')
}

describe('模型这一行归谁管', () => {
  /**
   * ⚠️ **这几条 2026-09-15 当天翻过一次面。**
   *
   * 最早那一版写的是「（本机）」——那时候挑哪一档确实只写全局，标签是对的。
   * 同一天用户定下「模型配置跟项目走」，挑的那一半搬进了项目的
   * changji.toml，标签跟着改成「（这部电影）」。
   *
   * 留着这段话是因为**两件事现在挤在同一行里**：挑哪一档跟项目走，那一档
   * 的文件在这台机器的哪儿跟机器走。谁要是只看见一半，很容易又把标签改回
   * 去，或者干脆把另一半也搬走。
   */
  it('项目页那一行标的是「这部电影」', () => {
    const body = code(VIEW)
    const at = body.indexOf('>模型')
    expect(at, '那一行的键名改了？').toBeGreaterThan(0)
    expect(body.slice(at, at + 80)).toMatch(/这部电影/)
  })

  it('那一行的悬停要把两半都说清', () => {
    const body = code(VIEW)
    const at = body.indexOf('>模型')
    const near = body.slice(Math.max(0, at - 500), at)
    expect(near, '没说挑哪一档跟项目走').toMatch(/这部电影的设置/)
    expect(near, '没说文件在哪儿跟机器走').toMatch(/这台机器/)
  })

  it('弹窗里也说一次，并且把两个文件都摆出来', () => {
    const body = code(DLG)
    expect(body, '没有 scopeHint').toMatch(/const scopeHint = computed/)
    expect(body, '没把全局那份的路径摆出来').toMatch(/configFile/)
    expect(body, '没把项目那份摆出来').toMatch(/changji\.toml/)
    // 没选项目时只写全局，那也要说
    expect(body).toMatch(/还没选项目/)
  })

  it('引擎那头两份都写：文件名进全局，挑的那一档进项目', () => {
    expect(SETUP, '不写全局那份了？').toMatch(/save_user_config/)
    expect(SETUP, 'configFile 报的不再是全局那份了？').toMatch(
      /user_config_path\(\)/,
    )
    expect(SETUP, '不写项目那份了？那标签就成了错的').toMatch(/"models\.pick"/)
  })
})

describe('统一内存那台机器上，整机多大也要说', () => {
  // `vramGb` 在苹果芯片上是 **Metal 肯给的那一份**（128 GB 的机器上是
  // 107.5），不是整机内存。这一页所有门槛都拿它比，所以它必须显示；
  // 但只显示它，用户看到的是「我买的明明是 128」——而这一页正是他决定
  // 要不要下 91 GB 那一档的地方。
  //
  // 引擎老早就把 `unifiedGb` 发过来了，setup_api.cpp 里还专门写了一段
  // 注释说界面为什么需要它，界面却一个字都没读——这条断言钉的是那件事。
  it('两个数都摆出来，措辞跟体检那边一致', () => {
    expect(DLG, '没读 unifiedGb').toMatch(/gpu\.unifiedGb/)
    expect(DLG, '措辞和 doctor.cpp 对不上').toMatch(/整机[^<]*其余留给系统/)
  })

  it('这行不能再钉死不换行', () => {
    // 加上那一句之后整行长了一倍，nowrap 会让它在窄窗口上直接顶出去。
    expect(DLG).not.toMatch(/class="tiny dim nowrap">\s*\n?\s*<template v-if="gpu"/)
  })
})
