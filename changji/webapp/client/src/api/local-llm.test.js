/**
 * 「这个地址要不要 API Key」——前端那张表和引擎那张对不对得上。
 *
 * **为什么值得单写一条用例。** 判据在两处各写了一份：引擎
 * `LLMConfig::needs_api_key()`（config/settings.cpp）和 models store 里
 * 那条正则。少列一个地址的后果是**只有界面说谎**：那个地址上的本机服务
 * 被判成"云端"，模型窗里弹一句「这家要密钥」，让人去给自己的 Ollama
 * 申请一把 API Key——而引擎那边压根不要，体检也不会报。
 *
 * 原来前端只有五条，漏了 `[::1]` 和整段 `172.16.`～`172.19.` /
 * `172.30.` / `172.31.`。**`172.17.0.1` 正是 Docker 默认网桥的网关**，
 * 而体检里那条出路自己写着「用 Docker: docker compose up -d ollama」。
 *
 * 只比**两边列的是不是同一组**，不替引擎补全：172.2x 那几段引擎也没列
 * （它的注释写着"落到当云那一侧，代价只是多提示一句"），这儿照抄。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const CPP = fileURLToPath(new URL('../../../../cpp/src/config/settings.cpp', import.meta.url))
const STORE = fileURLToPath(new URL('../stores/models.js', import.meta.url))

/** 引擎 needs_api_key 里那张 kLocal 表。 */
function engineLocals() {
  const src = fs.readFileSync(CPP, 'utf8')
  const at = src.indexOf('bool LLMConfig::needs_api_key() const {')
  if (at < 0) throw new Error('settings.cpp 里找不到 needs_api_key')
  const body = src.slice(at, src.indexOf('\n}', at))
  const m = body.match(/kLocal\[\]\s*=\s*\{([\s\S]*?)\};/)
  if (!m) throw new Error('needs_api_key 里找不到 kLocal 那张表')
  return m[1].match(/"([^"]+)"/g).map((x) => x.slice(1, -1))
}

/** models store 里那条正则。 */
function frontendRe() {
  const src = fs.readFileSync(STORE, 'utf8')
  const m = src.match(/^\s*!(\/(?:\\.|\[[^\]]*\]|[^/\\])+\/)\.test\($/m)
  if (!m) throw new Error('models.js 里找不到判"要不要密钥"的那条正则')
  const lit = m[1]
  const at = lit.lastIndexOf('/')
  return new RegExp(lit.slice(1, at), lit.slice(at + 1))
}

describe('哪些地址算本机、不用填密钥', () => {
  it('引擎列的每一条，前端都认', () => {
    const re = frontendRe()
    const locals = engineLocals()
    // 取到的确实是十二条，不是空转
    expect(locals.length).toBe(12)
    for (const m of locals) {
      // kLocal 里存的就是 `//127.0.0.1` 这种片段，拼成一个地址来试
      expect(re.test(`http:${m}1:11434`), `引擎列了 ${m}，前端没认`).toBe(true)
    }
  })

  it('两边都不认的，前端也别自作主张认', () => {
    const re = frontendRe()
    const locals = engineLocals()
    // 172.2x 引擎故意没列（注释：落到"当云"那一侧，代价只是多提示一句）
    expect(locals.some((m) => m.includes('172.20'))).toBe(false)
    expect(re.test('http://172.20.0.1:11434')).toBe(false)
  })

  it('云端一律要密钥', () => {
    const re = frontendRe()
    for (const u of [
      'https://open.bigmodel.cn/api/paas/v4',
      'https://api.openai.com/v1',
      'https://api.deepseek.com',
    ]) {
      expect(re.test(u), u).toBe(false)
    }
  })

  it('Docker 网桥那个网关不要密钥——修的就是这一条', () => {
    // 体检里那条出路教人「用 Docker: docker compose up -d ollama」，
    // 出来的 Ollama 就在 172.17.0.1 上。
    expect(frontendRe().test('http://172.17.0.1:11434')).toBe(true)
  })

  it('IPv6 本机也不要密钥', () => {
    expect(frontendRe().test('http://[::1]:11434')).toBe(true)
  })

  it('空地址不算本机（还没配，按要密钥提示）', () => {
    expect(frontendRe().test('')).toBe(false)
  })
})
