/**
 * 顶栏那份系统表：**一条通道，而且断了有退路**。
 *
 * 两件事各自都是"不报错、只是永远空白"的那种：
 *
 *   一、SysMeter 和 JobBadge 原来各开一条 `system` socket。同一个频道订两遍
 *       ——引擎两秒一次那份表要发两遍，而两处的连接管理是逐字重复的两份，
 *       连"讣告串台、重连越积越多"那个坑都分别踩了一遍修了一遍。
 *   二、两块都是**纯 socket**。代理把 Upgrade 掐了就是永远空白：顶栏三个
 *       小表整个不出现，「AI 作业中」也一次都不出现——而后者是从设定页
 *       点完「批量补分镜」之后**唯一**看得见的出口（那一页自己的提示就写着
 *       去那儿看进度）。
 *
 * 退路能成立，全靠引擎那头 `/api/system` 回的**和推过去的那份一模一样**
 * （server.cpp 里那句注释，而且是同一个函数拼的）。这一条尤其要盯住：
 * 哪天 REST 那边少拼一个 `jobs`，界面上就是"没有任何活在跑"，一个字不说。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const SRC = fileURLToPath(new URL('..', import.meta.url))
const read = (rel) => fs.readFileSync(path.join(SRC, rel), 'utf8')

function allSources() {
  const out = []
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name)
      if (e.isDirectory()) walk(p)
      else if ((e.name.endsWith('.vue') || e.name.endsWith('.js')) && !e.name.endsWith('.test.js')) {
        out.push({ rel: path.relative(SRC, p), src: fs.readFileSync(p, 'utf8') })
      }
    }
  }
  walk(SRC)
  return out
}

describe('system 这条频道', () => {
  it('只有 useSystemFeed 一处订它', () => {
    const subs = allSources().filter((f) => /openJobSocket\(\s*\n?\s*'system'/.test(f.src))
    expect(subs.map((f) => f.rel)).toEqual(['composables/useSystemFeed.js'])
  })

  it('两块顶栏都从那一处读，不自己开', () => {
    for (const rel of ['components/SysMeter.vue', 'components/JobBadge.vue']) {
      const src = read(rel)
      expect(src, `${rel} 还在自己开 socket`).not.toContain('openJobSocket')
      expect(src).toContain('useSystemFeed')
    }
  })

  it('socket 没有的时候要退回拉 /api/system', () => {
    const feed = read('composables/useSystemFeed.js')
    expect(feed).toContain('api.system()')
    // 推上来了就别再拉：不为同一个节奏做两遍功
    expect(feed).toMatch(/stopPulling\(\)/)
  })
})

describe('引擎那两头回的是同一份', () => {
  it('/api/system 也带 jobs，和推送同一个来源', () => {
    const src = fs.readFileSync(
      fileURLToPath(new URL('../../../../cpp/src/http/server.cpp', import.meta.url)),
      'utf8',
    )
    // 推的那一下
    const pump = src.indexOf('ws::hub().broadcast("system", msg)')
    expect(pump).toBeGreaterThan(0)
    expect(src.slice(pump - 600, pump)).toContain('pipeline::running_work()')
    // REST 那一条
    const rest = src.indexOf('CROW_ROUTE(app, "/api/system")')
    expect(rest).toBeGreaterThan(0)
    expect(src.slice(rest, rest + 600)).toContain('pipeline::running_work()')
  })
})
