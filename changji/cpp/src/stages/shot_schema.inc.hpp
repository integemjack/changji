// 本文件由 cpp/tools/gen_prompts.py 生成，不要手改。
// pydantic 从 Shot 的字段定义导出的 JSON Schema，原样嵌进来。
// llm_shot_schema() 在这上面做删字段和收紧枚举的加工。
// 改了 Shot 的定义就要重跑这个脚本。

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kShotSchemaJson =
    R"CJ({
 "$defs": {
  "CameraAngle": {
   "enum": [
    "low",
    "eye_level",
    "high",
    "overhead",
    "dutch"
   ],
   "title": "CameraAngle",
   "type": "string"
  },
  "CameraMove": {
   "enum": [
    "static",
    "pan_left",
    "pan_right",
    "tilt_up",
    "tilt_down",
    "push_in",
    "pull_out",
    "handheld",
    "orbit"
   ],
   "title": "CameraMove",
   "type": "string"
  },
  "CharacterInShot": {
   "additionalProperties": false,
   "description": "角色在本镜头中的表现。\n\n只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。",
   "properties": {
    "char_id": {
     "description": "角色 id，必须是已注册角色之一",
     "title": "Char Id",
     "type": "string"
    },
    "expression": {
     "default": "",
     "description": "表情，如 愕然、隐忍",
     "maxLength": 40,
     "title": "Expression",
     "type": "string"
    },
    "action": {
     "default": "",
     "description": "本镜动作，如 后退半步",
     "maxLength": 80,
     "title": "Action",
     "type": "string"
    },
    "wardrobe_state": {
     "default": "default",
     "description": "服装状态 id，如 suit_torn",
     "maxLength": 40,
     "title": "Wardrobe State",
     "type": "string"
    },
    "face_pose": {
     "$ref": "#/$defs/FacePose",
     "default": "front",
     "description": "面部朝向"
    },
    "screen_pos": {
     "default": "center",
     "description": "画面位置：left / center / right",
     "title": "Screen Pos",
     "type": "string"
    }
   },
   "required": [
    "char_id"
   ],
   "title": "CharacterInShot",
   "type": "object"
  },
  "DialogueLine": {
   "additionalProperties": false,
   "description": "一句台词。时长字段由配音阶段回填，分镜阶段不填。",
   "properties": {
    "char_id": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "说话角色。为空表示旁白",
     "title": "Char Id"
    },
    "text": {
     "maxLength": 200,
     "minLength": 1,
     "title": "Text",
     "type": "string"
    },
    "emotion": {
     "default": "neutral",
     "description": "情绪标签",
     "title": "Emotion",
     "type": "string"
    },
    "emotion_intensity": {
     "default": 0.5,
     "maximum": 1.0,
     "minimum": 0.0,
     "title": "Emotion Intensity",
     "type": "number"
    },
    "voice_id": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "音色 id，由角色资产决定",
     "title": "Voice Id"
    },
    "audio_path": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "相对项目根的路径",
     "title": "Audio Path"
    },
    "actual_duration_s": {
     "anyOf": [
      {
       "minimum": 0,
       "type": "number"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "title": "Actual Duration S"
    }
   },
   "required": [
    "text"
   ],
   "title": "DialogueLine",
   "type": "object"
  },
  "FacePose": {
   "description": "角色面部朝向。口型判定用。",
   "enum": [
    "front",
    "three_quarter",
    "profile",
    "back",
    "off_screen"
   ],
   "title": "FacePose",
   "type": "string"
  },
  "ShotSize": {
   "description": "景别。取值顺序由近到远。",
   "enum": [
    "ECU",
    "CU",
    "MCU",
    "MS",
    "MLS",
    "LS",
    "ELS"
   ],
   "title": "ShotSize",
   "type": "string"
  },
  "ShotStatus": {
   "description": "镜头在流水线上的位置。断点续跑靠它。",
   "enum": [
    "planned",
    "audio_done",
    "frame_done",
    "draft_done",
    "draft_rejected",
    "final_done",
    "final_rejected",
    "fallback",
    "locked"
   ],
   "title": "ShotStatus",
   "type": "string"
  },
  "Transition": {
   "enum": [
    "cut",
    "dissolve",
    "fade_in",
    "fade_out",
    "whip"
   ],
   "title": "Transition",
   "type": "string"
  }
 },
 "additionalProperties": false,
 "description": "一个镜头。",
 "properties": {
  "shot_id": {
   "description": "全局唯一，如 ep01_s03_sh007",
   "pattern": "^[a-z0-9_]+$",
   "title": "Shot Id",
   "type": "string"
  },
  "scene_id": {
   "pattern": "^[a-z0-9_]+$",
   "title": "Scene Id",
   "type": "string"
  },
  "order": {
   "description": "集内顺序",
   "minimum": 0,
   "title": "Order",
   "type": "integer"
  },
  "visual_desc": {
   "default": "",
   "description": "给人看的中文描述",
   "maxLength": 300,
   "title": "Visual Desc",
   "type": "string"
  },
  "first_frame_prompt": {
   "default": "",
   "maxLength": 1200,
   "title": "First Frame Prompt",
   "type": "string"
  },
  "last_frame_prompt": {
   "anyOf": [
    {
     "maxLength": 1200,
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "description": "为空则走单帧图生视频",
   "title": "Last Frame Prompt"
  },
  "motion_prompt": {
   "default": "",
   "description": "运动描述，给视频模型",
   "maxLength": 400,
   "title": "Motion Prompt",
   "type": "string"
  },
  "negative_prompt": {
   "default": "",
   "title": "Negative Prompt",
   "type": "string"
  },
  "shot_size": {
   "$ref": "#/$defs/ShotSize",
   "default": "MS"
  },
  "camera_angle": {
   "$ref": "#/$defs/CameraAngle",
   "default": "eye_level"
  },
  "camera_move": {
   "$ref": "#/$defs/CameraMove",
   "default": "static"
  },
  "camera_id": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "description": "复用机位 id。同场景同机位保证不越轴",
   "title": "Camera Id"
  },
  "characters": {
   "items": {
    "$ref": "#/$defs/CharacterInShot"
   },
   "title": "Characters",
   "type": "array"
  },
  "location_id": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Location Id"
  },
  "prop_ids": {
   "items": {
    "type": "string"
   },
   "title": "Prop Ids",
   "type": "array"
  },
  "duration_s": {
   "default": 5.0,
   "description": "镜头时长",
   "exclusiveMinimum": 0,
   "maximum": 30,
   "title": "Duration S",
   "type": "number"
  },
  "duration_locked": {
   "default": false,
   "description": "真表示已由配音时长反推锁定，不可再调",
   "title": "Duration Locked",
   "type": "boolean"
  },
  "dialogue": {
   "items": {
    "$ref": "#/$defs/DialogueLine"
   },
   "title": "Dialogue",
   "type": "array"
  },
  "sfx": {
   "items": {
    "type": "string"
   },
   "title": "Sfx",
   "type": "array"
  },
  "bgm_cue": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Bgm Cue"
  },
  "needs_lipsync": {
   "default": false,
   "description": "由 derive_needs_lipsync 规则推导，不要让模型填",
   "title": "Needs Lipsync",
   "type": "boolean"
  },
  "transition_in": {
   "$ref": "#/$defs/Transition",
   "default": "cut"
  },
  "transition_dur_s": {
   "default": 0.0,
   "maximum": 2.0,
   "minimum": 0,
   "title": "Transition Dur S",
   "type": "number"
  },
  "subtitle_text": {
   "default": "",
   "title": "Subtitle Text",
   "type": "string"
  },
  "beat": {
   "default": "",
   "description": "叙事功能，如 反转、铺垫",
   "maxLength": 20,
   "title": "Beat",
   "type": "string"
  },
  "continuity_notes": {
   "default": "",
   "maxLength": 200,
   "title": "Continuity Notes",
   "type": "string"
  },
  "missing_info": {
   "description": "模型自报的信息缺口，供校验阶段检查",
   "items": {
    "type": "string"
   },
   "title": "Missing Info",
   "type": "array"
  },
  "status": {
   "$ref": "#/$defs/ShotStatus",
   "default": "planned"
  },
  "attempts": {
   "default": 0,
   "minimum": 0,
   "title": "Attempts",
   "type": "integer"
  },
  "frame_path": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Frame Path"
  },
  "video_path": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Video Path"
  },
  "gate_notes": {
   "items": {
    "type": "string"
   },
   "title": "Gate Notes",
   "type": "array"
  }
 },
 "required": [
  "shot_id",
  "scene_id",
  "order"
 ],
 "title": "Shot",
 "type": "object"
})CJ";

// 分镜阶段允许大模型填的字段。外观类字段不在其中，这是刻意的。
inline constexpr const char* kLlmShotFields[] = {
    "shot_id",
    "scene_id",
    "order",
    "visual_desc",
    "first_frame_prompt",
    "motion_prompt",
    "shot_size",
    "camera_angle",
    "camera_move",
    "camera_id",
    "characters",
    "location_id",
    "duration_s",
    "dialogue",
    "transition_in",
    "transition_dur_s",
    "subtitle_text",
    "beat",
    "continuity_notes",
    "missing_info",
};

}  // namespace changji::stages::prompt
