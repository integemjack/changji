// 写剧本。
//
// 产出不是自由文本，是结构化的场次表，由我们自己渲染成「名字：台词」。
// 让模型直接写剧本格式的话，它一会儿用冒号一会儿用括号，一会儿把旁白
// 也写成对白，后面识别角色就开始出错。形式我们定，模型只管内容。
//
// 和 Python 版做过 51 个用例的逐一比对。

import { describe, expect, it, vi } from 'vitest'

import { LLMConfigSchema } from '../src/engine/config.js'
import { StyleLine } from '../src/engine/models/character.js'
import {
  ScriptError,
  budgetChars,
  buildPremisePrompt,
  buildPrompt,
  buildTrailerPrompt,
  draftDialogueChars,
  draftSpeakers,
  generatePremises,
  generateScript,
  normalizeSpeaker,
  parseDraft,
  parsePremises,
  renderDraft,
  stripLeadingTimecode,
  stripWrapper,
} from '../src/engine/stages/script.js'

describe('削包裹', () => {
  it('削掉套住整段的括号和引号', () => {
    expect(stripWrapper('（他犹豫了一下）')).toBe('他犹豫了一下')
    expect(stripWrapper('「我不走了。」')).toBe('我不走了。')
    expect(stripWrapper('((双层))')).toBe('双层')
  })

  it('中间的括号是内容的一部分，不动', () => {
    // 光数左右个数不够：「（甲说）乙答（丙笑）」左右各两个、数目相等，
    // 但首尾那两个并不是一对，削掉就把中间的括号弄错位了
    expect(stripWrapper('（甲说）乙答（丙笑）')).toBe('（甲说）乙答（丙笑）')
  })

  it('没有包裹就原样返回', () => {
    expect(stripWrapper('没有包裹')).toBe('没有包裹')
    expect(stripWrapper('   带空格   ')).toBe('带空格')
  })

  it('空内容不削成空串', () => {
    expect(stripWrapper('（）')).toBe('（）')
  })
})

describe('削时间码', () => {
  it('开头像时间码的括号削掉', () => {
    // 模型爱在动作行开头挂时间码。这段文字会原样进到分镜提示词里，
    // 让画面模型去理解一个时间码
    expect(stripLeadingTimecode('[0-3秒] 画面特写：她的手')).toBe('画面特写：她的手')
    expect(stripLeadingTimecode('【5秒】她抬头')).toBe('她抬头')
    expect(stripLeadingTimecode('[3s] 开场')).toBe('开场')
    expect(stripLeadingTimecode('（0:12）转场')).toBe('转场')
  })

  it('正常的括号开头不动', () => {
    // 不加限制的话「（他犹豫了）他开口」也会被削
    expect(stripLeadingTimecode('（他犹豫了）他开口')).toBe('（他犹豫了）他开口')
    expect(stripLeadingTimecode('（他说）')).toBe('（他说）')
  })
})

describe('说话人归一', () => {
  it('各种「没人说话」的写法统一成空串', () => {
    // 原样当名字的话，成片里会出现一个叫 none 的角色，
    // 字幕上写着「none：寂静」
    for (const s of ['none', 'NONE', '旁白', '（无）', 'narrator', 'N/A', '—', '']) {
      expect(normalizeSpeaker(s)).toBe('')
    }
  })

  it('真名字保留', () => {
    expect(normalizeSpeaker('林晚')).toBe('林晚')
    expect(normalizeSpeaker('林 晚')).toBe('林 晚')
  })
})

describe('字数预算', () => {
  it('按语速和对白占比推', () => {
    // 不给预算的话，模型写出来的东西按时长算能拍十分钟
    expect(budgetChars(60)).toBe(171)
    expect(budgetChars(30)).toBe(85)
  })

  it('再短也给一个下限', () => {
    expect(budgetChars(0.5)).toBe(20)
  })
})

