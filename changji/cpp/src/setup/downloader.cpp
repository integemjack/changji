#include "setup/downloader.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "util/paths.hpp"
#include "util/proc.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using clock_type = std::chrono::steady_clock;

namespace changji::setup {

namespace {

/// 盯着下载器的间隔。**别调到 200 毫秒以下**：每次都要 stat 一个文件、
/// 读一段日志，而这条循环在整个下载期间一直转。
constexpr int kPollMs = 400;

/// 一个文件最多重来几次。网络抖一下是常态，每次都从断点续，
/// 所以重试很便宜；真正下不动的（地址没了、盘满了）三次也救不回来。
constexpr int kMaxAttempts = 3;

std::uint64_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

/// 读文件末尾的一段。日志会长到几 MB，每次全读一遍太浪费。
std::string tail_of(const fs::path& p, std::size_t bytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end <= 0) return {};
    const auto want = static_cast<std::streamoff>(bytes);
    const std::streamoff from = end > want ? end - want : std::streamoff(0);
    in.seekg(from, std::ios::beg);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}

/// 日志里最后几行，报错时带给用户。
///
/// **必须带上。** 只说"下载失败"的话，用户看不出是地址 404、盘满了、
/// 还是证书过期——这三种的下一步动作完全不同。
std::string last_lines(const std::string& text, int count) {
    std::string flat = text;
    for (char& c : flat) {
        if (c == '\r') c = '\n';
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= flat.size()) {
        const auto nl = flat.find('\n', start);
        const auto end = nl == std::string::npos ? flat.size() : nl;
        std::string line = flat.substr(start, end - start);
        // 去掉两头的空白和 aria2 画的那些分隔线
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        if (!line.empty() && line.find_first_not_of("=-*") != std::string::npos) {
            lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    std::string out;
    const std::size_t from =
        lines.size() > static_cast<std::size_t>(count) ? lines.size() - count : 0;
    for (std::size_t i = from; i < lines.size(); ++i) {
        if (!out.empty()) out += "\n";
        out += lines[i];
    }
    return out;
}

}  // namespace

const char* to_string(ItemState v) {
    switch (v) {
        case ItemState::Pending: return "pending";
        case ItemState::Running: return "running";
        case ItemState::Done: return "done";
        case ItemState::Present: return "present";
        case ItemState::Failed: return "failed";
        case ItemState::Canceled: return "canceled";
    }
    return "pending";
}

const char* to_string(RunState v) {
    switch (v) {
        case RunState::Idle: return "idle";
        case RunState::Running: return "running";
        case RunState::Done: return "done";
        case RunState::Failed: return "failed";
        case RunState::Canceled: return "canceled";
    }
    return "idle";
}

std::optional<std::uint64_t> parse_human_bytes(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const std::size_t num_start = i;
    while (i < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[i])) != 0 || text[i] == '.')) {
        ++i;
    }
    if (i == num_start) return std::nullopt;
    double value = 0.0;
    try {
        value = std::stod(text.substr(num_start, i - num_start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    std::string unit;
    while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
        unit += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        ++i;
    }
    // aria2 用的是 1024 进制（KiB/MiB/GiB）。裸数字就是字节。
    double mul = 1.0;
    if (unit == "kib" || unit == "k" || unit == "kb") mul = 1024.0;
    else if (unit == "mib" || unit == "m" || unit == "mb") mul = 1024.0 * 1024;
    else if (unit == "gib" || unit == "g" || unit == "gb") mul = 1024.0 * 1024 * 1024;
    else if (unit == "tib" || unit == "t") mul = 1024.0 * 1024 * 1024 * 1024;
    else if (!unit.empty() && unit != "b") return std::nullopt;
    if (value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(value * mul);
}

std::optional<Aria2Progress> parse_aria2_progress(const std::string& text) {
    // 最后一条 `[#xxxxxx 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]`。
    // 取最后一条而不是第一条：日志是追加的，前面那些是几秒钟以前的。
    const auto open = text.rfind("[#");
    if (open == std::string::npos) return std::nullopt;
    const auto close = text.find(']', open);
    if (close == std::string::npos) return std::nullopt;
    const std::string line = text.substr(open, close - open + 1);

    // 空格分段。gid 那一段跳过。
    const auto slash = line.find('/');
    if (slash == std::string::npos) return std::nullopt;
    const auto space_before = line.rfind(' ', slash);
    if (space_before == std::string::npos) return std::nullopt;
    const auto paren = line.find('(', slash);

    Aria2Progress p;
    const auto got = parse_human_bytes(line.substr(space_before + 1, slash - space_before - 1));
    if (!got.has_value()) return std::nullopt;
    p.downloaded = *got;
    if (paren != std::string::npos) {
        const auto total = parse_human_bytes(line.substr(slash + 1, paren - slash - 1));
        if (total.has_value()) p.total = *total;
    }
    const auto dl = line.find("DL:");
    if (dl != std::string::npos) {
        const auto end = line.find(' ', dl);
        const auto len = end == std::string::npos ? std::string::npos : end - dl - 3;
        const auto speed = parse_human_bytes(line.substr(dl + 3, len));
        if (speed.has_value()) p.speed_bps = static_cast<double>(*speed);
    }
    return p;
}

std::string pick_tool() {
    // aria2c 排前面：这些镜像单连接常常只有 1～2 MB/s，八连接能到
    // 10 MB/s 以上，43 GB 那一档差的是五个小时。
    if (proc::which("aria2c").has_value()) return "aria2c";
    if (proc::which("curl").has_value()) return "curl";
    return {};
}

json Snapshot::to_json() const {
    json items_json = json::array();
    for (const auto& it : items) {
        items_json.push_back({{"group", it.group},
                              {"option", it.option},
                              {"name", it.name},
                              {"note", it.note},
                              {"total", it.total},
                              {"downloaded", it.downloaded},
                              {"speedBps", it.speed_bps},
                              {"state", to_string(it.state)},
                              {"error", it.error}});
    }
    return {{"state", to_string(state)},
            {"items", items_json},
            {"error", error},
            {"tool", tool},
            {"source", source},
            {"dir", dir},
            {"total", total},
            {"downloaded", downloaded},
            {"speedBps", speed_bps},
            {"etaSeconds", eta_seconds}};
}

Downloader& Downloader::instance() {
    static Downloader d;
    return d;
}

Downloader::~Downloader() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Downloader::running() const { return running_.load(); }

Snapshot Downloader::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

bool Downloader::start(std::vector<Item> items, const fs::path& dir,
                       const std::string& source,
                       std::function<void(const Item&)> on_item_done) {
    if (running_.exchange(true)) return false;
    // 上一轮的线程还挂着就先收掉。**不能不 join**：worker_ 被赋值时
    // 如果还是 joinable 的，std::thread 的赋值运算符会直接 terminate。
    if (worker_.joinable()) worker_.join();
    cancel_ = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        snap_ = Snapshot{};
        snap_.state = RunState::Running;
        snap_.tool = pick_tool();
        snap_.source = source;
        snap_.dir = paths::to_utf8(dir);
        for (const auto& it : items) {
            ItemProgress p;
            p.group = it.group;
            p.option = it.option;
            p.name = it.file.name;
            p.note = it.file.note;
            p.total = it.file.bytes;
            snap_.items.push_back(std::move(p));
            snap_.total += it.file.bytes;
        }
    }

    worker_ = std::thread(&Downloader::run, this, std::move(items), dir,
                          std::move(on_item_done));
    return true;
}

void Downloader::cancel() { cancel_ = true; }

void Downloader::run(std::vector<Item> items, fs::path dir,
                     std::function<void(const Item&)> on_item_done) {
    std::string fatal;
    bool canceled = false;

    if (pick_tool().empty()) {
        // **说清楚装什么，而不是"没有下载器"。** 这台机器上一个都没有是
        // 罕见情况，碰上的人多半也不知道该装哪个。
        fatal =
            "这台机器上没找到下载器。装一个就行：\n"
            "  Windows：winget install aria2.aria2（或者用系统自带的 curl，"
            "重开一个终端让 PATH 生效）\n"
            "  Debian/Ubuntu：apt-get install -y aria2\n"
            "  macOS：brew install aria2";
    } else {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            fatal = "建不了模型目录 " + paths::to_utf8(dir) + "：" + ec.message();
        }
    }

    if (fatal.empty()) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (cancel_) { canceled = true; break; }
            const fs::path dest = dir / paths::from_utf8(items[i].file.name);
            if (!fetch_one(items[i], dest, i)) {
                std::lock_guard<std::mutex> lock(mu_);
                if (snap_.items[i].state == ItemState::Canceled) canceled = true;
                continue;  // 一个失败不拦住别的：能下多少是多少
            }
            if (on_item_done) on_item_done(items[i]);
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (!fatal.empty()) {
        snap_.state = RunState::Failed;
        snap_.error = fatal;
    } else if (canceled || cancel_) {
        snap_.state = RunState::Canceled;
        for (auto& it : snap_.items) {
            if (it.state == ItemState::Pending || it.state == ItemState::Running) {
                it.state = ItemState::Canceled;
            }
        }
    } else {
        const bool any_failed =
            std::any_of(snap_.items.begin(), snap_.items.end(),
                        [](const ItemProgress& p) { return p.state == ItemState::Failed; });
        snap_.state = any_failed ? RunState::Failed : RunState::Done;
        if (any_failed) snap_.error = "有文件没下下来，看下面每一项的说明。重来一次会从断点接着下。";
    }
    snap_.speed_bps = 0.0;
    snap_.eta_seconds = -1.0;
    running_ = false;
}

