/* 首次打开时自动加载 Wan 2.2 工作流。
   ComfyUI 内置的默认模板是 Z-Image Turbo，它引用的模型本机没有，
   直接点 Run 会报 "Value not in list" 校验失败。这里把它换掉。
   用户之后自己打开过别的工作流就不再干预。 */
;(function () {
  var SEEDED = 'AIDuanju.DefaultWorkflowSeeded'
  var WF_PATH = 'workflows/Wan2.2_5B_图生视频.json'
  // 内置默认模板的特征节点，用来判断当前画布是不是那个模板
  var TEMPLATE_MARKERS = ['EmptySD3LatentImage', 'ModelSamplingAuraFlow']

  try {
    if (localStorage.getItem(SEEDED)) return
  } catch (e) {
    return // 隐私模式等场景读不到 storage，直接放弃，不影响正常使用
  }

  function whenReady(cb) {
    var t0 = Date.now()
    var iv = setInterval(function () {
      var app = window.comfyAPI && window.comfyAPI.app && window.comfyAPI.app.app
      if (app && app.graph && app.graph._nodes) {
        clearInterval(iv)
        cb(app)
      } else if (Date.now() - t0 > 90000) {
        clearInterval(iv)
      }
    }, 300)
  }

  whenReady(function (app) {
    var types = app.graph._nodes.map(function (n) { return n.type })
    var isDefaultTemplate = TEMPLATE_MARKERS.some(function (m) {
      return types.indexOf(m) !== -1
    })
    // 只在空画布或内置默认模板时替换，绝不覆盖用户自己的工作流
    if (app.graph._nodes.length !== 0 && !isDefaultTemplate) {
      try { localStorage.setItem(SEEDED, '1') } catch (e) {}
      return
    }

    fetch('./api/userdata/' + encodeURIComponent(WF_PATH))
      .then(function (r) { return r.ok ? r.json() : null })
      .then(function (g) {
        if (!g) throw new Error('工作流文件读取失败')
        return app.loadGraphData(g)
      })
      .then(function () {
        try { localStorage.setItem(SEEDED, '1') } catch (e) {}
        console.log('[AI短剧] 已加载默认工作流 Wan2.2_5B_图生视频')
      })
      .catch(function (e) {
        console.warn('[AI短剧] 默认工作流加载失败，可手动在 Workflows 里打开:', e)
      })
  })
})()
