// 本文件由 cpp/tools/gen_prompts.py 生成，不要手改。
// 字段的中文标签，从 web/server.py 的 _FIELD_NAMES 原样导出。
// 报「已应用 min_frame_similarity」不如报「与首帧相似度下限」。
// 另外是配置段映射：哪个字段该写进配置文件的哪一节。

#pragma once

namespace changji::stages::prompt {

// 字段名 -> 中文标签。
inline constexpr const char* kFieldNames[][2] = {
    {R"CJ(base_url)CJ", R"CJ(地址)CJ"},
    {R"CJ(model)CJ", R"CJ(模型名)CJ"},
    {R"CJ(api_key)CJ", R"CJ(api key)CJ"},
    {R"CJ(temperature)CJ", R"CJ(温度)CJ"},
    {R"CJ(job_timeout_s)CJ", R"CJ(单镜超时)CJ"},
    {R"CJ(max_retries)CJ", R"CJ(重试次数)CJ"},
    {R"CJ(backend)CJ", R"CJ(后端)CJ"},
    {R"CJ(fps)CJ", R"CJ(帧率)CJ"},
    {R"CJ(crf)CJ", R"CJ(画质 crf)CJ"},
    {R"CJ(subtitle_font)CJ", R"CJ(字幕字体)CJ"},
    {R"CJ(subtitle_max_chars_per_line)CJ", R"CJ(字幕单行字数)CJ"},
    {R"CJ(subtitle_max_lines)CJ", R"CJ(字幕行数)CJ"},
    {R"CJ(scene_transition_s)CJ", R"CJ(场景转场)CJ"},
    {R"CJ(max_attempts_per_shot)CJ", R"CJ(单镜最多重试)CJ"},
    {R"CJ(min_pixel_std)CJ", R"CJ(画面展布下限)CJ"},
    {R"CJ(min_frame_similarity)CJ", R"CJ(与首帧相似度下限)CJ"},
    {R"CJ(max_audio_drift_s)CJ", R"CJ(台词落点最大偏差)CJ"},
    {R"CJ(target_lufs)CJ", R"CJ(响度目标)CJ"},
    {R"CJ(tolerance_s)CJ", R"CJ(时长容差)CJ"},
    {R"CJ(max_tempo_shift)CJ", R"CJ(变速上限)CJ"},
    {R"CJ(tts_tolerance_s)CJ", R"CJ(配音时长容差)CJ"},
    {R"CJ(tts_max_tempo_shift)CJ", R"CJ(配音变速上限)CJ"},
    {R"CJ(gates_enabled)CJ", R"CJ(质量闸门)CJ"},
    {R"CJ(fallback_on_exhausted)CJ", R"CJ(重试超限降级)CJ"},
    {R"CJ(draft_width)CJ", R"CJ(草稿档宽)CJ"},
    {R"CJ(draft_height)CJ", R"CJ(草稿档高)CJ"},
    {R"CJ(draft_steps)CJ", R"CJ(草稿档步数)CJ"},
    {R"CJ(final_width)CJ", R"CJ(成片档宽)CJ"},
    {R"CJ(final_height)CJ", R"CJ(成片档高)CJ"},
    {R"CJ(final_steps)CJ", R"CJ(成片档步数)CJ"},
    {R"CJ(llm_base_url)CJ", R"CJ(大模型地址)CJ"},
    {R"CJ(llm_model)CJ", R"CJ(模型名)CJ"},
    {R"CJ(llm_api_key)CJ", R"CJ(api key)CJ"},
    {R"CJ(llm_temperature)CJ", R"CJ(温度)CJ"},
    {R"CJ(tts_backend)CJ", R"CJ(配音后端)CJ"},
    {R"CJ(tts_base_url)CJ", R"CJ(配音服务地址)CJ"},
    {R"CJ(vram_gb_override)CJ", R"CJ(显存覆盖)CJ"},
};

// 字段名 -> {配置节, 节内的键名}。
// 画质档位不在这里：它是按显存推出来的，写死在配置里等于
// 把这台机器的显存刻进项目，换台机器就不对了。
inline constexpr const char* kSettingSections[][3] = {
    {R"CJ(fps)CJ", R"CJ(assembly)CJ", R"CJ(fps)CJ"},
    {R"CJ(crf)CJ", R"CJ(assembly)CJ", R"CJ(crf)CJ"},
    {R"CJ(subtitle_font)CJ", R"CJ(assembly)CJ", R"CJ(subtitle_font)CJ"},
    {R"CJ(subtitle_max_chars_per_line)CJ", R"CJ(assembly)CJ", R"CJ(subtitle_max_chars_per_line)CJ"},
    {R"CJ(subtitle_max_lines)CJ", R"CJ(assembly)CJ", R"CJ(subtitle_max_lines)CJ"},
    {R"CJ(scene_transition_s)CJ", R"CJ(assembly)CJ", R"CJ(scene_transition_s)CJ"},
    {R"CJ(max_attempts_per_shot)CJ", R"CJ(gates)CJ", R"CJ(max_attempts_per_shot)CJ"},
    {R"CJ(min_pixel_std)CJ", R"CJ(gates)CJ", R"CJ(min_pixel_std)CJ"},
    {R"CJ(min_frame_similarity)CJ", R"CJ(gates)CJ", R"CJ(min_frame_similarity)CJ"},
    {R"CJ(max_audio_drift_s)CJ", R"CJ(gates)CJ", R"CJ(max_audio_drift_s)CJ"},
    {R"CJ(target_lufs)CJ", R"CJ(gates)CJ", R"CJ(target_lufs)CJ"},
    {R"CJ(fallback_on_exhausted)CJ", R"CJ(gates)CJ", R"CJ(fallback_on_exhausted)CJ"},
    {R"CJ(gates_enabled)CJ", R"CJ(gates)CJ", R"CJ(enabled)CJ"},
    {R"CJ(tts_tolerance_s)CJ", R"CJ(tts)CJ", R"CJ(tolerance_s)CJ"},
    {R"CJ(tts_max_tempo_shift)CJ", R"CJ(tts)CJ", R"CJ(max_tempo_shift)CJ"},
};

}  // namespace changji::stages::prompt