bool Downloader::fetch_one(const Item& item, const fs::path& dest, std::size_t index) {
    const auto set = [&](auto&& fn) {
        std::lock_guard<std::mutex> lock(mu_);
        fn(snap_.items[index]);
        // 总量每次跟着重算。分开算的话会出现"明细都下完了、总数还差一截"。
        std::uint64_t sum = 0;
        for (const auto& it : snap_.items) sum += it.downloaded;
        snap_.downloaded = sum;
    };

    const std::uint64_t want = item.file.bytes;
    // 盘上已经有一份对得上的就跳过。**这就是"重来一次不会重下"**：
    // 用户中途取消、或者上一轮有几个失败了，再点一次只补缺的那几个。
    if (file_size_or_zero(dest) == want && want > 0) {
        set([&](ItemProgress& p) {
            p.state = ItemState::Present;
            p.downloaded = want;
            p.speed_bps = 0.0;
        });
        return true;
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        set([&](ItemProgress& p) {
            p.state = ItemState::Failed;
            p.error = "建不了目录 " + paths::to_utf8(dest.parent_path()) + "：" + ec.message();
        });
        return false;
    }

    const std::string tool = pick_tool();
    const fs::path log = dest.parent_path() / ".changji-download.log";

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (cancel_) {
            set([](ItemProgress& p) { p.state = ItemState::Canceled; });
            return false;
        }

        // 日志每次清掉。留着的话上一次的进度行会被当成这一次的——
        // 表现是进度条一开始就停在上次断掉的位置不动。
        fs::remove(log, ec);

        std::vector<std::string> args;
        if (tool == "aria2c") {
            args = {"-x", "8", "-s", "8", "-k", "4M",
                    "--file-allocation=none", "--auto-file-renaming=false",
                    "--allow-overwrite=true", "--continue=true",
                    "--max-tries=3", "--retry-wait=5",
                    "--console-log-level=warn", "--summary-interval=1",
                    "-d", paths::to_utf8(dest.parent_path()),
                    "-o", paths::to_utf8(dest.filename()), item.url};
        } else {
            // curl 是单连接，文件大小就是真进度，不用解析输出。
            args = {"-L", "--fail", "--retry", "3", "--retry-delay", "5",
                    "-C", "-", "-o", paths::to_utf8(dest), item.url};
        }

        const proc::ProcHandle h = proc::spawn(tool, args, log);
        if (h == 0) {
            set([&](ItemProgress& p) {
                p.state = ItemState::Failed;
                p.error = "起不来 " + tool + "。检查它在不在 PATH 上。";
            });
            return false;
        }

        set([](ItemProgress& p) {
            p.state = ItemState::Running;
            p.error.clear();
        });

        // 盯着它。速度优先用下载器自己报的（aria2 的 DL:），
        // 拿不到就自己按文件大小的增量算。
        auto last_at = clock_type::now();
        std::uint64_t last_bytes = file_size_or_zero(dest);
        double smoothed = 0.0;
        bool killed = false;

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            if (cancel_) {
                proc::kill_spawned(h, 2000);
                killed = true;
                break;
            }
            const bool still = proc::alive(h);

            std::uint64_t now_bytes = file_size_or_zero(dest);
            double speed = 0.0;
            if (tool == "aria2c") {
                const auto parsed = parse_aria2_progress(tail_of(log, 4096));
                if (parsed.has_value()) {
                    // **用它报的字节数，不用文件大小。** 多连接下载会往
                    // 文件中间写，表观大小随时可能跳到接近全长，
                    // 那样进度条会先冲到 99% 再原地不动。
                    now_bytes = parsed->downloaded;
                    speed = parsed->speed_bps;
                }
            }
            const auto now = clock_type::now();
            if (speed <= 0.0) {
                const double dt =
                    std::chrono::duration<double>(now - last_at).count();
                if (dt > 0.0 && now_bytes >= last_bytes) {
                    speed = static_cast<double>(now_bytes - last_bytes) / dt;
                }
            }
            last_at = now;
            last_bytes = now_bytes;
            // 平滑一下。瞬时速度抖得厉害，界面上那个数字会闪到看不清。
            smoothed = smoothed <= 0.0 ? speed : smoothed * 0.7 + speed * 0.3;

            const double shown = smoothed;
            const std::uint64_t bytes = now_bytes;
            set([&](ItemProgress& p) {
                p.downloaded = bytes;
                p.speed_bps = shown;
            });
            {
                std::lock_guard<std::mutex> lock(mu_);
                snap_.speed_bps = shown;
                snap_.eta_seconds =
                    shown > 1.0 && snap_.total > snap_.downloaded
                        ? static_cast<double>(snap_.total - snap_.downloaded) / shown
                        : -1.0;
            }

            if (!still) break;
        }

        if (killed) {
            set([](ItemProgress& p) {
                p.state = ItemState::Canceled;
                p.speed_bps = 0.0;
            });
            return false;
        }

        const std::uint64_t got = file_size_or_zero(dest);
        // **字节数对不上就算没下完。** 下载器退出码为 0 也可能留下截断的
        // 文件（连接被中间设备掐断、镜像返回了一个错误页）。
        // 而截断的权重加载时报的是"读不对"，指向完全错误的方向。
        if (want == 0 || got == want) {
            set([&](ItemProgress& p) {
                p.state = ItemState::Done;
                p.downloaded = want == 0 ? got : want;
                p.speed_bps = 0.0;
            });
            return true;
        }

        const std::string detail = last_lines(tail_of(log, 4096), 4);
        if (attempt == kMaxAttempts) {
            set([&](ItemProgress& p) {
                p.state = ItemState::Failed;
                p.speed_bps = 0.0;
                p.error = "下了 " + std::to_string(got) + " 字节，应该是 " +
                          std::to_string(want) + "。试了 " +
                          std::to_string(kMaxAttempts) + " 次都没下全。" +
                          (detail.empty() ? "" : "\n下载器最后说：\n" + detail);
            });
            return false;
        }
        set([&](ItemProgress& p) {
            p.error = "第 " + std::to_string(attempt) + " 次没下全，接着续传。";
        });
    }
    return false;
}

}  // namespace changji::setup
