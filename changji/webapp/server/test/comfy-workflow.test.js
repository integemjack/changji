// 工作流格式转换。
//
// ComfyUI 有两种工作流格式，这是新手最容易栽的地方。界面版把参数放在
// widgets_values 数组里，只有位置没有名字；接口版是有名字的键值对。
// 两者的映射关系不在文件里，得从服务端的 /object_info 拿。
//
// 这里每一条都对应一种会让工作流「照样能提交、照样能跑、出来的东西不对」
// 的错法——那是最难查的一类。全部和 Python 版逐用例比对过。

import { describe, expect, it } from 'vitest'

import {
  ApiWorkflow,
  WorkflowConverter,
  WorkflowError,
  isWidgetType,
} from '../src/engine/comfy/workflow.js'

const objectInfo = {
  KSampler: {
    input: {
      required: {
        model: ['MODEL'],
        seed: ['INT'],
        steps: ['INT'],
        cfg: ['FLOAT'],
        sampler_name: [['euler', 'dpmpp_2m']],
        scheduler: [['normal', 'karras']],
        denoise: ['FLOAT'],
      },
      optional: { extra: ['STRING'] },
    },
  },
  CLIPTextEncode: { input: { required: { text: ['STRING'], clip: ['CLIP'] } } },
  CheckpointLoaderSimple: { input: { required: { ckpt_name: [['a.safetensors']] } } },
  SaveImage: { input: { required: { images: ['IMAGE'], filename_prefix: ['STRING'] } } },
  ComboV3: { input: { required: { pick: ['COMFY_DYNAMICCOMBO_V3'], n: ['INT'] } } },
}

const convert = (ui) => new WorkflowConverter(objectInfo).convert(ui).toJSON()

describe('控件还是连线', () => {
  it('标量类型是控件', () => {
    for (const t of ['INT', 'FLOAT', 'STRING', 'BOOLEAN']) {
      expect(isWidgetType(t)).toBe(true)
    }
  })

  it('全大写的模型类型是连线', () => {
    // 判断必须用白名单：INT 和 FLOAT 也是全大写，但它们是控件不是连线
    for (const t of ['MODEL', 'CLIP', 'VAE', 'IMAGE', 'LATENT', 'CONDITIONING']) {
      expect(isWidgetType(t)).toBe(false)
    }
  })

  it('下拉框是控件', () => {
    expect(isWidgetType([['a', 'b']])).toBe(true)
    expect(isWidgetType('COMBO')).toBe(true)
    expect(isWidgetType('COMFY_DYNAMICCOMBO_V3')).toBe(true)
  })
})

describe('widgets_values 对回控件名', () => {
  it('seed 后面的伪控件要跳过', () => {
    // 界面上 seed 后面跟着一个 control_after_generate 下拉框，它占了
    // 数组一格但接口不认。不跳过的话 steps、cfg、sampler 全部错位一格，
    // 而错位之后工作流照样能跑，出来的东西只是不对
    const api = convert({
      nodes: [
        {
          id: 1,
          type: 'KSampler',
          widgets_values: [12345, 'randomize', 30, 5.0, 'euler', 'normal', 1.0],
          inputs: [{ name: 'model', link: null }],
        },
      ],
      links: [],
    })
    expect(api['1'].inputs).toMatchObject({
      seed: 12345,
      steps: 30,
      cfg: 5.0,
      sampler_name: 'euler',
      scheduler: 'normal',
      denoise: 1.0,
    })
  })

  it('没有伪控件时不能多跳', () => {
    const api = convert({
      nodes: [{ id: 1, type: 'KSampler', widgets_values: [12345, 30, 5.0, 'euler', 'normal', 1.0] }],
      links: [],
    })
    expect(api['1'].inputs.steps).toBe(30)
    expect(api['1'].inputs.denoise).toBe(1.0)
  })

  it('控件值比控件名少时按有的填', () => {
    const api = convert({
      nodes: [{ id: 1, type: 'KSampler', widgets_values: [1, 'fixed', 30] }],
      links: [],
    })
    expect(api['1'].inputs).toEqual({ seed: 1, steps: 30 })
  })

  it('COMBO 变体算控件', () => {
    const api = convert({ nodes: [{ id: 1, type: 'ComboV3', widgets_values: ['x', 7] }], links: [] })
    expect(api['1'].inputs).toEqual({ pick: 'x', n: 7 })
  })
})

describe('连线', () => {
  it('连线型输入写成 [源节点, 槽位]，且不占控件位', () => {
    const api = convert({
      nodes: [
        { id: 2, type: 'CLIPTextEncode', widgets_values: ['一句提示词'], inputs: [{ name: 'clip', link: 7 }] },
        { id: 3, type: 'CheckpointLoaderSimple', widgets_values: ['a.safetensors'] },
      ],
      links: [[7, 3, 1, 2, 0, 'CLIP']],
    })
    expect(api['2'].inputs.clip).toEqual(['3', 1])
    expect(api['2'].inputs.text).toBe('一句提示词')
  })
})

describe('节点过滤', () => {
  it('静音和旁路的节点不提交', () => {
    // 用户在界面上把某个节点静音是为了不跑它。照样提交的话，
    // 界面上看到的和实际跑的是两回事
    const api = convert({
      nodes: [
        { id: 1, type: 'SaveImage', widgets_values: ['out'], mode: 2 },
        { id: 2, type: 'SaveImage', widgets_values: ['out'], mode: 4 },
        { id: 3, type: 'SaveImage', widgets_values: ['keep'] },
      ],
      links: [],
    })
    expect(Object.keys(api)).toEqual(['3'])
  })
})

describe('报错', () => {
  it('接口版工作流传进来会说清楚', () => {
    expect(() => convert({ '1': { class_type: 'KSampler' } })).toThrow(/不是界面版/)
  })

  it('服务端不认识的节点类型指向缺插件', () => {
    expect(() =>
      convert({ nodes: [{ id: 1, type: '某个自定义节点', widgets_values: [1] }], links: [] }),
    ).toThrow(/缺少对应的自定义节点包/)
  })
})

describe('改参数', () => {
  const wf = () =>
    new ApiWorkflow({
      1: { class_type: 'KSampler', inputs: { seed: 1, steps: 20 } },
      2: { class_type: 'CLIPTextEncode', inputs: { text: '旧的' } },
      3: { class_type: 'CLIPTextEncode', inputs: { text: '负向' } },
    })

  it('按类型定位唯一节点', () => {
    const w = wf()
    w.setByClass('KSampler', { steps: 30, cfg: 7 })
    expect(w.getInput('1', 'steps')).toBe(30)
    expect(w.getInput('1', 'cfg')).toBe(7)
  })

  it('同类型有多个时拒绝乱改', () => {
    // 正负提示词都是 CLIPTextEncode。猜一个改的话，
    // 有一半概率把正向提示词写进负向那一格
    expect(() => wf().setByClass('CLIPTextEncode', { text: '新的' })).toThrow(/有 2 个/)
  })

  it('类型不存在时说清楚', () => {
    expect(() => wf().setByClass('NotThere', {})).toThrow(WorkflowError)
  })

  it('改不存在的节点会报错', () => {
    expect(() => wf().setInput('99', 'x', 1)).toThrow(/节点 99 不存在/)
  })

  it('copy 是深拷贝', () => {
    const a = wf()
    const b = a.copy()
    b.setInput('1', 'steps', 99)
    expect(a.getInput('1', 'steps')).toBe(20)
  })
})
