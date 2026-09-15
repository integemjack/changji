/**
 * 音色那一栏：外部配音服务和进程内配音要的**不是同一种东西**。
 *
 * 引擎按 `[tts].backend` 算好了发过来（`/api/voices` 的 `kind`），而且在
 * voices.cpp 里明写着「前端不该靠 backend 猜」：
 *
 *   clip  从项目 voices/ 下那几段里挑，也可以传一段新的
 *   name  手填一个**那个外部服务认的**音色名
 *
 * 前端一直没读这个字段，两种情况画成同一套。**后果不是少个样式**：走外
 * 部服务时点「传一段人声」，片段落在项目的 voices/ 下、voice_id 变成
 * `voices/xxx.wav`，而对面根本不认这个名字——配音直接失败。而同一屏上
 * 此刻正印着引擎给的那句「外部配音服务的音色由那个服务自己管，这里问
 * 不到」。摆着一颗按钮，让人去做那句话刚说过做不成的事。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const VIEW = fs.readFileSync(
  fileURLToPath(new URL('./AssetCharacters.vue', import.meta.url)),
  'utf8',
)
const ENGINE = fs.readFileSync(
  fileURLToPath(new URL('../../../../../cpp/src/http/voices.cpp', import.meta.url)),
  'utf8',
)

/** 去掉注释再比对，免得被解释这个坑的注释本身骗过去。 */
function code(text) {
  return text.replace(/<!--[\s\S]*?-->/g, '').replace(/^\s*\/\/.*$/gm, '')
}

describe('音色那一栏按引擎说的 kind 画', () => {
  it('引擎确实在发这个字段', () => {
    expect(ENGINE, '不发 kind 了？').toMatch(/"kind",\s*"name"/)
    expect(ENGINE).toMatch(/"kind",\s*"clip"/)
  })

  it('前端读它，而不是自己靠 backend 猜', () => {
    expect(code(VIEW), '没读 /api/voices 的 kind').toMatch(
      /data\.kind === 'name'/,
    )
  })

  it('老引擎不发这个字段时按 clip 走（就是以前的行为）', () => {
    expect(code(VIEW)).toMatch(/voiceKind\s*=\s*ref\('clip'\)/)
  })

  it('换项目要跟着清掉', () => {
    // 上一部走外部服务、这一部走进程内的话，不清的话这一部的抽屉里
    // 「传一段人声」是灰的，而它本该能用。
    expect(code(VIEW)).toMatch(/voiceKind\.value = 'clip'/)
  })

  it('走外部服务时，传片段那颗灰掉、摇音色那块不摆', () => {
    expect(code(VIEW), '传一段人声没跟着灰').toMatch(
      /'is-off':\s*byName\s*\|\|/,
    )
    expect(code(VIEW), '摇音色那块还摆着').toMatch(
      /<details v-if="!byName"/,
    )
  })
})
