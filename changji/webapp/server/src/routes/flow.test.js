/**
 * 引导流程的进度判定。
 *
 * flow.js 的文件头写着这套规则"只写在这一处"——写在前端的话，侧边栏、
 * 顶部进度条、下一步按钮三个地方各判一遍迟早对不上。
 *
 * **但"只写在一处"也意味着错了就一起错**：三个地方看起来还是"一致"的，
 * 反而更难看出是判定本身的问题。所以它值得有测试。
 */

import { describe, expect, it } from 'vitest'
import { assess, STEPS } from './flow.js'

/** 造一份最小的项目数据。 */
function project(over = {}) {
  return {
    project_id: 'p1',
    premise: '一句梗概',
    episodes: [{ episode_id: 'ep01', shots: 3, synopsis: '第一集' }],
    characters: [{ char_id: 'c1' }],
    locations: [{ location_id: 'loc1' }],
    ...over,
  }
}

const shot = (over = {}) => ({ shot_id: 's1', status: 'planned', location_id: 'loc1', ...over })

describe('项目和剧本', () => {
  it('没有 project_id 就是没选项目', () => {
    expect(assess({}, [], null, new Set()).done.project).toBe(false)
    expect(assess(project(), [], null, new Set()).done.project).toBe(true)
  })

  it('剧本要梗概加至少一集写出内容', () => {
    expect(assess(project({ premise: '  ' }), [], null, new Set()).done.script)
      .toBe(false)
    expect(assess(project({ episodes: [] }), [], null, new Set()).done.script)
      .toBe(false)
    expect(assess(project(), [], null, new Set()).done.script).toBe(true)
  })

  it('剧本按全剧算，不按当前这一集算', () => {
    // 文件头写明了这一条：只看当前这一集的话，**新建第五集时前四步的对勾
    // 会集体消失**——用户会以为自己把什么弄坏了。
    const p = project({
      episodes: [
        { episode_id: 'ep01', shots: 3, synopsis: '写好的' },
        { episode_id: 'ep05', shots: 0, synopsis: '' },  // 刚新建的空集
      ],
    })
    expect(assess(p, [], { episode_id: 'ep05' }, new Set()).done.script).toBe(true)
  })
})

describe('场景这一步', () => {
  it('还没出分镜时，库里有场景就算数', () => {
    expect(assess(project(), [], null, new Set()).done.scenes).toBe(true)
  })

  it('库是空的就不算', () => {
    expect(assess(project({ locations: [] }), [], null, new Set()).done.scenes)
      .toBe(false)
  })

  it('分镜引用了库里没有的场景就不算', () => {
    // 不挡住的话，跑到一半会报「场景未注册」——那时候已经花了算力。
    const r = assess(project(), [shot({ location_id: 'loc_不存在' })], null, new Set())
    expect(r.done.scenes).toBe(false)
    expect(r.counters.missingLocations).toEqual(['loc_不存在'])
  })

  it('老分镜把场景填在 scene_id 里的，算「没接上」', () => {
    // 那种镜头渲染时拿不到场景描述，界面上要能看出来。
    const r = assess(project(), [shot({ location_id: '', scene_id: 'loc1' })],
                     null, new Set())
    expect(r.counters.unlinkedShots).toBe(1)
    expect(r.done.scenes).toBe(false)   // 有没接上的就不算完成
  })
})

describe('镜头', () => {
  // 分镜和制作 2026-09-10 合成一步。**判据取的是原来「制作」那条**：
  // 这一格代表"这一集的镜头做完了"，光有分镜表不算——那时候一帧画面
  // 都还没有，而侧边栏打了勾用户就以为这一步过了。
  it('光有分镜表不算做完', () => {
    expect(assess(project(), [], null, new Set()).done.shots).toBe(false)
    expect(assess(project(), [shot()], null, new Set()).done.shots).toBe(false)
  })

  it('所有镜头都到终态才算做完', () => {
    const two = [shot({ shot_id: 'a', status: 'final_done' }), shot({ shot_id: 'b' })]
    expect(assess(project(), two, null, new Set()).done.shots).toBe(false)

    const bothDone = [
      shot({ shot_id: 'a', status: 'final_done' }),
      shot({ shot_id: 'b', status: 'locked' }),
    ]
    expect(assess(project(), bothDone, null, new Set()).done.shots).toBe(true)
  })

  it('fallback 也算终态', () => {
    // 闸门判它退回兜底方案，那也是"这一镜不会再动了"。
    const r = assess(project(), [shot({ status: 'fallback' })], null, new Set())
    expect(r.done.shots).toBe(true)
  })

  it('一个镜头都没有时不算做完', () => {
    // 0 === 0 会让"全都做完了"意外成立，要单独挡一下。
    expect(assess(project(), [], null, new Set()).done.shots).toBe(false)
  })

  it('老的 storyboard / production 两个 key 不再出现', () => {
    // 前端按 key 认人。留着旧 key 的话侧边栏会多出两格灰的，
    // 而它们永远不会打勾——没有任何东西再去写它们了。
    const d = assess(project(), [shot()], null, new Set()).done
    expect('storyboard' in d).toBe(false)
    expect('production' in d).toBe(false)
  })
})

describe('成片和上传', () => {
  it('成片由调用方按 outputs 回填，判定里先给 false', () => {
    // 这一条钉住的是那个契约：assess 自己判不了成片（它不读磁盘），
    // 调用方必须回填。哪天有人把回填那行删了，这里至少说明了它该在。
    expect(assess(project(), [shot()], null, new Set()).done.film).toBe(false)
  })

  it('这一集投过才算上传完', () => {
    const ep = { episode_id: 'ep01' }
    expect(assess(project(), [], ep, new Set()).done.publish).toBe(false)
    expect(assess(project(), [], ep, new Set(['ep01'])).done.publish).toBe(true)
    // 投的是别的集不算。
    expect(assess(project(), [], ep, new Set(['ep02'])).done.publish).toBe(false)
  })
})

describe('八步本身', () => {
  it('步骤表和判定的键要对得上', () => {
    // 少一个键的话，前端那一步永远是灰的，而没有任何报错。
    const r = assess(project(), [shot()], { episode_id: 'ep01' }, new Set())
    for (const s of STEPS) {
      expect(r.done).toHaveProperty(s.key)
    }
    expect(Object.keys(r.done).sort()).toEqual(STEPS.map((s) => s.key).sort())
  })
})
