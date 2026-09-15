/**
 * 项目页那一行「模型」是**这台机器**的设置，不是这部剧的——得说出来。
 *
 * 用户 2026-09-15 就是这么理解错的：「就是现在模型都在项目里设置，本地我只
 * 设置了不下载，远程如果用…」。怪不得他——那一行紧挨着「这部片子」和
 * 「片尾」，而那两行确确实实是剧的属性（写进项目里的 changji.toml）。只有
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
  it('项目页那一行上写着「本机」', () => {
    const body = code(VIEW)
    const at = body.indexOf('>模型')
    expect(at, '那一行的键名改了？').toBeGreaterThan(0)
    expect(body.slice(at, at + 80)).toMatch(/本机/)
  })

  it('那一行的悬停说清是全局、而且跨机时不算数', () => {
    const body = code(VIEW)
    const at = body.indexOf('>模型')
    const near = body.slice(Math.max(0, at - 400), at)
    expect(near, '没说对所有项目生效').toMatch(/所有项目/)
    expect(near, '没说别的机器用它自己那份').toMatch(/别的机器/)
  })

  it('弹窗里也说一次，并且把配置文件路径摆出来', () => {
    const body = code(DLG)
    expect(body).toMatch(/本机设置/)
    expect(body, '没有 scopeHint').toMatch(/const scopeHint = computed/)
    expect(body, '没把 configFile 摆出来').toMatch(/configFile/)
  })

  it('引擎那头确实写的是全局配置——标签才站得住', () => {
    // 哪天它改成写项目里的 changji.toml，上面那两处就成了错的。
    expect(SETUP, 'post_setup_download 不再写用户配置了？').toMatch(
      /save_user_config/,
    )
    expect(SETUP, 'configFile 报的不再是全局那份了？').toMatch(
      /user_config_path\(\)/,
    )
  })
})
