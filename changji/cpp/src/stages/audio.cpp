#include "stages/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <fstream>

#include "stages/storyboard.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

namespace fs = std::filesystem;

namespace changji::stages {

namespace {

double round3(double v) { return std::nearbyint(v * 1000.0) / 1000.0; }

void put_u32(std::ostream& o, std::uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF),
                       static_cast<char>((v >> 8) & 0xFF),
                       static_cast<char>((v >> 16) & 0xFF),
                       static_cast<char>((v >> 24) & 0xFF)};
    o.write(b, 4);
}

void put_u16(std::ostream& o, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF),
                       static_cast<char>((v >> 8) & 0xFF)};
    o.write(b, 2);
}

std::uint32_t get_u32(const std::string& s, std::size_t off) {
    if (off + 4 > s.size()) return 0;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3])) << 24);
}

std::uint16_t get_u16(const std::string& s, std::size_t off) {
    if (off + 2 > s.size()) return 0;
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(s[off]) |
        (static_cast<unsigned char>(s[off + 1]) << 8));
}

}  // namespace

void write_silence(const fs::path& path, double seconds, int sample_rate) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // 至少一帧。零帧的 wav 有些播放器和 ffmpeg 的 concat 会当成损坏文件。
    const std::uint32_t frames = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(seconds * sample_rate));
    const std::uint16_t channels = 1;
    const std::uint16_t bits = 16;
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(sample_rate) * channels * (bits / 8);
    const std::uint32_t data_bytes = frames * channels * (bits / 8);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw AudioError("写不了音频文件：" + paths::to_utf8(path));

    f.write("RIFF", 4);
    put_u32(f, 36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(f, 16);
    put_u16(f, 1);            // PCM
    put_u16(f, channels);
    put_u32(f, static_cast<std::uint32_t>(sample_rate));
    put_u32(f, byte_rate);
    put_u16(f, static_cast<std::uint16_t>(channels * (bits / 8)));
    put_u16(f, bits);
    f.write("data", 4);
    put_u32(f, data_bytes);

    const std::string zeros(4096, '\0');
    std::uint32_t left = data_bytes;
    while (left > 0) {
        const std::uint32_t take = std::min<std::uint32_t>(left, 4096);
        f.write(zeros.data(), take);
        left -= take;
    }
}

std::optional<double> wav_peak_ratio(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    // 整个读进来。这条只在时长短过绝对下限时才会被调，文件也就几十 KB。
    std::string all((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (all.size() < 12 || all.compare(0, 4, "RIFF") != 0 ||
        all.compare(8, 4, "WAVE") != 0) {
        return std::nullopt;
    }
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    std::size_t data_off = 0;
    std::size_t data_len = 0;
    std::size_t off = 12;
    while (off + 8 <= all.size()) {
        const std::string id = all.substr(off, 4);
        const std::uint32_t size = get_u32(all, off + 4);
        if (id == "fmt ") {
            format = get_u16(all, off + 8);
            bits = get_u16(all, off + 22);
        } else if (id == "data") {
            data_off = off + 8;
            data_len = std::min<std::size_t>(size, all.size() - data_off);
            break;
        }
        off += 8 + size + (size % 2);
    }
    if (format != 1 || bits != 16 || data_len < 2) return std::nullopt;

    int peak = 0;
    for (std::size_t i = data_off; i + 1 < data_off + data_len; i += 2) {
        const int v = static_cast<std::int16_t>(get_u16(all, i));
        peak = std::max(peak, v < 0 ? -v : v);
    }
    return peak / 32768.0;
}

double probe_wav_duration(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));

    // 只读头部。一段几十秒的 wav 是几兆，为了读时长全读进来是浪费，
    // 而配音一集要读几十次。
    std::string head(1024, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.size() < 12 || head.compare(0, 4, "RIFF") != 0 ||
        head.compare(8, 4, "WAVE") != 0) {
        throw AudioError("不是 WAV 文件：" + paths::to_utf8(path));
    }

    // **块要逐个走，不能假定 fmt 在 12、data 在 36。** 很多 TTS 引擎
    // 会插一个 LIST 块写元数据，按固定偏移读的话拿到的是垃圾数——
    // 而垃圾数会变成一个荒唐的时长，然后镜头按它锁定。
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t data_bytes = 0;

    std::size_t off = 12;
    while (off + 8 <= head.size()) {
        const std::string id = head.substr(off, 4);
        const std::uint32_t size = get_u32(head, off + 4);
        if (id == "fmt ") {
            channels = get_u16(head, off + 10);
            sample_rate = get_u32(head, off + 12);
            bits = get_u16(head, off + 22);
        } else if (id == "data") {
            data_bytes = size;
            break;
        }
        // 块大小是奇数时后面有一个填充字节。
        off += 8 + size + (size % 2);
    }

    if (sample_rate == 0 || channels == 0 || bits == 0) {
        throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));
    }
    if (data_bytes == 0) {
        // data 块在头 1KB 之外（元数据很长时会这样）。退回按文件大小估：
        // 比抛异常强——那会让整条流水线断在一句台词上。
        std::error_code ec;
        const auto total = fs::file_size(path, ec);
        if (ec || total <= off) {
            throw AudioError("音频文件读不出时长：" + paths::to_utf8(path));
        }
        data_bytes = static_cast<std::uint32_t>(total - off - 8);
    }

    const double frames =
        static_cast<double>(data_bytes) / (channels * (bits / 8.0));
    return frames / sample_rate;
}

