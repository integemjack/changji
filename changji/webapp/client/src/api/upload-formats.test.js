/**
 * 「参考音色收哪几种」这一条，全仓库有**四份拷贝**。
 *
 * 真身只有一个：进程内那条配音走 mtmd → miniaudio，只认 wav / mp3 / flac
 * （`infer/llama_tts.cpp` 里读不了时说的就是这句）。而另外三处各写了一遍：
 *
 *   · `http/upload.cpp` 的 `voice_types()`——收上来的按 Content-Type 认；
 *   · 同一个函数下面那句 400 的话（「只收 …，收到的是 …」）；
 *   · 角色那一页 `<input type="file">` 的 accept——决定文件选择器里哪些
 *     文件是亮的。
 *
 * **多一个的代价是"收下了，几个钟头之后才炸"。** 2026-09-15 之前这三处
 * 都多收一个 m4a：传上去回一句「参考音色已存」、角色也配上了，而第一句
 * 台词开配才报「参考音色读不了（只认 wav / mp3 / flac）」——那时候人已经
 * 在跑整章了。少一个的代价小一些（明明能用的格式选不了），但一样是错的。
 *
 * 这条用例把四份拉到一起比。做法同 `speech-rate.test.js`：直接读源码，
 * 那边改了这儿当场红。
 */
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

import { describe, expect, it } from 'vitest'

const read = (rel) => fs.readFileSync(fileURLToPath(new URL(rel, import.meta.url)), 'utf8')

/** 进程内那条路真能读的几种。真身。 */
function runtimeFormats() {
  const src = read('../../../../cpp/src/infer/llama_tts.cpp')
  // why = "参考音色读不了：" + … + "（只认 wav / mp3 / flac）"
  const m = src.match(/只认\s*([a-z0-9 /]+?)\s*）/)
  if (!m) throw new Error('llama_tts.cpp 里找不到「只认 …」那句')
  return m[1].split('/').map((s) => s.trim()).filter(Boolean).sort()
}

const UPLOAD = '../../../../cpp/src/http/upload.cpp'

/** 上传那头收的 Content-Type → 落盘用的扩展名。 */
function uploadTypes() {
  const src = read(UPLOAD)
  const block = src.match(/voice_types\(\)\s*\{[\s\S]*?\{\{([\s\S]*?)\}\}/)
  if (!block) throw new Error('upload.cpp 里找不到 voice_types 那张表')
  const pairs = [...block[1].matchAll(/\{"([^"]+)",\s*"\.([a-z0-9]+)"\}/g)]
  if (!pairs.length) throw new Error('voice_types 那张表是空的？')
  return pairs.map(([, mime, ext]) => ({ mime, ext }))
}

/**
 * 那句 400 里报出去的几种。
 *
 * **只在 `post_character_voice` 里面找。** 同一个文件里参考图那处也有一句
 * 一模一样形状的「只收 png、jpg、webp，收到的是 …」，而且排在前面——不圈
 * 范围的话这条用例量的是图片那张表，音色这边改错了它还是绿的。
 */
function uploadMessage() {
  const src = read(UPLOAD)
  const at = src.indexOf('post_character_voice')
  if (at < 0) throw new Error('upload.cpp 里找不到 post_character_voice')
  const m = src.slice(at).match(/"只收 ([^"]+?)，收到的是/)
  if (!m) throw new Error('post_character_voice 里找不到那句「只收 …」')
  return m[1].split('、').map((s) => s.trim()).filter(Boolean).sort()
}

/** 文件选择器里亮着的几种。 */
function pickerTypes() {
  const src = read('../views/assets/AssetCharacters.vue')
  const m = src.match(/accept="((?:audio\/[^",]+,?)+)"/)
  if (!m) throw new Error('AssetCharacters 里找不到那个音频 accept')
  return m[1].split(',').map((s) => s.trim()).filter(Boolean).sort()
}

describe('参考音色收哪几种', () => {
  it('上传收的，进程内那条路都得读得了', () => {
    // 多一个就是"收下了、几个钟头之后才炸"
    const runtime = new Set(runtimeFormats())
    for (const { mime, ext } of uploadTypes()) {
      expect(runtime.has(ext), `upload.cpp 收 ${mime}（.${ext}），而进程内那条路读不了它`).toBe(true)
    }
  })

  it('进程内读得了的，上传也都收（别把能用的挡在外面）', () => {
    const got = new Set(uploadTypes().map((t) => t.ext))
    expect([...got].sort()).toEqual(runtimeFormats())
  })

  it('那句 400 报的和真收的是同一批', () => {
    expect(uploadMessage()).toEqual([...new Set(uploadTypes().map((t) => t.ext))].sort())
  })

  it('文件选择器亮的和接口收的是同一批', () => {
    // 选择器宽了：挑得出来、传上去 400；窄了：能用的格式在选择器里是灰的，
    // 而人多半不会去改那个「所有文件」
    expect(pickerTypes()).toEqual([...new Set(uploadTypes().map((t) => t.mime))].sort())
  })
})

