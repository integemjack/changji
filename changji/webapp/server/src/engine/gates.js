/**
 * 质量闸门。
 *
 * 无人值守模式下，闸门是唯一阻止废片流入成片的机制。
 *
 * 三条纪律：
 * 判定必须程序可算，不能依赖人看。
 * 失败必须给出可操作的下一步，而不只是说不合格。
 * 绝不静默放行，重试超限就明确降级并留下记录。
 *
 * 这个文件只放判定逻辑。真正去读像素和时长的是 FFmpeg 封装，
 * 判定拿到的是数字——这样闸门本身完全可测，不需要真的有视频文件。
 */

export const Verdict = {
  PASS: 'pass',
  RETRY: 'retry', // 换种子重跑可能就好了
  REGRESS: 'regress', // 重跑也没用，得退回上一阶段
  FALLBACK: 'fallback', // 重试超限，降级处理
}

export const gateResult = (shotId, verdict, gate, reasons = [], metrics = {}) => ({
  shot_id: shotId,
  verdict,
  gate,
  reasons,
  metrics,
  ok: verdict === Verdict.PASS,
})

export function describeGate(result) {
  if (result.ok) return `${result.shot_id} 通过${result.gate}`
  return `${result.shot_id} 未过${result.gate}：${result.reasons.join('；')}`
}

/**
 * 近乎纯色。生成失败最常见的表现。
 *
 * 阈值取 8。实测正常画面的展布在 40 以上，纯色或近乎纯色的画面在个位数。
 *
 * 展布用第 90 和第 10 百分位之差，而不是极差。极差会被单个亮点或暗点
 * 带偏：一张几乎全黑但有一个高光点的废图，极差能到 250，看起来很正常。
 */
export const looksBlank = (stats) => stats.spread < 8

/** 整体过曝或全黑。 */
export const looksClipped = (stats) => stats.mean < 6 || stats.mean > 249

function positionName(index, total) {
  if (total <= 1) return '画面'
  if (index === 0) return '片头'
  if (index === total - 1) return '片尾'
  return '片中'
}

/**
 * 检查一个镜头的视频。草稿档和成片档用同一套检查，只是期望值不同。
 *
 * 传进来的是已经探好的信息和取样统计，不是文件路径——判定和取数分开，
 * 闸门这一层就不需要真的有视频文件才能测。
 */
export function gateVideo(shot, { info, samples, config, expectedDurationS = null, expectedSize = null, gateName = '画面闸门' }) {
  const reasons = []
  const metrics = {}

  if (!info) return gateResult(shot.shot_id, Verdict.RETRY, gateName, ['视频文件读不出来'])
  if (!info.has_video) {
    return gateResult(shot.shot_id, Verdict.RETRY, gateName, ['文件里没有视频轨'])
  }

  metrics.duration_s = info.duration_s
  metrics.width = info.width
  metrics.height = info.height

  // 时长。差太多说明帧数算错了，重跑也是一样，得退回上一阶段。
  if (expectedDurationS) {
    const drift = Math.abs(info.duration_s - expectedDurationS)
    metrics.duration_drift_s = round3(drift)
    if (drift > Math.max(0.5, expectedDurationS * 0.2)) {
      return gateResult(
        shot.shot_id,
        Verdict.REGRESS,
        gateName,
        [
          `时长 ${info.duration_s.toFixed(2)} 秒，期望 ${expectedDurationS.toFixed(2)} 秒，` +
            `差 ${drift.toFixed(2)} 秒，多半是帧数算错了`,
        ],
        metrics,
      )
    }
  }

  // 分辨率。不符说明档位参数没生效。
  if (expectedSize && (info.width !== expectedSize[0] || info.height !== expectedSize[1])) {
    return gateResult(
      shot.shot_id,
      Verdict.REGRESS,
      gateName,
      [
        `分辨率 ${info.width}x${info.height}，期望 ${expectedSize[0]}x${expectedSize[1]}，` +
          `档位参数没生效`,
      ],
      metrics,
    )
  }

  // 画面内容。多点取样，只看一帧会漏掉中途崩坏。
  if (!samples || !samples.length) {
    return gateResult(shot.shot_id, Verdict.RETRY, gateName, ['取不到任何画面'], metrics)
  }

  const spreads = samples.map((s) => s.spread)
  const means = samples.map((s) => s.mean)
  metrics.spread_min = round2(Math.min(...spreads))
  metrics.mean_avg = round2(means.reduce((a, b) => a + b, 0) / means.length)

  const blank = samples.map((s, i) => (looksBlank(s) ? i : -1)).filter((i) => i >= 0)
  if (blank.length) {
    const where = blank.map((i) => positionName(i, samples.length)).join('、')
    reasons.push(`${where}的画面近乎纯色，展布只有 ${Math.min(...spreads).toFixed(1)}`)
  }

  const clipped = samples.map((s, i) => (looksClipped(s) ? i : -1)).filter((i) => i >= 0)
  if (clipped.length) {
    const where = clipped.map((i) => positionName(i, samples.length)).join('、')
    reasons.push(`${where}的画面整体过暗或过曝`)
  }

  if (Math.min(...spreads) < config.min_pixel_std && !blank.length) {
    reasons.push(
      `画面细节偏少，展布 ${Math.min(...spreads).toFixed(1)} 低于阈值 ` +
        `${config.min_pixel_std.toFixed(0)}`,
    )
  }

  // 相邻取样点之间画面差异过大，说明中途崩坏
  if (means.length >= 2) {
    const swing = Math.max(...means) - Math.min(...means)
    metrics.mean_swing = round2(swing)
    if (swing > 60) {
      reasons.push(`片中亮度剧烈跳变 ${swing.toFixed(0)}，可能中途崩坏`)
    }
  }

  if (reasons.length) return gateResult(shot.shot_id, Verdict.RETRY, gateName, reasons, metrics)
  return gateResult(shot.shot_id, Verdict.PASS, gateName, [], metrics)
}