TTSBackend estimate_backend(int sample_rate) {
    TTSBackend b;
    b.name = "estimate";
    b.synthesize = [sample_rate](const std::string& text, const fs::path& out,
                                 const std::optional<std::string>&,
                                 const std::string&, double) {
        SynthesisResult r;
        r.duration_s = estimate_speech_duration(text);
        write_silence(out, r.duration_s, sample_rate);
        r.audio_path = out;
        return r;
    };
    // 刻意不给 list_voices：估算后端没有音色的概念，
    // 给一个空列表的话会被当成"问到了但一条都没有"，
    // 而那会让 voice_for 去自动挑选，挑出个空的。
    return b;
}

// ---- AudioStage ----

AudioStage::AudioStage(TTSBackend backend, config::TTSConfig config,
                       models::ProjectPaths paths)
    : backend_(std::move(backend)),
      config_(std::move(config)),
      paths_(std::move(paths)) {}

void AudioStage::split_long_lines(models::Shot& shot) const {
    const double limit = max_line_seconds();
    std::vector<models::DialogueLine> out;
    for (const auto& line : shot.dialogue) {
        const auto pieces = split_long_text(line.text, limit);
        if (pieces.size() <= 1) {
            out.push_back(line);
            continue;
        }
        for (const auto& piece : pieces) {
            models::DialogueLine clone = line;
            clone.text = piece;
            // 切开之后原来那份音频对不上了，清掉重新合成。
            // 不清的话新的一段会指向整句的音频，混音时那一句会被念两遍。
            clone.audio_path.reset();
            clone.actual_duration_s.reset();
            out.push_back(clone);
        }
    }
    shot.dialogue = out;
}

const std::vector<std::string>& AudioStage::available_voices() {
    if (!voices_.has_value()) {
        // 查一次就够，缓存住。一集四十镜、每镜两句，不缓存就是八十次请求。
        voices_ = backend_.list_voices ? backend_.list_voices()
                                       : std::vector<std::string>{};
    }
    return *voices_;
}

std::optional<std::string> AudioStage::voice_for(
    const models::DialogueLine& line, const models::AssetLibrary& assets) {
    // **音色由角色资产决定，不由分镜决定。** 分镜是大模型写的，
    // 它没有理由知道这个项目里有哪些音色。
    const models::Character* ch = nullptr;
    if (line.char_id.has_value()) {
        const auto it = assets.characters.find(*line.char_id);
        if (it != assets.characters.end()) ch = &it->second;
    }
    const std::optional<std::string> wanted =
        ch != nullptr ? ch->voice_id : line.voice_id;

    const auto& available = available_voices();
    if (available.empty()) {
        // 问不到列表就别自作主张，照填的来。
        return wanted;
    }

    if (wanted.has_value() && !wanted->empty()) {
        if (std::find(available.begin(), available.end(), *wanted) !=
            available.end()) {
            // 人工指定的优先，不该被自动挑选覆盖。
            return wanted;
        }
        // 老项目里存的可能是 v_角色名 这种早年自造的 id，服务端不认。
        // 原样提交上去节点会拒绝，整条流水线断在配音这一步。
        // 退回自动挑选，让老项目还能跑，而不是让人先去挨个改一遍音色。
        unknown_.insert(*wanted);
    }

    if (ch == nullptr) {
        return models::pick_voice(available, "", 0);   // 旁白
    }
    return models::pick_voice(available, ch->voice_gender, ch->voice_order);
}

double AudioStage::lock_duration(models::Shot& shot, double speech_s) const {
    // 没有台词的镜头保持分镜给的时长不动，它们是节奏调节的余量。
    if (shot.dialogue.empty()) return shot.duration_s;
    // 有台词的向上吸附到可生成档位并锁定。**宁长勿短**：
    // 短了会截断台词，长了尾巴上留一点表演余韵反而自然。
    const double locked = ceil_duration(speech_s + kTailS);
    shot.duration_s = locked;
    shot.duration_locked = true;
    return locked;
}