describe('解析', () => {
  const draft = () =>
    parseDraft({
      title: 't',
      logline: 'l',
      beats: [
        { kind: 'dialogue', speaker: 'none', text: '我走了' },
        { kind: 'action', speaker: '', text: '[0-3秒] 她转身' },
        { kind: 'dialogue', speaker: '林晚', text: '「别走」' },
        { kind: 'action', speaker: '', text: '  ' },
      ],
    })

  it('说了话却没说是谁说的，当旁白而不是丢掉', () => {
    // 丢掉的话这句台词就从成片里消失了，那比配错声音还糟
    expect(draft().beats[0]).toEqual({ kind: 'action', speaker: '', text: '我走了' })
  })

  it('顺手削掉时间码和引号', () => {
    expect(draft().beats[1].text).toBe('她转身')
    expect(draft().beats[2].text).toBe('别走')
  })

  it('空内容的拍子丢掉', () => {
    expect(draft().beats.length).toBe(3)
  })

  it('渲染成「名字：台词」', () => {
    expect(renderDraft(draft())).toBe('我走了\n她转身\n林晚：别走')
  })

  it('出场人物按出现顺序去重', () => {
    expect(draftSpeakers(draft())).toEqual(['林晚'])
  })

  it('台词字数只算对白', () => {
    expect(draftDialogueChars(draft())).toBe(2)
  })

  it('一句台词都没有就是默片，要报错', () => {
    expect(() =>
      parseDraft({ beats: [{ kind: 'action', speaker: '', text: '空镜' }] }),
    ).toThrow(/默片/)
  })

  it('没有 beats 报错', () => {
    expect(() => parseDraft({ title: 't' })).toThrow(ScriptError)
    expect(() => parseDraft(null)).toThrow(/没有返回对象/)
  })
})

describe('选题解析', () => {
  it('取出三个方案', () => {
    const ideas = parsePremises({
      ideas: [
        { title: '「雪夜」', premise: '（保安发现监控多一个人）', hook: '钩子' },
        { title: 'b', premise: 'p2', hook: 'h2' },
        { title: 'c', premise: '', hook: 'h3' },
      ],
    })
    expect(ideas.length).toBe(2) // 空 premise 的丢掉
    expect(ideas[0].title).toBe('雪夜') // 引号削掉
    expect(ideas[0].premise).toBe('保安发现监控多一个人')
  })

  it('全空要报错而不是给空数组', () => {
    expect(() => parsePremises({ ideas: [] })).toThrow(ScriptError)
  })
})

describe('提示词', () => {
  it('写一集带上字数预算和已有角色', () => {
    const p = buildPrompt('梗概', 60, StyleLine.REALISTIC, {
      characters: ['林晚', '老王'],
      previous: '前情提要',
    })
    expect(p).toContain('171 个字左右')
    expect(p).toContain('林晚、老王')
    expect(p).toContain('前情提要')
  })

  it('选题提示词明确拒绝题材标签', () => {
    // 模型很爱回「都市 / 复仇 / 逆袭」，那种东西写不成剧本
    const p = buildPremisePrompt('职场', StyleLine.REALISTIC, 3)
    expect(p).toContain('题材标签不是选题')
    expect(p).toContain('职场')
  })

  it('选题提示词会避开已经想过的方向', () => {
    const p = buildPremisePrompt('', StyleLine.REALISTIC, 3, ['已有的方向'])
    expect(p).toContain('已有的方向')
  })

  it('预告片提示词要求不剧透', () => {
    // 预告片不是把正片缩短，是另一种东西
    const p = buildTrailerPrompt('梗概', 20, StyleLine.REALISTIC, { episodes: '正片内容' })
    expect(p).toContain('结局绝对不能剧透')
    expect(p).toContain('蒙太奇')
    expect(p).toContain('正片内容')
  })

  it('动漫线换一套说法', () => {
    expect(buildPrompt('x', 60, StyleLine.ANIME)).toContain('动漫短剧')
  })
})

describe('生成', () => {
  const llm = LLMConfigSchema.parse({ base_url: 'http://x/v1', model: 'm' })
  const reply = (obj) => ({
    status: 200,
    json: async () => ({ choices: [{ message: { content: JSON.stringify(obj) } }] }),
    text: async () => '',
  })

  it('用 schema 约束解码', async () => {
    const fetchImpl = vi.fn(async () =>
      reply({ title: 't', logline: 'l', beats: [{ kind: 'dialogue', speaker: '甲', text: '话' }] }),
    )
    const draft = await generateScript(llm, '梗概', { fetchImpl })
    expect(draft.title).toBe('t')
    const body = JSON.parse(fetchImpl.mock.calls[0][1].body)
    expect(body.response_format.json_schema.schema.required).toContain('beats')
  })

  it('空梗概直接拦下，不去打模型', async () => {
    const fetchImpl = vi.fn()
    await expect(generateScript(llm, '   ', { fetchImpl })).rejects.toThrow(ScriptError)
    expect(fetchImpl).not.toHaveBeenCalled()
  })

  it('选题数量限制在三到五个之间', async () => {
    const fetchImpl = vi.fn(async () =>
      reply({ ideas: [{ title: 'a', premise: 'p', hook: 'h' }] }),
    )
    await generatePremises(llm, { count: 99, fetchImpl })
    expect(fetchImpl.mock.calls[0][1].body).toContain('给出 5 个')
  })
})
