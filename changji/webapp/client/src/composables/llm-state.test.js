import { describe, it, expect } from 'vitest'
import { describeLlmState } from './llm-state'

describe('大模型装着没有', () => {
  it('装着：连量到多少一起说', () => {
    const r = describeLlmState({ llmLoaded: true, llmMeasuredVramGb: 15.42 })
    expect(r.text).toBe('装着')
    expect(r.measured).toBe('15.4 GB')
    expect(r.known).toBe(true)
  })

  it('没装：说清不用手动做什么', () => {
    const r = describeLlmState({ llmLoaded: false, llmMeasuredVramGb: null })
    expect(r.text).toBe('没装（要用时自动装）')
    expect(r.measured).toBe('')
  })

  it('引擎没给这个字段：只能说"不知道"，绝不能说成"没装"', () => {
    // 老版本二进制配新前端。说成"没装"的话，用户会据此判断显存去哪了
    // ——而我们其实没问到。
    expect(describeLlmState({}).text).toBe('不知道')
    expect(describeLlmState({}).known).toBe(false)
    expect(describeLlmState(undefined).text).toBe('不知道')
    expect(describeLlmState(null).text).toBe('不知道')
    // 字段在但不是布尔（接口变形）也一样不猜
    expect(describeLlmState({ llmLoaded: 'true' }).text).toBe('不知道')
    expect(describeLlmState({ llmLoaded: 1 }).text).toBe('不知道')
  })

  it('没量过就不显示占用，不写 0 GB', () => {
    // 写 0 GB 会被当成"量过、占 0"，那是另一回事。
    expect(describeLlmState({ llmLoaded: true }).measured).toBe('')
    expect(
      describeLlmState({ llmLoaded: true, llmMeasuredVramGb: '15' }).measured,
    ).toBe('')
  })
})
