/**
 * 牌子上的「准备 / 步 / 解码」三种文案。
 *
 * sd.cpp 拿同一个回调报三种阶段，采样完了之后的 VAE 分块解码以前也
 * 写成「准备 12/78」——用户看着以为模型又在重载（2026-09-15 报的）。
 * 引擎现在给 shot_phase，三种要各写各的；解码和准备一样不画百分比条
 * （分母和采样不是一个量级，画了会倒退）。
 *
 * 浏览器全局的桩同 useShots.test.js。
 */
import { createPinia, setActivePinia } from 'pinia'
import { describe, expect, it, vi } from 'vitest'

vi.mock('@/api', () => ({
  api: {
    run: () => Promise.resolve({}),
    shots: () => Promise.resolve({ shots: [] }),
    runStatus: () => Promise.resolve({ running: true, events: [] }),
    seriesStatus: () => Promise.resolve({ running: false }),
    stopRun: () => Promise.resolve({}),
    runPending: () => Promise.resolve({ shots: [] }),
  },
  mediaUrl: () => '',
}))
vi.stubGlobal('WebSocket', class { constructor() { throw new Error('这几条不测连接') } })
const mem = new Map()
vi.stubGlobal('localStorage', {
  getItem: (k) => (mem.has(k) ? mem.get(k) : null),
  setItem: (k, v) => mem.set(k, String(v)),
  removeItem: (k) => mem.delete(k),
})
vi.stubGlobal('document', {
  addEventListener() {},
  removeEventListener() {},
  querySelector: () => null,
  documentElement: { setAttribute() {}, removeAttribute() {} },
})
vi.stubGlobal('window', {
  location: { protocol: 'http:', host: 'x' },
  addEventListener() {},
  removeEventListener() {},
  dispatchEvent() {},
})

const { useRun } = await import('@/stores/run')
const { useSession } = await import('@/stores/session')
const { useShots } = await import('./useShots')

function fresh() {
  setActivePinia(createPinia())
  const session = useSession()
  session.selectProject('/p-phase')
  session.selectEpisode('ep01')
  return { runStore: useRun(), wall: useShots() }
}

const msg = (extra) => ({
  type: 'progress', job_id: 'run-1', stage: 'frames', shot_id: 's1',
  step: 1, total: 3, message: '出首帧', ...extra,
})

describe('牌子上的阶段文案', () => {
  it('采样写「N/M 步」并画百分比', async () => {
    const { runStore, wall } = fresh()
    await runStore.poll()
    runStore.applyMessage(msg({ shot_step: 3, shot_steps: 8, shot_phase: 'sample' }))
    expect(wall.shotState({ shot_id: 's1' })).toBe('首帧 3/8 步')
    expect(wall.pct('s1')).toBe(38)
  })

  it('准备写「准备 N/M」，不画条', async () => {
    const { runStore, wall } = fresh()
    await runStore.poll()
    runStore.applyMessage(msg({ shot_step: 6, shot_steps: 28, shot_phase: 'prep' }))
    expect(wall.shotState({ shot_id: 's1' })).toBe('首帧·准备 6/28')
    expect(wall.pct('s1')).toBeNull()
  })

  it('解码写「解码 N/M」，不是「准备」，也不画条', async () => {
    const { runStore, wall } = fresh()
    await runStore.poll()
    runStore.applyMessage(msg({ shot_step: 12, shot_steps: 78, shot_phase: 'decode' }))
    expect(wall.shotState({ shot_id: 's1' })).toBe('首帧·解码 12/78')
    expect(wall.pct('s1')).toBeNull()
  })

  it('没步数只有 prep：正在准备模型', async () => {
    const { runStore, wall } = fresh()
    await runStore.poll()
    runStore.applyMessage(msg({ shot_phase: 'prep' }))
    expect(wall.shotState({ shot_id: 's1' })).toBe('首帧·正在准备模型')
  })
})
