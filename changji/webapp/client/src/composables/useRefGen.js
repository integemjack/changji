/**
 * 设定页上跟出图有关的两件公共事：**用哪个种子**，和**画完了叫一声**。
 *
 * 用户 2026-09-11：「顶部加上一键出图和种子设置」。
 *
 * 放在模块级（不是每个组件一份）是因为这两件事天然是跨格子的：种子在
 * 页头填，用的是角色格和场景格里的每一次出图；而「一键出图」在页头跑，
 * 画完要让那两格自己去重新拉一遍。
 *
 * **种子是这一页上唯一能让人"再来一次、换个样子"的旋钮。** 引擎那边不给
 * 种子就按名字算一个固定值（见 ref_gen.cpp 的 ref_seed）——好处是同一个
 * 角色重画还是那张脸，坏处是**不满意的时候重画一百遍也是同一张**。
 * 填个数就换一张脸，而且填回原来那个数还能换回去。
 */
import { ref } from 'vue'

/** 空串 = 不给，引擎按名字算（同一个人永远同一张脸）。 */
const seed = ref('')

/** 画完了就加一。角色格和场景格 watch 它，变了就重新拉。 */
const stamp = ref(0)

export function useRefGen() {
  /**
   * 并进请求体里的种子。空的时候**一个键都不加**——加个空串或者 0 的话
   * 引擎会当成"就要这个种子"，而 0 是个合法种子。
   */
  function seedPayload() {
    const raw = String(seed.value).trim()
    if (!raw) return {}
    const n = Number(raw)
    if (!Number.isFinite(n)) return {}
    return { seed: Math.trunc(n) }
  }

  function rollSeed() {
    seed.value = String(Math.floor(Math.random() * 2147483648))
  }

  function clearSeed() {
    seed.value = ''
  }

  function touch() {
    stamp.value += 1
  }

  return { seed, stamp, seedPayload, rollSeed, clearSeed, touch }
}