/**
 * 参考图那一边，同一把尺子。
 *
 * 这边没法像音色那样从源码里读出"真身"——`sd_image.cpp` 的 `load_image()`
 * 走 `stbi_load_from_memory`，而 stb_image 支持哪几种写在它自己的头文件
 * 注释里，不在我们仓库。所以这条直接把答案钉死：**png 和 jpg，没有 webp**。
 *
 * stb_image 那份格式清单是 JPEG / PNG / TGA / BMP / PSD / GIF / HDR / PIC /
 * PNM——整份文件里 "webp" 出现 0 次。上一版收 webp 的依据是 ComfyUI 的
 * LoadImage（PIL，读得了），而 comfy 那一档 2026-09-10 就拆了。
 *
 * 要是哪天真给参考图接了别的解码器，这条会红——那时候连着把上面那段依据
 * 一起改，而不是把它删掉了事。
 */
function refTypes() {
  const src = read(UPLOAD)
  const block = src.match(/ref_types\(\)\s*\{[\s\S]*?\{\{([\s\S]*?)\}\}/)
  if (!block) throw new Error('upload.cpp 里找不到 ref_types 那张表')
  const pairs = [...block[1].matchAll(/\{"([^"]+)",\s*"\.([a-z0-9]+)"\}/g)]
  if (!pairs.length) throw new Error('ref_types 那张表是空的？')
  return pairs.map(([, mime, ext]) => ({ mime, ext }))
}

/** 清同名旧文件时要扫的那张表。 */
function staleExts(name) {
  const src = read(UPLOAD)
  const block = src.match(new RegExp(`${name}\\(\\)\\s*\\{[\\s\\S]*?\\{([\\s\\S]*?)\\}`))
  if (!block) throw new Error(`upload.cpp 里找不到 ${name}`)
  return [...block[1].matchAll(/"\.([a-z0-9]+)"/g)].map((m) => m[1]).sort()
}

/** 某个 .vue 里那个图片 accept。 */
function imagePicker(rel) {
  const m = read(rel).match(/accept="((?:image\/[^",]+,?)+)"/)
  if (!m) throw new Error(`${rel} 里找不到那个图片 accept`)
  return m[1].split(',').map((s) => s.trim()).filter(Boolean).sort()
}

describe('参考图收哪几种', () => {
  it('只收 png 和 jpg——出图那头拿 stb_image 读，它不认 webp', () => {
    expect([...new Set(refTypes().map((t) => t.ext))].sort()).toEqual(['jpg', 'png'])
  })

  it('那句 400 报的和真收的是同一批', () => {
    const src = read(UPLOAD)
    const at = src.indexOf('check_upload')
    const m = src.slice(at).match(/"只收 ([^"]+?)，收到的是/)
    if (!m) throw new Error('check_upload 里找不到那句「只收 …」')
    expect(m[1].split('、').map((s) => s.trim()).sort()).toEqual(
      [...new Set(refTypes().map((t) => t.ext))].sort(),
    )
  })

  it('两页的文件选择器亮的都和接口收的是同一批', () => {
    const want = [...new Set(refTypes().map((t) => t.mime))].sort()
    expect(imagePicker('../views/assets/AssetCharacters.vue')).toEqual(want)
    expect(imagePicker('../views/assets/AssetLocations.vue')).toEqual(want)
  })
})

/**
 * **收件表和清理表是两件事。**
 *
 * 收件表说的是"往后还收不收"，清理表说的是"盘上可能躺着什么"。收窄收件表
 * 的时候顺手把清理表也削了的话，同一个槽位后来传了张新的，旧那份就永远留
 * 在目录里——`claim_ref_path` 上面那段要防的正是这个：「留一张永远用不上的，
 * 而且用户看不到」。
 *
 * 所以清理表只能比收件表大，不能小。
 */
describe('清理旧文件那两张表', () => {
  it('参考图：扫的扩展名盖得住收的', () => {
    const stale = new Set(staleExts('ref_stale_exts'))
    for (const { ext } of refTypes()) expect(stale.has(ext)).toBe(true)
    // 收过 webp 的项目盘上还躺着，得接着扫
    expect(stale.has('webp')).toBe(true)
  })

  it('参考音色：扫的扩展名盖得住收的', () => {
    const stale = new Set(staleExts('voice_stale_exts'))
    for (const { ext } of uploadTypes()) expect(stale.has(ext)).toBe(true)
    // 同上：收过 m4a 的项目盘上还躺着
    expect(stale.has('m4a')).toBe(true)
  })
})