/**
 * 检查镜头时长能不能装下配音。
 *
 * 这个检查放在装配之前。装完再发现装不下就得重做整集。
 */
export function gateAudioSync(shot, { info, config, speechS }) {
  if (speechS === null || speechS === undefined) {
    return gateResult(shot.shot_id, Verdict.REGRESS, '音画闸门', [
      '有台词但没有配音时长，配音阶段没跑完',
    ])
  }
  if (speechS === 0) return gateResult(shot.shot_id, Verdict.PASS, '音画闸门')
  if (!info) {
    return gateResult(shot.shot_id, Verdict.RETRY, '音画闸门', ['读不出视频时长'])
  }

  const slack = info.duration_s - speechS
  const metrics = {
    speech_s: round3(speechS),
    video_s: round3(info.duration_s),
    slack_s: round3(slack),
  }

  if (slack < -config.max_audio_drift_s) {
    return gateResult(
      shot.shot_id,
      Verdict.REGRESS,
      '音画闸门',
      [
        `配音 ${speechS.toFixed(2)} 秒装不进 ${info.duration_s.toFixed(2)} 秒的镜头，` +
          `超出 ${(-slack).toFixed(2)} 秒。需要重新锁定时长后重出这一镜`,
      ],
      metrics,
    )
  }
  return gateResult(shot.shot_id, Verdict.PASS, '音画闸门', [], metrics)
}

/**
 * 闸门失败后决定怎么办。
 *
 * 这是无人值守能不能不卡死的关键。重试超限时降级而不是停下来，
 * 保证整集能出片，同时把问题记录下来供人事后查。
 */
export function decideNext(result, shot, config) {
  if (result.ok) return Verdict.PASS
  if (result.verdict === Verdict.REGRESS) return Verdict.REGRESS
  if (shot.attempts + 1 >= config.max_attempts_per_shot) {
    return config.fallback_on_exhausted ? Verdict.FALLBACK : Verdict.REGRESS
  }
  return Verdict.RETRY
}

/** 闸门结果概览。无人值守时这是人唯一要看的东西。 */
export function summarizeGates(results) {
  if (!results.length) return '没有需要检查的镜头'
  const passed = results.filter((r) => r.ok)
  const lines = [`闸门检查 ${results.length} 个镜头，通过 ${passed.length} 个`]
  for (const r of results) {
    if (!r.ok) lines.push(`  ${describeGate(r)}`)
  }
  return lines.join('\n')
}

const round2 = (x) => Math.round(x * 100) / 100
const round3 = (x) => Math.round(x * 1000) / 1000
