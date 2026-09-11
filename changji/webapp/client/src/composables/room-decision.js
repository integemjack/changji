/**
 * 把「上一次要不要腾显存」那个判断翻成人话。
 *
 * **单独一个文件，是为了能被测到。** 写在 SettingsView 的 computed 里
 * 就只能靠肉眼——而这三个分支说的是三件完全不同的事，说错一句用户就
 * 走错一步：
 *
 *   没判过  模型本来就装着、画幅也没超过量过的，压根没有"要不要腾"这个问题。
 *           **这一支绝不能借用"够，没动"那套措辞**：那条记录里「当时空闲」
 *           是 0、「问来的」是假，界面会显示成「问不到卡」——而那是让用户
 *           盯着报警的那一项（一直显示问不到就说明显存探测没走通）。
 *           凭空来个假警报比不显示更糟。
 *   够      真判过，而且判下来不用动别的模型。
 *   卸了    真判过，腾了地方。
 *
 * 另外「要多少」这个数本身是量出来的还是估出来的，也必须说：估算在出片
 * 这一路被实测推翻过两次，都是往小了错五倍，而判错的后果是 CUDA OOM
 * 把整个服务带走。
 */
export function describeRoomDecision(d) {
  if (!d) return null
  const gb = (v) => (typeof v === 'number' ? `${v.toFixed(1)} GB` : '不知道')

  if (d.alreadyLoaded) {
    return {
      slot: d.slot,
      skipped: true,
      ok: true,
      verdict: '模型本来就装着，没动别的',
    }
  }

  return {
    slot: d.slot,
    skipped: false,
    verdict: d.kept ? '够，没动别的模型' : `不够，卸了 ${d.evicted} 个`,
    ok: Boolean(d.kept),
    need: gb(d.liveGb),
    needHow: d.liveMeasured ? '量出来的' : '估的，这个槽还没量过',
    needTrusted: Boolean(d.liveMeasured),
    free: gb(d.freeSeenGb),
    // **这一位最要紧**：空闲是问显卡问来的，还是拿量到的数推算的。
    // 一直显示"推算"就说明问卡那条路没通，那本身就是个要查的问题。
    how: d.probed ? '问显卡问来的' : '问不到卡，按量到/估到的推算',
  }
}
