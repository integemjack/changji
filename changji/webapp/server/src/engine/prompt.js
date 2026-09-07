/**
 * 提示词拼接。一致性真正生效的地方。
 *
 * 分层顺序是固定的，而且不能改：
 *
 *     身份层（角色外观，逐字节不变）
 *     场景层（空间与光线，每场固定）
 *     镜头层（景别、机位、动作，每镜可变）
 *     风格层（全剧统一）
 *
 * 顺序不能改是因为提示词里靠前的词权重更高。顺序一变，画面重心就跟着变，
 * 同一个角色在不同镜头里会显得不是同一个人。
 *
 * 身份层是从资产库读出来的同一个字符串，几十个镜头拿到的完全相同——
 * 这就是角色跨镜头一致的全部机制，不靠在提示词里叮嘱模型「保持一致」。
 */

import {
  StyleLine,
  refForPose,
  renderCharacterPrompt,
  renderLocationPrompt,
} from './models/character.js'

export class RenderError extends Error {}

// 景别的中文说法。写实线用自然语言，模型对中文景别词有反应。
const SHOT_SIZE_ZH = {
  ECU: '大特写', CU: '特写', MCU: '近景',
  MS: '中景', MLS: '中远景', LS: '远景', ELS: '大远景',
}
const ANGLE_ZH = {
  low: '仰拍', eye_level: '平视', high: '俯拍',
  overhead: '顶拍', dutch: '斜角构图',
}
const MOVE_ZH = {
  static: '固定镜头', pan_left: '向左横摇', pan_right: '向右横摇',
  tilt_up: '上摇', tilt_down: '下摇', push_in: '镜头缓慢推近',
  pull_out: '镜头缓慢拉远', handheld: '手持轻微晃动', orbit: '环绕运镜',
}

export class PromptComposer {
  constructor(assets) {
    this.assets = assets
    this.styleLine = assets.style.style_line
  }

  get sep() {
    return this.styleLine === StyleLine.ANIME ? ', ' : '，'
  }

  /** 把一个镜头拼成正负提示词和参考图列表。 */
  compose(shot) {
    const layers = []
    const refs = []

    // 身份层。逐字节从资产库拼出来，模型碰不到。
    for (const inShot of shot.characters) {
      const char = this.assets.characters[inShot.char_id]
      if (!char) {
        throw new RenderError(
          `镜头 ${shot.shot_id} 引用了未注册角色 ${inShot.char_id}`,
        )
      }
      const beats = [
        renderCharacterPrompt(char, this.styleLine, inShot.wardrobe_state),
      ]
      if (inShot.expression) beats.push(inShot.expression)
      if (inShot.action) beats.push(inShot.action)
      layers.push(beats.join(this.sep))

      const ref = refForPose(char, inShot.face_pose)
      if (ref) refs.push(ref)
    }

    // 场景层
    if (shot.location_id) {
      const loc = this.assets.locations[shot.location_id]
      if (!loc) {
        throw new RenderError(
          `镜头 ${shot.shot_id} 引用了未注册场景 ${shot.location_id}`,
        )
      }
      layers.push(renderLocationPrompt(loc, this.styleLine))
      if (loc.ref_empty) refs.push(loc.ref_empty)
    }

    // 镜头层
    const camera = [SHOT_SIZE_ZH[shot.shot_size] ?? '', ANGLE_ZH[shot.camera_angle] ?? '']
    layers.push(camera.filter(Boolean).join(this.sep))
    if (shot.first_frame_prompt) layers.push(shot.first_frame_prompt)

    // 风格层
    if (this.assets.style.global_style) layers.push(this.assets.style.global_style)

    const negative = [shot.negative_prompt, this.assets.style.negative_prompt]
      .filter(Boolean)
      .join(this.sep)

    return {
      positive: layers.filter((p) => p.trim()).join(this.sep),
      negative,
      reference_images: refs,
    }
  }

  /** 给视频模型的运动描述。它只管动作，不重复外观。 */
  motionPrompt(shot) {
    const parts = [MOVE_ZH[shot.camera_move] ?? '']
    if (shot.motion_prompt) parts.push(shot.motion_prompt)
    for (const inShot of shot.characters) {
      if (inShot.action) parts.push(inShot.action)
    }
    return parts.filter(Boolean).join(this.sep)
  }
}
