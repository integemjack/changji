# 前置验证结论

对应方案 [第四节](../../docs/C++重构方案.md) 的六项验证。这份文件记的是
**实测出来的事实和它们对方案的影响**，不是操作日志。

验证环境（2026-09-07）：

| 项 | 值 |
|---|---|
| GPU | RTX 2060 6GB，compute 7.5（Turing） |
| 内存 | 32GB |
| 驱动 | 610.88 |
| MSVC | v14.44.35207（VS 2022 Build Tools 17.14.37614） |
| Windows SDK | 10.0.26100 |
| CMake | 4.4.3 |
| CUDA | 13.3 |
| llama.cpp | `5202104`（2026-09-07），ggml 0.23.0 |
| sd.cpp | `d8fb10c`（2026-09-07），master-846 |

> ⚠️ **这台不是选型结论那台。** [选型结论.md](../../docs/选型结论.md) 里的产能
> 数据是在 RTX 5080 16GB（sm_120，Blackwell）上测的。这里是 2060 6GB
> （sm_75，Turing）。编译期的结论（能不能编、能不能链、ABI 对不对）
> 与显卡型号无关，在这台上成立即成立；**Blackwell sm_120 的兼容性验不到**，
> 那一条本来就在选型结论的"待核实"清单里，仍然待核实。
>
> 好的一面：2060 的 6GB 正是"6GB 卡跑 16GB 模型"这个立项理由的目标场景，
> 第三项（`--offload-to-cpu`）在这台上验比在 5080 上更有说服力。

---

## 一、统一 ggml：编译不过（结论：风险二成立，且比方案写的更硬）

> **本节推翻了一个中间结论。** 在只读 CMakeLists 还没编译的阶段，我判断
> "上游已经预留了复用的路，风险二可以降级"。编译之后证明是错的。
> 保留这段过程，因为它说明为什么这个验证工程必须真的编一次，
> 光读构建脚本会得出相反的结论。

### 配置期：看起来很顺

sd.cpp 的 CMakeLists 确实预留了复用：

```cmake
# Only add ggml if it hasn't been added yet
if (NOT TARGET ggml)
    if (SD_USE_SYSTEM_GGML)
        find_package(ggml REQUIRED)
        add_library(ggml ALIAS ggml::ggml)
    else()
        add_subdirectory(ggml)
    endif()
endif()
```

先 `add_subdirectory(llama.cpp)` 再加 sd.cpp，它自己就跳过自带的那份，
配置期干净通过：

```
-- ggml version: 0.23.0
-- ggml commit:  5202104              ← llama.cpp 的 HEAD
-- stable-diffusion.cpp version master-846-d8fb10c
```

### 编译期：撞墙

```
ggml_extend.hpp(1062): error C3861: "ggml_mul_mat_i8_tensorwise": 找不到标识符
ggml_extend.hpp(2777): error C2065: "GGML_TYPE_F8_E4M3": 未声明的标识符
ggml_extend.hpp(2777): error C2065: "GGML_TYPE_F8_E5M2": 未声明的标识符
ggml_extend.hpp(2811): error C3861: "ggml_quantize_i8_convrot": 找不到标识符
```

这些符号的分布：

| 符号 | leejet/ggml | llama.cpp 的 ggml |
|---|---|---|
| `ggml_mul_mat_i8_tensorwise` | 2 个文件 | **0** |
| `GGML_TYPE_F8_E4M3` / `F8_E5M2` | 8 个文件 | **0** |
| `ggml_quantize_i8_convrot` | 2 个文件 | **0** |

