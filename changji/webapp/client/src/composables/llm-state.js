/**
 * 大模型此刻装着没有——翻成界面上那一行。
 *
 * **单独一个文件，是为了能被测到**（同 room-decision.js）。这一行回答的
 * 正是需求里那两句话的状态：「默认加载 llm」和「点击出片清理掉大模型」。
 *
 * 三种情况，说错任何一种都是在撒谎：
 *
 *   装着    权重在显存里。量到过的话一并给出占了多少。
 *   没装     不在显存里。要用的时候自动装，不用手动做什么。
 *   不知道   **引擎没给这个字段**（老版本的二进制配新前端）。这时候绝不能
 *           说成"没装"——用户会据此判断显存去哪了，而我们其实没问到。
 *           宁可显示"不知道"。
 */
export function describeLlmState(node) {
  const n = node ?? {}
  const measured =
    typeof n.llmMeasuredVramGb === 'number'
      ? `${n.llmMeasuredVramGb.toFixed(1)} GB`
      : ''
  if (typeof n.llmLoaded !== 'boolean') {
    return { text: '不知道', measured, known: false }
  }
  return {
    text: n.llmLoaded ? '装着' : '没装（要用时自动装）',
    measured,
    known: true,
  }
}
