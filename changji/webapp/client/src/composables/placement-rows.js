/**
 * 「权重放哪」那两行——出首帧和出片各一行。
 *
 * **单独一个文件，是为了能被测到**（同 room-decision.js、llm-state.js）。
 * 这两行是用户排查显存问题时读的第一处：估算多少、实测多少、差多少，
 * 一眼看得见。而「够就不清理」判的是实测那个数，不是估算那个。
 *
 * 几处容易写错、写错了就会误导人的地方：
 *
 *   没有 placement（老引擎）   整块不显示，返回空数组。不能拿默认值凑一行。
 *   某一路没给 weights         那一路不显示。接口里少了字段不等于"放内存"。
 *   没量过                     measured 给 null，让界面别显示这一段。
 *                              **不能写 0**：0 会被读成"量过、占 0 GB"。
 *   量了但没记画幅             measuredWorkMp 给 null。老持久化文件里的数
 *                              就是这样（work = 0），它罩不住任何指定了
 *                              大小的一镜，报个"0 MP·帧"只会让人困惑。
 */
export function placementRows(placement) {
  if (!placement) return []
  return [
    { key: 'image', label: '出首帧', ...(placement.image ?? {}) },
    { key: 'video', label: '出片', ...(placement.video ?? {}) },
  ]
    .filter((r) => r.weights !== undefined)
    .map((r) => ({
      ...r,
      measured: typeof r.measuredVramGb === 'number' ? r.measuredVramGb : null,
      // 像素 × 帧数，换算成「百万像素·帧」才是人能比的量级。
      measuredWorkMp:
        typeof r.measuredWork === 'number' && r.measuredWork > 0
          ? Math.round(r.measuredWork / 1e6)
          : null,
    }))
}