**sd.cpp 硬依赖 [leejet/ggml](https://github.com/leejet/ggml) 这个 fork 的私有扩展**：
FP8 张量类型、int8 convrot 量化、i8 tensorwise 矩阵乘。而且调用点
**没有 `#ifdef` 守卫**——不是可以关掉的可选特性，是主代码路径。
sd.cpp 的 `docs/int8_convrot.md` 就是讲这套东西的。

所以方案风险二里"退路是 fork 其中一个"这句**要保留，而且它不是退路，
是唯一的路**。三个可能的方向，按代价从低到高：

1. **反过来，让 llama.cpp 用 leejet/ggml。** llama.cpp 有
   `option(LLAMA_USE_SYSTEM_GGML "Use system libggml" OFF)`，接口是现成的。
   问题是 leejet 的 fork 相对 llama.cpp 需要的 ggml 落后多少——llama.cpp
   对自己那份 ggml 的耦合很紧，新算子加得很频繁。**这是下一个要做的实验。**
2. **把 leejet 的扩展移植到 llama.cpp 的 ggml 上**，长期维护一个补丁集。
   代价是每次两个上游前进都要重新对齐，正是方案里写的那笔长期维护成本。
3. **放弃 sd.cpp 的 FP8 / int8 convrot 路径**，自己改 sd.cpp 把那些调用点
   换成通用实现。等于 fork sd.cpp。

无论走哪条，"两个库各自 vendor 不同 commit 的 ggml，同时链接会出问题"
这个判断是对的，只是出问题的形式不是符号冲突，而是**其中一方的私有扩展
在另一方那里根本不存在**。

### 反方向也编不过：让 llama.cpp 用 leejet/ggml

两边的 CMakeLists 都用 `if (NOT TARGET ggml)` 包着自己那份，llama.cpp 那边
还明写了 `# ... otherwise assume ggml is added by a parent CMakeLists.txt`。
所以父工程先把 leejet/ggml 加进来，两个库都会复用它——**配置期干净通过，
退出码 0**。

编译期失败，这次轮到 llama.cpp：

```
error C2039: "mmap_support": 不是 "ggml_backend_dev_caps" 的成员
error C2660: "ggml_ssm_scan": 函数不接受 9 个参数
error C3861: "ggml_flash_attn_ext_set_n_kv_max": 找不到标识符
error C3861: "ggml_rope_set_offset": 找不到标识符
error C3861: "ggml_swiglu_clamp": 找不到标识符
```

leejet 的 fork 自报 0.19.0，llama.cpp 的 ggml 是 0.23.0，四个小版本的差距是
真的。（中途我抽查过几个 API 觉得"并不落后"，那是抽样太浅——抽到的正好
是两边都有的。这里再记一次教训：ggml 这种改动频繁的库，判断版本差距要靠
编译，不要靠抽查符号。）

### 结论：当前两个上游的 HEAD 上，统一 ggml 双向不可行

| 方向 | 结果 | 原因 |
|---|---|---|
| 用 llama.cpp 的 ggml（0.23.0） | 编不过 | sd.cpp 硬依赖 leejet fork 的 FP8 类型、int8 convrot、i8 tensorwise matmul |
| 用 leejet 的 ggml（0.19.0） | 编不过 | llama.cpp 需要 0.19 之后新增的算子和结构体字段 |

四条出路，按代价排：

1. **放弃"进程内链接 llama.cpp"。** 只链 sd.cpp，文本推理走 HTTP（llama.cpp
   自带的 server 就是 OpenAI 兼容的）。这样根本没有两份 ggml，问题不存在。
   代价是"严格单一二进制"在文本推理这一环打折——而这两条正是决策台账里
   第 2 项和第 5 项定下来的。**这个验证结果直接把那两个决策推回到桌面上。**
2. **钉住旧版 llama.cpp**，找 ggml 还是 0.19.x 那个时期的版本。代价是文本
   推理的能力和性能停在那个时间点，且以后 sd.cpp 更新 fork 时要重新找。
3. **把 leejet 的三组扩展移植到新版 ggml 上**，自己维护一个补丁集。
   两个上游各自前进时都要重新对齐。这是方案风险二里写的那笔长期维护成本，
   实际比原文估计的重——不是"找一个能编的 commit"，是持续移植。
4. **fork sd.cpp**，把 FP8 / int8 convrot 那些调用点改成通用实现。
   等于放弃 leejet 在量化上的工作，而那正是低显存跑大模型的关键。

### 已定：走"移植 leejet 扩展 + 维护补丁集"（决策 17）

四条出路里选了第 3 条。选完立刻量了工作量，结论是**这条路比方案原文估计的
轻**，理由有三：

**一、分叉点很近，改动几乎纯增量。** leejet/ggml 相对上游的分叉点是
`8846b79e6`（2026-08-12），到 HEAD 是 **31 文件、+2564 −17、79 个 hunk**。
只删 17 行——补丁集最怕改动散布在上游会变的代码里，纯新增的东西 rebase 轻得多。
那个"0.19 vs 0.23"的版本号差距是 fork 里没跟着改的陈迹，实际只落后四周。

**二、73/79 个 hunk 能直接落到 llama.cpp 的 ggml 上。** 实测 `git apply --reject`
到 llama.cpp `5202104`：

| 需手工处理 | hunk | 性质 |
|---|---|---|
| `include/ggml-rpc.h` | 1 | `GGML_OP_COUNT` 断言基数不同 |
| `src/ggml-cuda/CMakeLists.txt` | 1 | 纯新增的 cuBLASLt FP8 块 |
| `src/ggml-cuda/common.cuh` | 2 | 加 `cublaslt_handles` 成员和 getter |
| `src/ggml-cuda/ggml-cuda.cu` | 1 | 析构里加 `cublasLtDestroy`（这文件 +699 行只冲突这一处） |
| `src/ggml-vulkan/ggml-vulkan.cpp` | 1 | struct 里加三个 pipeline 成员声明 |

**六个全是纯新增上的上下文漂移，零语义冲突。**

**三、上游白送了三个测试。** `test-fp8-cast`、`test-fp8-matmul`、
`test-int8-convrot` 共 581 行，正好用来验证移植对不对，不用自己造。

两个不算冲突但要手工处理的：

- **Metal shader 的布局变了。** 上游已把 Metal shader 拆进
  `ggml-metal/kernels/`，leejet 的 fork 还是单体 `ggml-metal.metal`。
  那 66 行 FP8 shader 要手工搬进新布局。（`ggml-metal-device.m` 的 3 行干净落地。）
- **`tests/CMakeLists.txt` 无处安放。** llama.cpp 不 vendor ggml 的独立 tests 目录，
  三个测试要另找挂载点。

补丁和维护约定见 [../patches/](../patches/)。**核心约定：上游一动就要重跑，
冲突 hunk 数是这条路的健康度指标**——从 6 涨到几十就说明上游在这些区域动了
大手术，该重新评估。

## 二、真正的坑是一个编译期常量，不是符号冲突

sd.cpp 的 CMakeLists：

```cmake
if (NOT SD_USE_SYSTEM_GGML)
    add_definitions(-DGGML_MAX_NAME=160)    # 上游 ggml 默认是 64
endif()
```

`add_definitions` 只作用于**它之后的目录作用域**，加不到已经在别处编好的
llama.cpp 的 ggml 上。结果是 ggml 按 64 编、sd.cpp 按 160 编，两边看到的
`ggml_tensor` 结构体大小不一致。

**链接照样通过，错在运行时踩内存。** 这比符号冲突难查得多，因为它不报错。

解法是在顶层 `add_compile_definitions(GGML_MAX_NAME=160)`，让所有目标用
同一个值。[CMakeLists.txt](CMakeLists.txt) 里那一行就是干这个的，删掉它
编译一样过。

[main.cpp](main.cpp) 的 `check_ggml_abi()` 是这一条的运行时探针：写一个
100 字符的张量名再读回来，**库自己按多少编的，名字就在多少字节处被截断**。
测的是库的编译期常量，不是本程序的。

## 三、上游不能被拖进 C++20（结论：changji/cpp/CMakeLists.txt 要改）

三个项目声明的 C++ 标准：

| 项目 | 声明 |
|---|---|
| llama.cpp | `target_compile_features(llama PRIVATE cxx_std_17)  # don't bump` |
| ggml | `set(CMAKE_CXX_STANDARD 17)` + 同样的 `# don't bump` |
| sd.cpp | `set(CMAKE_CXX_STANDARD 17)` |
| **changji/cpp** | **`set(CMAKE_CXX_STANDARD 20)`** |

`target_compile_features(... cxx_std_17)` 声明的是**下限不是上限**。父作用域给
`CMAKE_CXX_STANDARD 20`，"至少 17"被满足，上游整个按 C++20 编。

然后在 MSVC 上炸：

```
llama-chat.cpp(557): error C2088: 内置运算符"<<"无法应用于 std::stringstream
llama-chat.cpp(561): error C2280: operator<<(..., const char8_t*) 尝试引用已删除的函数
```

根因比表面更精确——llama.cpp 其实处理过 C++20：

```cpp
#if __cplusplus >= 202000L
    #define LU8(x) (const char*)(u8##x)
#else
    #define LU8(x) u8##x
#endif
```

但 **MSVC 不加 `/Zc:__cplusplus` 时 `__cplusplus` 永远是 `199711L`**，这个守卫
静默失效，走 `#else` 拿到 `const char8_t*`，而 C++20 删除了
`operator<<(ostream&, const char8_t*)`。**只在「MSVC + C++20」这个组合下出现，
GCC 上编不出来**——阶段 0 用 MinGW 编所以从没遇到。

**对方案的影响**：[changji/cpp/CMakeLists.txt](../CMakeLists.txt) 现在设的就是
`CMAKE_CXX_STANDARD 20`（注释写着为了 `std::format` 和 designated initializer）。
阶段 5 把 sd.cpp 和 llama.cpp 加进去时会原样撞上。改法是全局停在 17，
只用 `target_compile_features(changji PRIVATE cxx_std_20)` 把自己的目标抬上去。

给 MSVC 加 `/Zc:__cplusplus` 能修好这一个点，但上游那句 `# don't bump` 说明
他们并不保证 C++20 下的其它地方，不要走这条路。

## 三点五、验证第一项：MSVC + CUDA —— 通过

**结论：通过。** 用 MSVC + CUDA 13.3 把 sd.cpp 那条链（ggml + ggml-cuda +
stable-diffusion）完整编出来了，零错误。

| 产物 | 大小 |
|---|---|
| `stable-diffusion.lib` | 135 MB |
| `ggml-cuda.dll` | 50 MB |
| `ggml-base.dll` / `ggml-cpu.dll` / `ggml.dll` | 641K / 811K / 66K |

编译架构 `CMAKE_CUDA_ARCHITECTURES=75`（sm_75，本机 RTX 2060 是 Turing）。
单架构编译是有意的：全架构编译产物有好几个 G，验证阶段没必要。

方案第四节里"MSVC + CUDA 能编"这一项可以划掉。**阶段 1 之前切 MSVC 这个
决定没有障碍。**

### 版本兼容性


nvcc 的 host_config.h 里那道门：

```c
#if defined(_MSC_VER)
#if _MSC_VER < 1920 || _MSC_VER >= 1960
```

合法区间是 `_MSC_VER` ∈ [1920, 1959]。本机 MSVC 14.44（VS 2022 17.14）是
**1944**，落在区间内。CUDA 13.3.73 + VS 2022 17.14 这个组合**合法**，
不需要为了 nvcc 去降 MSVC 版本——这是切 MSVC 之前最担心的一件事。

一个只会咬一次但很费时间的坑：CUDA 安装器把 VS 集成
（`BuildTools/MSBuild/Microsoft/VC/v170/BuildCustomizations/CUDA 13.3.targets`）
装好了，`CUDA_PATH` 也写进了机器级环境变量，但**装之前就已经起着的 shell
拿不到它**。CMake 探测 CUDA 编译器会失败并报

```
error : The CUDA Toolkit directory '' does not exist.
```

这句话指向"CUDA 没装好"，实际是"环境变量没进程内"。装完 CUDA 要开新 shell，
或者显式把 `CUDA_PATH` 传进去。

## 三点六、Metal 移植：已在 Apple M1 上验证通过

验证机：`wangchao@192.168.20.14`，macOS 26.6.2 / Apple M1 / 16GB / Xcode 21。

补丁里 Metal 那 68 行有一半不能机械落地——上游已把 Metal shader 拆进
`ggml-metal/kernels/`，leejet 的 fork 还是单体 `ggml-metal.metal`。搬迁脚本见
[../patches/port_metal_fp8.py](../patches/port_metal_fp8.py)。

**结果：上游 ggml + 补丁 + 移植脚本，编译零错误，`test-fp8-cast` 退出码 0，
四个 FP8 cast pipeline 在 M1 上编译加载，与 leejet 原版输出一致。**

这个测试是穷举的：遍历 E4M3 / E5M2 全部 256 个字节值，逐个与参考实现比对
并检查 NaN，任一不符即退出码 1。所以它证明的是整个输入域都正确，不是"能加载"。

三个附带结论：

**一、Metal 后端不需要 Xcode 的离线 Metal 工具链。** 这台机器上
`xcrun -sdk macosx metal` 报 "missing Metal Toolchain"（新版 Xcode 把它拆成了
2–3GB 的单独下载组件），但 `GGML_METAL_EMBED_LIBRARY` 默认 ON，嵌的是
`kernels/*.metal` 源码，由 `newLibraryWithSource:` 在运行时交给 macOS 的 Metal
框架编译。**省掉了一个原以为必需的大件依赖。**

**二、Metal 只得到 FP8 cast，得不到量化加速。** leejet 补丁里 Metal 部分只实现了
cast，i8 convrot 和 FP8 matmul 只有 CUDA 和 Vulkan 版本。同机实测：
`test-fp8-matmul` 整个包在 `#ifdef GGML_USE_CUDA` 里，非 CUDA 直接 `return 0`
（**退出码 0 是跳过不是通过**）；`test-int8-convrot` 只找名字含 "CUDA"/"Vulkan"
的 GPU 后端，Metal 不在列，回落到 CPU。所以 Mac 上 sd.cpp 能跑，但 int8/FP8
矩阵乘走 CPU。

**三、补丁的基准分支是 `sd.cpp` 不是 master。** leejet/ggml 的 master 只是上游
镜像（HEAD `30bf868`，提交信息 "ggml : bump version to 0.19.0 (#1581)"，PR 号
都是上游的），**里面没有任何 FP8 / convrot 代码**。按 master 生成补丁会得到
空补丁且不报错。这一条已写进 [../patches/README.md](../patches/README.md)。

## 四、Windows 长路径（工程约束，不影响方案）

llama.cpp 现在带了个 Svelte 写的 Web UI，`tools/ui/src/lib/components/app/chat/
ChatAttachments/ChatAttachmentsList/ChatAttachmentsListItem/...` 这种路径超过
Windows 的 260 字符上限。浅克隆到较深的目录时 **checkout 静默失败 3600 个文件**，
git 只在末尾提一句，`git clone` 的退出码看不出问题。

两个应对，都要：

- 克隆时带 `-c core.longpaths=true`
- 构建目录用短路径（本次用 `C:\cjv`）

这和 [cpp/README.md](../README.md) 里记的中文路径坑是同一类问题的两个面：
Windows 的路径处理会在你完全想不到的地方安静地失败。

---

## 待填

- [x] **第一项：MSVC + CUDA —— 通过**（版本兼容 + 真编一次，零错误）
- [x] 第二项：统一 ggml —— **双向都编不过**，见上
- [x] 第二项善后：出路已定（决策 17，补丁集），工作量已量化
- [x] Metal shader 搬家 —— 已在 M1 上验证通过
- [ ] 移植实施：剩 6 个 hunk（3 CUDA / 1 Vulkan / 1 ggml-rpc.h / 1 CUDA CMakeLists）
- [ ] 第二项后半：两个库各跑一次真实推理
- [ ] 第三项：`--offload-to-cpu` 的崩溃 bug 复不复现
- [ ] 第四项：`--vae-tiling` 在视频路径上通不通
- [ ] 第五项：ggml 的 Vulkan 后端在 Pi 5 上能不能起来
- [ ] 第六项：Pi 的 8GB 装不装得下 5B 的 Q4 权重加 VAE