ShotAudioPlan AudioStage::process_shot(models::Shot& shot,
                                       const models::AssetLibrary& assets,
                                       pipeline::CancelToken& tok) {
    // 过长的台词先按字数估一遍切开。估算只是第一道——
    // 合成出来的真实时长常常比估的长两成，所以合成之后还要再验一次。
    split_long_lines(shot);
    const double limit = max_line_seconds();

    std::vector<models::DialogueLine> done;
    std::deque<models::DialogueLine> queue(shot.dialogue.begin(),
                                           shot.dialogue.end());
    int idx = 0;
    int splits = 0;

    while (!queue.empty()) {
        if (tok.cancelled()) throw AudioError("已取消");
        models::DialogueLine line = queue.front();
        queue.pop_front();

        const auto voice = voice_for(line, assets);
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%02d.wav", shot.shot_id.c_str(), idx);
        const fs::path out = paths_.audio() / paths::from_utf8(name);

        const SynthesisResult result = backend_.synthesize(
            line.text, out, voice, line.emotion, line.emotion_intensity);

        // **合成出来还是装不下，按实测语速重切一次再合成。**
        //
        // 光靠估算不行：估的是每秒 4.6 个字，不同引擎不同音色差得很远。
        // 实测有过估 4.8 秒、出来 5.8 秒的，那 1 秒就盖到下一镜上了。
        if (result.duration_s > limit && splits < kMaxResplits &&
            text::utf8_len(line.text) > 6) {
            // 估算偏了多少就把预算缩多少：估 E 秒实际 D 秒，
            // 想让实际落到 limit，就得按 limit * E / D 去估。
            // 再留一成余量，免得来回重切。
            const double est = estimate_speech_duration(line.text);
            const double scaled = limit * est / result.duration_s * 0.9;
            const auto pieces = split_long_text(line.text, std::max(0.8, scaled));
            if (pieces.size() > 1) {
                ++splits;
                // 倒着插回队首，保持原来的先后。
                for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) {
                    models::DialogueLine clone = line;
                    clone.text = *it;
                    clone.audio_path.reset();
                    clone.actual_duration_s.reset();
                    queue.push_front(clone);
                }
                continue;
            }
        }

        line.actual_duration_s = round3(result.duration_s);
        if (result.audio_path.has_value()) {
            line.audio_path = paths_.rel(*result.audio_path);
        }
        line.voice_id = voice;
        done.push_back(line);
        ++idx;
    }

    shot.dialogue = done;
    double total = 0.0;
    for (const auto& l : done) total += l.actual_duration_s.value_or(0.0);
    const double locked = lock_duration(shot, total);

    ShotAudioPlan plan;
    plan.shot_id = shot.shot_id;
    plan.speech_duration_s = round3(total);
    plan.locked_duration_s = locked;
    plan.slack_s = round3(locked - total);
    plan.lines = static_cast<int>(done.size());
    return plan;
}

std::vector<ShotAudioPlan> AudioStage::run(std::vector<models::Shot*>& shots,
                                           const models::AssetLibrary& assets,
                                           pipeline::JobProgress& progress,
                                           pipeline::CancelToken& tok) {
    std::vector<ShotAudioPlan> plans;
    const int total = static_cast<int>(shots.size());
    int index = 0;

    for (models::Shot* shot : shots) {
        ++index;
        if (tok.cancelled()) break;

        {
            pipeline::Event e;
            e.stage = "audio";
            e.kind = "progress";
            e.current = index;
            e.total = total;
            e.shot_id = shot->shot_id;
            e.message = "配音 " + shot->shot_id;
            progress.report(e);
        }

        try {
            plans.push_back(process_shot(*shot, assets, tok));
            shot->status = models::ShotStatus::AUDIO_DONE;
        } catch (const std::exception& e) {
            // 一镜配音失败不拖垮后面几镜。这一镜的状态不推进，
            // 后面的闸门会看出"有台词但没有配音时长"。
            pipeline::Event ev;
            ev.stage = "audio";
            ev.kind = "warn";
            ev.shot_id = shot->shot_id;
            ev.message = shot->shot_id + " 配音失败：" + e.what();
            progress.report(ev);
        }
    }

    if (!unknown_.empty()) {
        std::string names;
        for (const auto& v : unknown_) {
            if (!names.empty()) names += "、";
            names += v;
        }
        pipeline::Event e;
        e.stage = "audio";
        e.kind = "warn";
        e.message = "这些音色服务端上没有，已自动换成可用的：" + names +
                    "。想指定的话去角色场景页从下拉框里选";
        progress.report(e);
    }
    return plans;
}

}  // namespace changji::stages
