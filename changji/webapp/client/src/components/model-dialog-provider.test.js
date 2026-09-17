/**
 * 换平台那一下，模型列表要问**新那一家**。
 *
 * ⚠️ `/api/llm/models` 不带参数时读的是**配置里存着的**那一家。换平台只改了
 * 窗里那个地址框、还没按保存，所以照旧问回来的是**上一家的模型**，一声不响
 * ——用户 2026-09-17：「大语言模型选择平台后无法立即刷新模型列表」。
 *
 * 这份用例读源码，不起组件：那个窗要一整套 store 和接口才挂得起来，而要钉的
 * 只有一句——`pickProvider` 里那一趟带没带新地址。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const src = fs.readFileSync(
  fileURLToPath(new URL('./ModelDialog.vue', import.meta.url)),
  'utf8',
)
const code = src
  .replace(/<!--[\s\S]*?-->/g, '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^\s*\/\/.*$/gm, '')

describe('换平台之后刷模型列表', () => {
  it('带着新地址去问，不是问配置里那一家', () => {
    const at = code.indexOf('async function pickProvider')
    expect(at, '找不到 pickProvider').toBeGreaterThan(-1)
    const fn = code.slice(at, code.indexOf('\n}', at))
    expect(fn, '没把新地址带过去').toMatch(/reloadModels\(\s*\{[^}]*base_url/)
  })

  it('密钥也带上，而且走请求体', () => {
    // 换了一家，旧密钥多半不认，不带必然 401。
    const at = code.indexOf('async function pickProvider')
    const fn = code.slice(at, code.indexOf('\n}', at))
    expect(fn).toMatch(/api_key/)
    // 查询串会落进访问日志和浏览器历史，密钥不许走那儿。
    expect(code).not.toMatch(/llm\/models\?[^'"]*api_key/)
  })
})
