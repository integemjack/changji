/**
 * 工作流格式转换与参数注入。
 *
 * ComfyUI 有两种工作流格式，这是新手最容易栽的地方：
 *
 * 界面版（用户在浏览器里保存的那种）是 {"nodes": [...], "links": [...]}，
 * 节点的参数放在 widgets_values 数组里，只有位置没有名字。
 *
 * 接口版（POST /prompt 要的那种）是
 * {"节点id": {"class_type": ..., "inputs": {...}}}，参数是有名字的键值对。
 *
 * 两者之间的映射关系不在文件里，必须从服务端的 /object_info 拿。
 * 硬编码一张映射表在 ComfyUI 升级后就会错，所以这里从服务端动态取。
 */

import fs from 'node:fs'

/**
 * 控件型输入的标量类型。
 *
 * 除此之外的全大写类型名都是连线型输入（MODEL、CLIP、VAE、IMAGE、LATENT、
 * CONDITIONING、AUDIO、VIDEO 等）。判断必须用白名单：INT 和 FLOAT 也是
 * 全大写，但它们是控件不是连线。
 */
const SCALAR_WIDGET_TYPES = new Set(['INT', 'FLOAT', 'STRING', 'BOOLEAN'])

/** 界面上有、接口上没有的伪控件的取值。它们会让后面所有参数错位一格。 */
const PSEUDO_WIDGET_VALUES = new Set(['fixed', 'increment', 'decrement', 'randomize'])

export class WorkflowError extends Error {}

/** 判断一个输入是控件（占 widgets_values 一格）还是连线。 */
export function isWidgetType(typeDef) {
  if (Array.isArray(typeDef)) return true // 下拉框，选项直接列在这里
  if (typeof typeDef !== 'string') return false
  if (SCALAR_WIDGET_TYPES.has(typeDef)) return true
  // COMBO、COMFY_DYNAMICCOMBO_V3 之类的下拉框变体
  return typeDef.includes('COMBO')
}

/** 接口格式的工作流。可以按节点类型或标题定位并改参数。 */
export class ApiWorkflow {
  constructor(prompt) {
    this.prompt = prompt
  }

  copy() {
    return new ApiWorkflow(structuredClone(this.prompt))
  }

  toJSON() {
    return this.prompt
  }

  // ---- 定位 ----

  /** 按节点类型找出所有节点 id。 */
  findByClass(classType) {
    return Object.entries(this.prompt)
      .filter(([, node]) => node.class_type === classType)
      .map(([nid]) => nid)
  }

  /** 按节点类型找唯一节点。不唯一就报错，避免改错地方。 */
  oneByClass(classType) {
    const found = this.findByClass(classType)
    if (!found.length) throw new WorkflowError(`工作流里没有 ${classType} 节点`)
    if (found.length > 1) {
      throw new WorkflowError(
        `工作流里有 ${found.length} 个 ${classType} 节点，无法确定改哪个。` +
          `请用节点 id 指定：${found.join('、')}`,
      )
    }
    return found[0]
  }

  // ---- 改参数 ----

  setInput(nodeId, key, value) {
    if (!(nodeId in this.prompt)) throw new WorkflowError(`节点 ${nodeId} 不存在`)
    this.prompt[nodeId].inputs ??= {}
    this.prompt[nodeId].inputs[key] = value
  }

  /** 定位唯一节点并批量改参数。返回节点 id。 */
  setByClass(classType, values) {
    const nid = this.oneByClass(classType)
    for (const [key, value] of Object.entries(values)) this.setInput(nid, key, value)
    return nid
  }

  getInput(nodeId, key) {
    return this.prompt[nodeId]?.inputs?.[key]
  }
}

/**
 * 把界面版工作流转成接口版。
 *
 * 需要服务端的 /object_info 才能知道每个节点的控件顺序和名字。
 */
export class WorkflowConverter {
  constructor(objectInfo) {
    this.objectInfo = objectInfo
    this._widgetNames = new Map()
  }

  /**
   * 取一个节点类型的控件名，按界面上的顺序。
   *
   * /object_info 里 input.required 的键顺序和界面上控件的顺序一致。
   * 连线型输入（MODEL、CLIP 这些）不占 widgets_values 的位置，要排除。
   */
  widgetNames(classType) {
    if (this._widgetNames.has(classType)) return this._widgetNames.get(classType)

    const info = this.objectInfo[classType]
    if (info === undefined) {
      throw new WorkflowError(
        `服务端不认识节点类型 ${classType}。通常是缺少对应的自定义节点包`,
      )
    }

    const names = []
    for (const section of ['required', 'optional']) {
      for (const [name, spec] of Object.entries(info.input?.[section] ?? {})) {
        if (!Array.isArray(spec) || !spec.length) continue
        if (isWidgetType(spec[0])) names.push(name)
      }
    }
    this._widgetNames.set(classType, names)
    return names
  }

  /** 界面版转接口版。 */
  convert(uiWorkflow) {
    const nodes = uiWorkflow.nodes
    if (nodes === undefined) {
      throw new WorkflowError(
        '这不是界面版工作流。如果已经是接口版，直接用 ApiWorkflow 包一层',
      )
    }

    // 连线表：link_id -> [源节点 id, 源输出槽]
    const linkSrc = new Map()
    for (const link of uiWorkflow.links ?? []) {
      if (Array.isArray(link) && link.length >= 5) linkSrc.set(link[0], [link[1], link[2]])
    }

    const prompt = {}
    for (const node of nodes) {
      // 界面上被静音（2）或旁路（4）的节点不提交
      if (node.mode === 2 || node.mode === 4) continue
      const nid = String(node.id)
      const classType = node.type
      const inputs = {}

      // 连线型输入
      for (const slot of node.inputs ?? []) {
        const linkId = slot.link
        if (linkId !== null && linkId !== undefined && linkSrc.has(linkId)) {
          const [srcNode, srcSlot] = linkSrc.get(linkId)
          inputs[slot.name] = [String(srcNode), srcSlot]
        }
      }

      // 控件型输入
      const values = [...(node.widgets_values ?? [])]
      if (values.length) {
        Object.assign(inputs, this._zipWidgets(this.widgetNames(classType), values, inputs))
      }

      prompt[nid] = { class_type: classType, inputs }
    }

    return new ApiWorkflow(prompt)
  }

  /**
   * 把 widgets_values 数组对回控件名。
   *
   * 难点是界面会在数组里插入伪控件。最典型的是 KSampler：界面上 seed
   * 后面跟着一个 control_after_generate 下拉框，它占了数组的一个位置，
   * 但接口不认这个参数。不跳过它，后面 steps、cfg、sampler 全部错位一格
   * ——而错位之后工作流照样能提交、照样能跑，出来的东西只是不对，
   * 这是最难查的一类。
   */
  _zipWidgets(names, values, alreadyLinked) {
    const out = {}
    let vi = 0
    for (const name of names) {
      // 已经由连线提供的输入不再从控件取
      if (name in alreadyLinked) continue
      if (vi >= values.length) break
      out[name] = values[vi]
      vi += 1
      // 取完 seed 之后如果还有多余的值，那多半是伪控件，跳过
      if ((name === 'seed' || name === 'noise_seed') && vi < values.length) {
        if (typeof values[vi] === 'string' && PSEUDO_WIDGET_VALUES.has(values[vi])) vi += 1
      }
    }
    return out
  }
}

/** 读界面版工作流文件。 */
export function loadUiWorkflow(file) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    throw new WorkflowError(`工作流文件不存在：${file}`)
  }
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    throw new WorkflowError(`工作流文件不是合法 JSON：${file}\n${err.message}`)
  }
}
