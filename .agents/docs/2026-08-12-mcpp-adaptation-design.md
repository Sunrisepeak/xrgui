# XRGUI × mcpp 适配与多平台构建设计

> 状态：**P0 完成，P1 进行中** · 日期：2026-08-12
> 探针证据：本机 mcpp `2026.8.11.3` + gcc `16.1.0`
>
> 决议：测试用 feature 门控 bin target（§5.3，零重命名）· slang 做 `xim:slang` 包（§8.2）
> · 标准钉 `c++23` · msdfgen 裁剪面留到 P1 实测（§13）
>
> **实施进展见 §14。** 一句话：源码符合性问题已全部清零，根包唯一剩余的构建阻塞
> 是三个第三方库（harfbuzz / msdfgen / mimalloc）尚未打包，加上 react_flow 撞上
> 一个 GCC 16 模块 bug。

---

## 1. 目标与边界

给 XRGUI 增加一套 `mcpp.toml` 描述的构建，使项目能被 `mcpp build` / `mcpp run` / `mcpp test`
驱动，并在此基础上打通 Linux。

### 1.1 已确定的四项决策

| 决策点 | 结论 |
|---|---|
| 目标终点 | **分阶段**：P0 编译通过 → P1 补齐第三方与 Vulkan 后端 → P2 Linux 上真跑起来 |
| 与 xmake 的关系 | **不动 `xmake.lua`**。mcpp.toml 独立存在，且自身要覆盖 Windows / Linux / macOS 三平台 |
| 三个 `mo_yanxi_*` 子模块 | **各自一份 mcpp.toml，以 path 依赖引入** |
| 索引里缺的第三方 | header-only **直接 vendor**；重库先建**本地包索引**，验证后贡献回 mcpp-index |

### 1.2 非目标

- 不替换、不修改 `xmake.lua`；两套构建平行存在，`xmake.lua` 仍是 Windows/MSVC 的权威路径。
- 不追求 mcpp 侧与 xmake 侧的产物二进制等价（优化档、运行时契约本来就不同）。
- 不做 Wayland 原生后端（`compat.glfw` 目前是 X11-only，XWayland 下可用）。

---

## 2. 现状盘点

### 2.1 代码规模与形态

| 组件 | 来源 | 模块接口(.ixx) | 实现(.cpp) | LOC |
|---|---|---:|---:|---:|
| xrgui `src/` + `src.backends/` + `gui.config/` | 本仓库 | 196 | ~44 | ~86k |
| `mo_yanxi_utility` | submodule（vkw 的 submodule） | 82 | 19 | ~26k |
| `mo_yanxi_vulkan_wrapper` | submodule | 33 | 4 | ~9k |
| `mo_yanxi_react_flow` | submodule | 11 | 12 | ~3k |
| **合计** | | **~322** | **~79** | **~124k** |

三个要点：

1. **模块接口用 `.ixx`**（MSVC 拼法），mcpp 默认只认 `.cppm` —— 必须显式声明
   `[build] module_extensions = [".ixx"]`。
2. **`import std;` 覆盖率 197/247 文件**，这正是 mcpp 的主场。
3. 当前 `external/` 六个 submodule **全部未初始化**（`git submodule status` 全带 `-` 前缀）。
   任何构建前置都要先 `git submodule update --init --recursive`。

### 2.2 `xmake.lua` 实际承担的职责

mcpp 侧要复刻的不是"一个构建文件"，而是下面 8 项职责：

| # | 职责 | xmake 实现 | mcpp 对应机制 |
|---|---|---|---|
| 1 | 收集 `.ixx`/`.cpp` 源 | `add_files` | `[build] sources` + `module_extensions` |
| 2 | 头文件搜索路径（7 条） | `add_includedirs` | `[build] include_dirs` |
| 3 | 全局宏（4 个） | `add_defines` | `[build] defines` |
| 4 | 第三方包（12 个） | `add_requires` | `[dependencies]` + vendor + 本地索引 |
| 5 | 子项目聚合 | `includes("external/**/xmake.lua")` | path 依赖 |
| 6 | 一份 object target 供 3 个 binary 复用 | `set_kind("object")` | mcpp 的 compile-once 模型（天然） |
| 7 | SVG → bin2c 头 + `assets_summary.h` 聚合 | 自定义 rule + `before_build` | `build.mcpp` |
| 8 | assets 拷贝到产物目录 | `after_build` | `mcpp run` 的 CWD 语义 / `[runtime] deploy_files` |

另有两条**独立于构建**的资产生成任务（`xrgui.gen_slang`、`xrgui.gen_icon`），依赖
`py` / `node` / `slangc`，产物当前**都不在仓库里**（`properties/assets/shader/spv/` 与
`properties/assets_raw/gen/icons/` 均为空）。

### 2.3 平台现状

**好消息**：平台相关代码已经隔离在 `src/platform/` 五个模块内，且 Linux 分支**已经写好**：

- `platform.ixx` — GTK bookmarks 解析（`__linux__` 分支完整）
- `font.ixx` — fontconfig 实现（`FcInitLoadConfigAndFonts` / `FcFontList`，完整）
- `thread.ixx` — pthread/sched 分支
- `memory.ixx` — `mmap`/`munmap` 分支
- `glfw.ixx` — **仅 Windows**：`get_native_window_handle()` 在非 Windows 返回空，
  `native_ime_controller` 的实现体全在 `#ifdef _WIN32` 内

全仓库 `_WIN32` 命中仅 **29 处**，全部集中在 `src/platform/`。

**结论：Linux 的缺口是 IME 与原生窗口句柄，不是"整个平台层"。** 这两项在 P2 之前可以
留空实现（IME 降级为 GLFW 的 `character callback`，句柄仅 Vulkan surface 创建需要，
而 GLFW 自带 `glfwCreateWindowSurface` 已覆盖）。

---

## 3. 可行性探针（实测，非推断）

在写方案前跑了 5 组真实构建。以下每条都有对应命令输出。

### 3.1 已验证成立的机制 ✅

| # | 假设 | 结果 |
|---|---|---|
| A | mcpp 能吃 `.ixx` 模块接口 | ✅ `module_extensions = [".ixx"]` 生效，默认 glob 自动变宽为 `src/**/*.{cppm,ixx,cpp,...}` |
| B | mcpp 能扫出这套 122k LOC 的模块依赖图 | ✅ 编 `mo_yanxi_utility` 时正确并行编译，**65 个对象 / 32 个 BMI** 产出，无一处图错误 |
| C | **path 依赖包里的任意模块，消费者可直接 `import`** | ✅ 消费者 `import my.deep.mod;` 直接成功，**不需要 lib-root，不需要在根模块 re-export** |
| D | 无 lib-root 的"模块袋"能作为 `kind = "lib"` | ✅ 不报错，正常参与链接 |
| E | path 依赖的 `include_dirs` 传播给消费者 | ✅ 消费者模块内 `#include <moy/attr.hpp>` 成功，宏值正确穿透 |
| F | 多个 `[targets.*]` bin 共享同一批 object | ✅ `shared.ixx.o` 只编一次，两个 bin 各自只有自己的入口 `.o` |
| G | `sources` glob 可以用 `../` 越出包根 | ✅ 薄包引用 submodule 源码可行（见 §4.3 过渡策略） |
| H | `mcpp run` 以**包根**为 CWD 启动产物 | ✅ `cwd=<包根>`，`ifstream("properties/probe.txt")` 直接读到 —— 见 §8.4 |

**C + E + F 三条合起来，直接证明了 §4 的四包拓扑成立。**

### 3.2 已暴露的阻塞 ⛔ —— 这是本次适配的真正成本所在

拿最小的依赖 `mo_yanxi_utility`（26k LOC / 63 个 `.ixx`）做端到端编译，
**连续撞出三类 GCC 源码符合性错误**。这些**不是构建配置问题，是源码问题**：
MSVC 不强制执行这些规则，GCC 16 强制执行。

#### 类别 ①：暴露 TU-local 实体 —— 匿名 lambda 当类型用

```
src/utility/generic/tuple_manipulate.ixx:121:7: error:
  'using mo_yanxi::reverse_tuple_t = typename decltype(<lambda>())::type'
  exposes TU-local entity 'struct mo_yanxi::<lambda()>'
note: 'mo_yanxi::<lambda()>' has no name and cannot be differentiated
      from similar lambdas in other TUs
```

模式：`using X = typename decltype([]{ ... }())::type;` 写在模块接口单元里。
匿名 lambda 闭包类型是 TU-local，导出它违反 [basic.link]/p14。

**修法**（已验证有效）：抽成具名 `consteval` 函数模板。

```cpp
// 之前
export template <typename Tuple>
using reverse_tuple_t = typename decltype([]{ /* ... */ }())::type;

// 之后
template <typename Tuple>
consteval auto reverse_tuple_impl() { /* ... 原样 ... */ }

export template <typename Tuple>
using reverse_tuple_t = typename decltype(reverse_tuple_impl<Tuple>())::type;
```

#### 类别 ②：暴露 TU-local 实体 —— `static` 内部链接函数被模块模板引用

```
src/utility/generic/function_manipulate.ixx:67:16: error:
  'consteval bool mo_yanxi::check_unique()' exposes TU-local entity
  'consteval void mo_yanxi::pin()'
```

模式：命名空间作用域的 `static consteval void pin()` 被具有模块链接的模板引用。

**修法**（已验证有效）：去掉 `static`。模块单元里本来就不需要它 —— 模块链接已经提供了封装。

#### 类别 ③：`export` 了别的模块所有的模板的特化

```
src/utility/math/basic/vector2.ixx:1368:1: error:
  explicit specializations are not permitted here
```

对应源码：

```cpp
export
template <>
struct std::hash<mo_yanxi::math::vector2<int>> { /* ... */ };
```

`std::hash` 的特化附着于**全局模块**，不能被 `export`。

**修法**：去掉 `export`（特化通过实例化点可达，不需要导出）。注意同文件里
partial specialization 的 `export` 已被作者手工注释掉（`// export`），说明这个坑
在 MSVC 上也踩过一半。

#### 规模估算（静态扫描，非精确计数）

| 仓库 | 模式① `decltype([]` | 模式② 命名空间级 `static` | 匿名 namespace | `.ixx` 数 |
|---|---:|---:|---:|---:|
| xrgui | 1 | 289 | 5 | 196 |
| mo_yanxi_utility | 3 | 216 | 0 | 82 |
| mo_yanxi_vulkan_wrapper | 0 | 35 | 0 | 33 |
| mo_yanxi_react_flow | 0 | 29 | 0 | 11 |

> ⚠️ **`static` 那一列是上界不是工作量**。只有"被具有模块链接的实体引用"的那些才报错；
> 纯文件内使用的 `static` 完全合法。实测 63 个 `.ixx` 里只有 **1 处** `static` 真正触发了错误
> （命中率 ~0.5%）。按此比例外推，全量 322 个 `.ixx` 的模式② 预计在 **5–15 处**量级。
> 模式① 全仓库只有 4 处。模式③ 需要单独 grep `export\s*\ntemplate\s*<>`。

**这类修复的性质：机械、局部、对 MSVC 侧完全无害**（三种修法在 MSVC 上语义等价）。
所以可以直接改进主干源码，不需要 `#ifdef` 分叉 —— 这一点对"不动 xmake"的约束很关键：
**源码修复是两套构建共享的收益，不是 mcpp 的私有补丁。**

### 3.3 一条必须记住的陷阱

> 任何**未被某个 `[targets.*]` 声明为 `main` 的**、含 `main()` 的源文件，都会被编进
> **共享 object 集**，链接时 `multiple definition of 'main'`。

xrgui 有三个入口：`src.hello/main.cpp`、`src.examples/main.cpp`、`src.tests/main.cpp`。
它们必须**要么**被声明为某 target 的 `main`，**要么**被 `sources` 的 `!` 前缀排除。

---

## 4. 总体架构

### 4.1 包拓扑

```
                    xrgui  (root package)
                    ├─ targets: xrgui_hello / xrgui_example (bin)
                    │
        ┌───────────┼────────────────────┐
        │ path      │ path               │ path
        ▼           ▼                    ▼
  moyanxi.utility   moyanxi.react_flow   moyanxi.vulkan_wrapper
        ▲                 │ path              │ path
        └─────────────────┴───────────────────┘

  index deps (compat.*)：glfw / freetype / vulkan(+headers,+runtime) / gtest
  local index：harfbuzz / msdfgen / mimalloc
  vendored（进 include_dirs 或 sources）：VMA / plf_hive / small_vector / magic_enum
                                          / allocator2d / stb / beman / gtl
                                          / nanosvg / miniaudio / spirv-reflect
```

拆成四个包而不是一个大包的理由（每条都对应 §3.1 的实测）：

- **可独立验证**：`mo_yanxi_utility` 能单独 `mcpp build`，源码符合性修复就能在最小
  单元上迭代，而不是每次都等 122k LOC 的图。P0 的整个价值就建立在这上面。
- **增量边界真实存在**：改 xrgui 的 GUI 代码不会让 utility 的 26k LOC 重编。
- **将来可发布**：三个包各自可以进 mcpp-index，`import mo_yanxi.math.vector2;` 成为
  别人也能用的东西。
- 代价：三份 manifest 要维护。但每份都只有 15 行左右（见 §5）。

### 4.2 目录布局

```
xrgui/
├── xmake.lua                    ← 不动
├── mcpp.toml                    ← 新增：根包
├── build.mcpp                   ← 新增：资产代码生成（§8）
├── mcpp/
│   ├── pkgs/                    ← 过渡期的三个薄包（§4.3）
│   │   ├── moyanxi-utility/mcpp.toml
│   │   ├── moyanxi-react-flow/mcpp.toml
│   │   └── moyanxi-vulkan-wrapper/mcpp.toml
│   └── index/                   ← 本地包索引（§7）
│       └── pkgs/
│           ├── c/compat.harfbuzz.lua
│           ├── m/compat.msdfgen.lua
│           └── m/compat.mimalloc.lua
├── vendor/                      ← 新增：header-only 第三方（§6）
│   ├── gtl/  nanosvg/  miniaudio/  spirv_reflect/
├── external/                    ← 现有 submodule，不动
└── .agents/docs/                ← 本文档
```

### 4.3 过渡期 → 终态

三个 submodule 的上游是 `Yuria-Shikibe/*`，**本次不向上游提交**。用 §3.1-G 验证过的
"越界 glob"做薄包：

```toml
# mcpp/pkgs/moyanxi-utility/mcpp.toml —— 过渡期形态
[build]
sources = ["../../../external/mo_yanxi_vulkan_wrapper/external/mo_yanxi_utility/src/{utility,latest}/**/*.ixx",
           "!../../../external/.../src/utility/container/open_hash_map.ixx"]
include_dirs = ["../../../external/mo_yanxi_vulkan_wrapper/external/mo_yanxi_utility/include"]
```

**终态**（上游接受 manifest 之后）：manifest 移进各 submodule 根目录，`sources` 改回
`src/**/*.ixx`，根包的 path 指向 `external/<submodule>`。**除了这两行，其余全部不变。**

> 注意 `mo_yanxi_utility` 的物理位置：它是 `mo_yanxi_vulkan_wrapper` 的 submodule，
> 不是 xrgui 的直接 submodule。`react_flow` 通过 xmake 的
> `spec_mo_yanxi_utility_path` 配置指向同一份。mcpp 侧用 path 依赖天然解决这个
> "菱形指向同一份源码"的问题 —— 三个包指向同一个 path，mcpp 只会构建一次。

---

## 5. Manifest 设计

### 5.1 根包 `mcpp.toml`

```toml
[package]
name        = "xrgui"
version     = "0.1.0"
standard    = "c++23"
description = "Retained-mode C++23 GUI library for high-performance desktop rendering"
license     = "BSD-2-Clause"
platforms   = ["linux", "windows"]

[build]
module_extensions = [".ixx"]

sources = [
  "src/**/*.ixx",                       "src/**/*.cpp",
  "src.backends/universal/**/*.ixx",    "src.backends/universal/**/*.cpp",
  "src.backends/vulkan/**/*.ixx",       "src.backends/vulkan/**/*.cpp",
  "src.backends/vulkan_glfw/**/*.ixx",  "src.backends/vulkan_glfw/**/*.cpp",
  "src.backends/miniaudio/**/*.ixx",    "src.backends/miniaudio/**/*.cpp",
  "gui.config/**/*.ixx",                "gui.config/**/*.cpp",
  # 以模块形式直接收编的 vendored 单元
  "external/allocator2d/include/mo_yanxi/allocator2d.ixx",
  "external/magic_enum/module/magic_enum.cppm",
  "vendor/spirv_reflect/spirv_reflect.c",
  # 排除：SIMD 探测是 xmake 侧的诊断件，不是构建单元
  "!src/vectorization_check.cpp",
]

include_dirs = [
  "external/include",                      # stb_image / beman::inplace_vector
  "external/plf_hive",
  "external/small_vector/source/include",
  "external/magic_enum/include",
  "external/mo_yanxi_vulkan_wrapper/external/VulkanMemoryAllocator/include",
  "vendor/gtl/include",
  "vendor/nanosvg/src",
  "vendor/miniaudio",
  "vendor/spirv_reflect",
]

defines = [
  "MO_YANXI_ALLOCATOR_2D_USE_STD_MODULE",
  "MO_YANXI_ALLOCATOR_2D_HAS_MATH_VECTOR2",
  "MO_YANXI_DATA_FLOW_DISABLE_THREAD_CHECK",
  "VK_USE_64_BIT_PTR_DEFINES=1",
]
# ↑ 刻意不定义 XRGUI_FUCK_MSVC_INCLUDE_CPP_HEADER_IN_MODULE：
#   该宏切到 `import <plf_hive.h>;` 头单元路径，GCC/Clang 都不该走。
#   xmake.lua 的注释也已说明新版 MSVC 不再需要它。

[dependencies]
utility        = { path = "mcpp/pkgs/moyanxi-utility" }
react_flow     = { path = "mcpp/pkgs/moyanxi-react-flow" }
vulkan_wrapper = { path = "mcpp/pkgs/moyanxi-vulkan-wrapper" }

[dependencies.compat]
glfw           = "3.4"
freetype       = "2.13.3"
vulkan         = "1.4.357.0"     # 自动带出 compat.vulkan-headers
vulkan-runtime = "2026.07.29"    # Linux ICD 符号农场，见 §9.2
harfbuzz       = "10.1.0"        # 本地索引，§7（版本待按实际打包确定）
msdfgen        = "1.12"          # 本地索引，§7（版本待按实际打包确定）

[dependencies.marzer]
tomlplusplus = "3.4.0"

# ── 目标 ───────────────────────────────────────────────
[targets.xrgui_hello]
kind = "bin"
main = "src.hello/main.cpp"

[targets.xrgui_example]
kind              = "bin"
main              = "src.examples/main.cpp"
required_features = ["examples"]

[targets.xrgui_tests]                        # 见 §5.3，不走 mcpp test
kind              = "bin"
main              = "src.tests/main.cpp"
required_features = ["tests"]

[features]
default  = []
# 伴生源只在对应 feature 激活时进入构建，避免被链进 hello
examples = { sources = ["src.examples/**/*.ixx", "src.examples/**/*.cpp",
                        "!src.examples/main.cpp"] }
tests    = { sources = ["src.tests/**/*.cpp", "!src.tests/main.cpp"] }
mimalloc = { defines = ["XRGUI_USE_MIMALLOC"] }

# feature 未激活时这些依赖完全不解析、不下载
[feature-deps.tests]
compat.gtest = "1.17.0"

[feature-deps.mimalloc]
compat.mimalloc = "2.1.7"        # 本地索引（版本待定）

# ── 平台条件 ───────────────────────────────────────────
[target.linux.build]
ldflags = ["-lfontconfig"]

[target.windows.build]
ldflags = ["-ladvapi32", "-limm32", "-lole32", "-lshell32", "-luser32"]

[toolchain]
default = "gcc@16.1.0"
windows = "llvm@20.1.7"
macos   = "llvm@22.1.8"

[profile.release]
opt   = 2
debug = true          # 对齐 xmake 的 set_symbols("debug","embed")

> `[xlings] deps` 刻意**不写**。配合 §8.2 的产物入仓，构建期不需要任何 host 工具；
> `python`/`node`/`slangc` 只在**再生成** shader/icon 时用得上，那是离线动作，
> 不该进构建图（`slang` 的分发方案见 §8.2）。
```

`compat.freetype` / `compat.gtest` / `compat.vulkan*` / `marzer.tomlplusplus` / `compat.glfw`
的版本号已按本机索引快照核对。`compat.harfbuzz` / `msdfgen` / `mimalloc` 是本地索引待建包，
版本以实际打包为准。

**几处需要 review 的取舍：**

1. `standard = "c++23"` 而不是 xmake 的 `c++latest` —— **已确认采纳**。理由：`c++latest`
   在 mcpp 里是"跟随最新"，跨编译器语义不稳定；钉死 `c++23` 拿到可复现基线。
   **如果源码里已有 C++26 依赖（如 `std::execution`），P0 首编会暴露**，届时提档是一行改动
   （代价是作废全部 BMI 缓存）。
2. `mimalloc` 做成 feature 而非硬依赖。`src/gui/core/gui.alloc.ixx` 无条件
   `#include <mimalloc.h>`，需要配合一个 `__has_include` 保护（一行改动），
   这样 P0 不必先把 mimalloc 打进本地索引。
3. `simdutf` **不进 manifest**：`src/util/unicode.ixx` 已经用 `#if __has_include(<simdutf.h>)`
   保护，缺失时走标量回退路径。零成本。
4. VMA 的 include 路径写的是 `external/mo_yanxi_vulkan_wrapper/external/VulkanMemoryAllocator/include`
   而不是 xmake.lua 第 92 行的 `./external/VulkanMemoryAllocator/include` —— 后者在本仓库
   **不存在**（VMA 是 vkw 的 submodule）。xmake 侧靠 vkw 的 public includedir 传播才没露馅，
   顺手在这里修正。

### 5.2 三个薄包（形态一致，此处示一）

```toml
# mcpp/pkgs/moyanxi-vulkan-wrapper/mcpp.toml
[package]
name     = "vulkan_wrapper"
version  = "0.1.0"
standard = "c++23"

[build]
module_extensions = [".ixx"]
sources = ["../../../external/mo_yanxi_vulkan_wrapper/src/vk_wrap/**/*.ixx",
           "../../../external/mo_yanxi_vulkan_wrapper/src/vk_wrap/**/*.cpp"]
include_dirs = ["../../../external/mo_yanxi_vulkan_wrapper/external/VulkanMemoryAllocator/include"]
defines = ["VK_USE_64_BIT_PTR_DEFINES=1", "MO_YANXI_VULKAN_WRAPPER_ENABLE_CHECK=0"]

[dependencies]
utility = { path = "../moyanxi-utility" }

[dependencies.compat]
vulkan = "1.4.357.0"

[targets.vulkan_wrapper]
kind = "lib"
```

> 注意上游 xmake 里 `add_links("vulkan-1")` 是 **Windows 库名**，Linux 上是 `vulkan`。
> mcpp 侧改由 `compat.vulkan` 提供，链接名由包描述符负责，这个跨平台坑自动消失。

### 5.3 测试目标 —— 决议：feature 门控的 bin target

**前提事实（已核实）**：`mcpp test` 的发现路径是**硬编码**的 ——
`src/build/test_targets.cppm:38` 写死 `expand_glob(packageRoot, "tests/**/*.cpp")`，
manifest 中不存在任何测试目录配置键。

三条路都评估过：

| 方案 | 能否用 `mcpp test` | 需重命名 | 碰 xmake.lua | 结论 |
|---|:--:|:--:|:--:|---|
| (a) `src.tests/` → `tests/` | ✅ | 是 | 一行 glob | 唯一能拿到原生 `mcpp test` 的路 |
| (b) workspace member | ✅ | 是（member 内仍须叫 `tests/`） | 否 | **不成立**：member 只是换了包根，硬编码 glob 依旧；且越界 glob 对测试发现无效 |
| (c) **feature 门控的 `bin` target** | ❌ | **否** | **否** | ✅ **采纳** |

> (b) 之所以不成立值得记一笔：workspace 并没有提供"配置测试路径"的能力，
> 它只是把 `packageRoot` 换成 member 目录。要让 `mcpp test` 发现文件，
> member 里必须有真实的 `tests/` 目录 —— 越界 glob 在这条路径上帮不上忙，
> 因为 glob 模式本身是硬编码的，只有 root 可变。

**采纳方案 (c)，已端到端实测通过：**

```toml
[targets.xrgui_tests]
kind              = "bin"
main              = "src.tests/main.cpp"
required_features = ["tests"]

[features]
tests = { sources = ["src.tests/**/*.cpp", "!src.tests/main.cpp"] }

[feature-deps.tests]
compat.gtest = "1.17.0"      # feature 未激活时完全不解析、不下载
```

实测结果：

- `mcpp build` —— 只产出 `app`，**gtest 根本不下载也不解析**，测试源不进构建图
- `mcpp build --features tests` —— 拉起 gtest，产出 `xrgui_tests`，二进制运行正常
  （`[  PASSED  ] 1 test.`）

这是"零重命名 + 零碰 xmake.lua + 跨平台"的解，把 §13 待决第 1 项直接消掉。

**已知代价（实测）**：`mcpp run <target>` **不接受** `--features`
（报 `error: unknown option: --features`）。所以跑测试是两步：

```bash
mcpp build --features tests
./target/<triple>/<fp>/bin/xrgui_tests            # 或 mcpp run xrgui_tests
```

放弃的是 `mcpp test` 的 pattern 过滤 / `--list` / 单测隔离 / `--message-format json`。
对一个 gtest 二进制来说，这些能力 gtest 自己用 `--gtest_filter` / `--gtest_list_tests`
基本都有，损失可接受。若将来想要原生 `mcpp test`，方案 (a) 随时可以做，
且与本方案不冲突。

---

## 6. 第三方依赖处置矩阵

| 库 | 形态 | 索引状态 | 处置 | 阶段 |
|---|---|---|---|:--:|
| VulkanMemoryAllocator | header-only | — | vendor（已是 vkw 的 submodule）→ `include_dirs` | P0 |
| plf_hive | header-only | — | vendor（已是 submodule）→ `include_dirs` | P0 |
| small_vector | header-only | — | vendor（已是 submodule）→ `include_dirs` | P0 |
| beman::inplace_vector | header | — | vendor（已在 `external/include`） | P0 |
| stb_image / stb_image_write | header | — | vendor（已在 `external/include`） | P0 |
| magic_enum | C++ 模块 | `neargye.magic_enum@0.9.8` ✅ | vendor（submodule 已有 `module/magic_enum.cppm`，直接进 `sources`） | P0 |
| allocator2d | C++ 模块 | — | vendor（`.ixx` 直接进 `sources`） | P0 |
| gtl (greg7mdp) | header-only | ❌ | **新增 vendor**（submodule 或 `vendor/gtl`） | P0 |
| nanosvg | header-only | ❌ | **新增 vendor** | P1 |
| miniaudio | 单头 | ❌ | **新增 vendor** | P1 |
| spirv-reflect | 1×`.c` + `.h` | ❌ | **新增 vendor**，`.c` 进 `sources`（mcpp 原生支持 C） | P1 |
| toml++ | header-only/模块 | `marzer.tomlplusplus@3.4.0` ✅ | 索引依赖 | P0 |
| glfw | 编译库 | `compat.glfw@3.4` ✅ | 索引依赖（Linux = X11，自带 X 栈 10 个包） | P1 |
| freetype | 编译库 | `compat.freetype@2.13.3` ✅ | 索引依赖（自带 libpng） | P1 |
| Vulkan loader | 编译库 | `compat.vulkan@1.4.357.0` ✅ | 索引依赖 | P1 |
| Vulkan ICD | 主机能力 | `compat.vulkan-runtime` ✅ | 索引依赖，见 §9.2 | P2 |
| gtest | 编译库 | `compat.gtest@1.17.0` ✅ | `[feature-deps.tests]`（见 §5.3） | P1 |
| **harfbuzz** | 编译库(meson) | ❌（`xim:harfbuzz` 是系统级 xlings 包，非 mcpp 源码包） | **本地索引 `compat.harfbuzz`** | P1 |
| **msdfgen** | 编译库(cmake，依赖 freetype) | ❌ | **本地索引 `compat.msdfgen`** | P1 |
| **mimalloc** | 编译库(cmake，C) | ❌ | **本地索引 `compat.mimalloc`**，且做成 feature | P1 |
| simdutf | 可选 | ❌ | **不引入**（`__has_include` 已保护） | — |
| fontconfig | 系统库(Linux) | ❌ | `[target.linux.build] ldflags = ["-lfontconfig"]` | P2 |

**三个"重库"的难度排序**（决定 P1 内部顺序）：

1. `mimalloc` —— 纯 C，源码文件表小而稳定，写 xpkg 描述符最简单。且已被 feature 隔离，可延后。
2. `msdfgen` —— C++，核心源码不多，`core/` + `ext/`；xmake 侧开了 `openmp` + `extensions`
   两个 config，其中 `extensions` 需要 freetype + 可选 skia。**建议只做 `core` + freetype 后端，
   关掉 openmp 和 skia**，先满足 `src/graphic/msdf.*` 的实际用法。
3. `harfbuzz` —— 最麻烦。上游是 meson，但它有官方的 "amalgamated" 单文件构建
   （`src/harfbuzz.cc`），且 xmake 侧已经在用 `-DHB_NO_MT`。
   **建议走 amalgam 路线**：一个 `.cc` + `include_dirs`，绕开 meson 全部配置逻辑。

---

## 7. 本地包索引方案

mcpp 支持项目级自定义索引，用 `path` 指向本地目录：

```toml
# 根 mcpp.toml
[indices]
xrgui-local = { path = "mcpp/index" }
```

索引目录就是一棵 xpkg 描述符树（与 mcpp-index 同构）：

```
mcpp/index/pkgs/
├── c/compat.harfbuzz.lua
├── m/compat.msdfgen.lua
└── m/compat.mimalloc.lua
```

描述符骨架（以 harfbuzz amalgam 为例）：

```lua
package = {
    spec = "1", namespace = "compat", name = "harfbuzz",
    licenses = {"MIT"}, repo = "https://github.com/harfbuzz/harfbuzz",
    type = "package",
    xpm = { linux = { ["10.1.0"] = { url = "...harfbuzz-10.1.0.tar.xz", sha256 = "..." } },
            windows = { --[[ 同 ]] }, macosx = { --[[ 同 ]] } },
    mcpp = {
        language = "c++17", import_std = false,
        include_dirs = { "*/src" },
        sources      = { "*/src/harfbuzz.cc" },
        cxxflags     = { "-DHB_NO_MT", "-DHAVE_FREETYPE=1" },
        targets      = { ["harfbuzz"] = { kind = "lib" } },
        deps         = { ["compat.freetype"] = "2.13.3" },
    },
}
```

**上游化路径**：三个描述符验证通过后，逐个向 `mcpp-community/mcpp-index` 提 PR；
提上去之后根 manifest 只需删掉 `[indices]` 那一节，依赖声明**一字不改**
（身份 `compat.harfbuzz` 在两处相同）。这是选本地索引而非 vendor 源码的核心理由。

`mcpp index pin` 可以把本地索引钉到 commit，保证 CI 可复现。

---

## 8. 资产与代码生成管线

### 8.1 分两层，不要混

| 层 | 内容 | 触发时机 | 依赖工具 |
|---|---|---|---|
| **离线资产生成** | Slang → SPIR-V；原始 SVG → 归一化 SVG | 人工/CI，改 shader 或图标时 | `slangc`、`python`、`node` |
| **构建期代码生成** | 归一化 SVG → bin2c 头；聚合 `assets_summary.h` | 每次 `mcpp build` | 无（`build.mcpp` 自己做） |

xmake 现在把第一层做成两个 `task`（`xrgui.gen_slang` / `xrgui.gen_icon`），第二层做成
`rule` + `before_build`。

### 8.2 slang 工具链 —— 决议：做 `xim:slang` 包

**xmake 现在是怎么处理的（已核实）：完全不处理。**

- `task("xrgui.gen_slang")` 的实现就是 `os.execv("py", {slang_builder.py, compiler, out, config, "-j", "30"})`
- `--compiler` 的默认值是 `./slang/bin/slangc.exe` —— **一个硬编码的 Windows 相对路径**
- CI 里靠 workflow 手动兜：打 GitHub API 取 latest tag → 下
  `slang-<ver>-windows-x86_64.zip` → 解压到 `C:\slang` → 写进 `GITHUB_PATH`

也就是说 **slangc 是纯外部前置**，xmake 侧没有任何依赖声明或版本管理。
xim / mcpp 索引里目前也都没有 slang（只有 `xim:glslang`）。

**结论：做 `xim:slang` 包是纯增量收益，且成本很低。** 上游有现成的预编译 tarball：

```
slang-2026.14.1-linux-x86_64.tar.gz          # 还有 glibc-2.27 / glibc-2.28 变体
slang-2026.14.1-linux-aarch64.tar.gz
slang-2026.14.1-macos-aarch64.tar.gz
slang-2026.14.1-windows-x86_64.zip
```

四平台齐全，包的实现就是"下载 + 解压 + 暴露 `bin/slangc`"，没有任何构建逻辑。
之后 shader 再生成的前置就一条命令：

```bash
xlings install slang -y      # 四平台统一，取代 CI 里那段 GitHub API + unzip 脚本
```

**刻意不写进 `[xlings] deps`。** 配合 §8.2.1 的产物入仓，slangc 不是构建期依赖，
只是**再生成**时才需要的工具 —— 把它放进构建图会让每个只想 `mcpp build` 的人
白白拉一份 slang。声明为文档化的前置即可。

`xrgui.gen_slang` / `gen_icon` 本身不必改 —— 它们调的是 PATH 上的 `slangc`。
（xmake 侧 `--compiler` 的默认值仍是硬编码的 `./slang/bin/slangc.exe`，
建议顺手改成裸 `slangc`；这属于 xmake 自己的可移植性修复，不算 mcpp 适配的一部分。）

### 8.2.1 仍然建议把产物入仓

`xim:slang` 解决的是"能不能生成"，不解决"要不要每次生成"。当前
`properties/assets/shader/spv/` 与 `properties/assets_raw/gen/icons/` **都是空的**，
CI 每次现生成。建议把 spv 和归一化后的 SVG 提交进仓库：

- 首次构建（尤其新贡献者和 Linux）不需要任何 host 工具，`mcpp build` 直接可跑
- 这两类产物确定性、体积小、变更频率极低
- 再生成入口保留，语义从"每次构建"变成"改了 shader 才跑"

两件事互不冲突，建议都做：入仓保证**日常构建零工具依赖**，`xim:slang` 保证
**需要改 shader 时四平台都能改**。

### 8.3 `build.mcpp` 设计

```cpp
// build.mcpp —— 只做第二层
import mcpp;
import std;

int main() {
    const std::filesystem::path root = mcpp::manifest_dir();
    const std::filesystem::path out  = std::filesystem::path{mcpp::out_dir()} / ".assets/includes";
    const std::filesystem::path src  = root / "properties/assets_raw/gen";

    std::filesystem::create_directories(out);

    std::vector<std::filesystem::path> svgs;      // 递归收集 src 下的 *.svg
    // …对每个 svg：若 out/<rel>.h 不存在或更旧，做 bin2c 写出…
    // …聚合生成 out/assets_summary.h（与 xmake before_build 的内容逐字节一致）…

    mcpp::include_dir(out.string());                       // 只染色本包 TU
    mcpp::rerun_if_changed_glob("properties/assets_raw/gen/**/*.svg");
    return 0;
}
```

三点说明：

- bin2c 是十几行 C++，不需要外部工具 —— 比 xmake 侧调用 `utils.binary.bin2c` 还简单。
- `mcpp::include_dir()` 是**私有**的（不传播给消费者），语义正是我们要的。
- `rerun_if_changed_glob` 让"新增一个图标"能正确触发重跑，而不是靠 mtime 巧合。

### 8.4 assets 运行期定位

xmake 用 `after_build` 把 `properties/assets` 和 `vk_layer_settings.txt` 拷到产物目录。
mcpp 没有 `after_build` 钩子。三个选项，按推荐度排序：

1. **依赖 `mcpp run` 的 CWD 语义**（P2 最省事，**已实测**：CWD = 包根，
   `ifstream("properties/probe.txt")` 直接读到）：`properties/assets` 的相对路径
   天然可达，开发期零成本。
2. **`[runtime] deploy_files`**：mcpp 定义为"复制到产物旁，绝不成为 linker flag"，
   语义完全对口。⚠️ **需实测**该字段用在**根包**上是否生效（文档场景写的是 provider 包）。
3. **`mcpp::action{ role = "object" }` 声明拷贝节点**：最通用但最重，留作兜底。

`mcpp pack` 分发时必须走 2 或 3。

---

## 9. Linux 配置方案

### 9.1 工具链与标准库

- 工具链：`gcc@16.1.0`（mcpp Linux x86_64 默认，本机已装）。
- `import std` 由 mcpp 自动预编译并按指纹缓存，无需任何配置。
- `cxx_runtime`：用默认的 `self-contained`（`-static-libstdc++`），产物可直接分发。

### 9.2 图形栈 —— 这是 Linux 上唯一的非平凡部分

mcpp 构建的二进制运行在**它自己的 glibc/loader 沙箱**里，
所以主机的 Vulkan ICD（`libvulkan_radeon.so` 等）默认 `dlopen` 不到。
mcpp-index 已经用 `compat.vulkan-runtime` 解决了这个问题 —— 它是一个
**符号农场**（symlink farm）：把主机的 ICD 及其传递依赖软链到一个包内目录，
挂上 `runtime.library_dirs`。

因此 Linux 图形栈的正确声明是**三件套**：

```toml
[dependencies.compat]
vulkan         = "1.4.357.0"    # Khronos loader，自动带出 vulkan-headers
vulkan-runtime = "2026.07.29"   # 主机 ICD 符号农场
glfw           = "3.4"          # X11 后端，自动带出 glx-runtime + 10 个 X 包
```

三点必须知道的性质：

- `compat.glfw` 在 Linux 上编译期只开 `_GLFW_X11`。**Wayland 会话下走 XWayland**，
  能跑，但不是原生。这是当前索引的既成事实，不是本设计的选择。
- `compat.vulkan-runtime` 在**没有任何 Vulkan 驱动的机器上也不报错**（农场为空，
  loader 只报自己的扩展）。所以 CI 上可以安全地构建 + 跑非图形测试。
- `xrgui` 自己**不要**去链 `vulkan-1` / 找 `VULKAN_SDK`。xmake 侧那套
  `os.getenv("VULKAN_SDK")` 逻辑在 mcpp 侧完全被包依赖取代。

### 9.3 字体

`src/platform/font.ixx` 的 Linux 分支用 fontconfig，是**主机系统库**：

```toml
[target.linux.build]
ldflags = ["-lfontconfig"]
```

⚠️ **需验证**：mcpp 沙箱 loader 下能否解析主机的 `libfontconfig.so`。如果撞上与
Vulkan ICD 同类的问题，两条出路：(a) `[xlings] deps = ["fontconfig"]`
如果 xim 有该包；(b) 给本地索引加一个 `compat.fontconfig` 源码包
（依赖 freetype + expat，都可解）。**这是 P2 的首要待验证项。**

### 9.4 Linux 上的功能缺口

| 功能 | Linux 现状 | P2 处理 |
|---|---|---|
| 系统字体枚举 | ✅ fontconfig 已实现 | 无 |
| 文件对话框书签 | ✅ GTK bookmarks 已实现 | 无 |
| 线程优先级/命名 | ✅ pthread 已实现 | 无 |
| 虚拟内存 | ✅ mmap 已实现 | 无 |
| 原生窗口句柄 | ⚠️ 返回空 | 用 `glfwCreateWindowSurface`，不需要裸句柄 |
| IME 输入法 | ⛔ 仅 Windows IMM | 降级为 GLFW `character callback`；完整 IBus/fcitx 支持另开议题 |

---

## 10. 多平台矩阵

`[toolchain]` 三行覆盖三平台，`[target.<os>.build]` 承载差异：

| 平台 | 工具链 | 状态 | 备注 |
|---|---|---|---|
| `x86_64-linux-gnu` | gcc@16.1.0 | **P0–P2 主战场** | 本设计的验证平台 |
| `x86_64-windows-msvc` | llvm@20.1.7 | P3 | mcpp 在 Windows 上用 clang 打 MSVC ABI，需已装 MSVC BuildTools |
| `x86_64-windows-gnu` | gcc@16.1.0 MinGW | P3 | 无 VS 时的自包含路径；但 Vulkan/GLFW 的 MinGW 链接需另测 |
| `aarch64-macos` | llvm@22.1.8 | 未定 | `src/platform/*` 有 `__APPLE__` 分支但未验证；Vulkan 需 MoltenVK，索引暂无 |

**Windows 侧的关键差异**：`[target.windows.build] ldflags` 要补 xmake 里那 5 个
`add_syslinks`。macOS 侧的 MoltenVK 是硬缺口，本设计不承诺。

---

## 11. 源码兼容性工作项

来自 §3.2 的实测。**全部直接改主干，不加 `#ifdef` 分叉**（三种修法在 MSVC 上语义等价）。

| # | 类别 | 定位方式 | 预估规模 |
|---|---|---|---:|
| S1 | 匿名 lambda 当类型（暴露 TU-local） | `grep -rn 'decltype(\[\]' --include='*.ixx'` | 4 处（已定位） |
| S2 | `static` 函数被模块链接实体引用 | 只能靠编译器报错逐个暴露 | 预估 5–15 处 |
| S3 | `export` 了 `std::` 模板特化 | `grep -rnB2 'struct std::' --include='*.ixx'` | 待统计 |
| S4 | `gui.alloc.ixx` 无条件 `#include <mimalloc.h>` | 已定位 | 1 处，加 `__has_include` |
| S5 | `#include <spirv_reflect.h>;` 尾随分号 | 已定位 2 处 | GCC 仅告警，可选修 |
| S6 | `-Winterference-size` 告警（`hardware_destructive_interference_size`） | 已出现在 utility | 加 `-Wno-interference-size` 或钉常量 |

> S1–S3 是**阻塞级**，S4 是 P0 前置，S5–S6 是噪音。

---

## 12. 分阶段路线图

### P0 — 骨架打通，最小可编译单元先绿

**范围**：三个薄包 manifest + 根包 manifest 骨架 + S1/S2/S3 修复。
除 `compat.vulkan-headers`（vulkan_wrapper 编译必需的纯头包）外，**不引入任何编译型第三方**。

1. `git submodule update --init --recursive`
2. 建 `mcpp/pkgs/moyanxi-utility/mcpp.toml`，`mcpp build` 直到绿 —— 迭代修 S1/S2/S3
3. 同法打通 `moyanxi-react-flow`（依赖 utility）
4. 同法打通 `moyanxi-vulkan-wrapper`（依赖 utility + `compat.vulkan`）
5. 根包 manifest 只收 `src/util/**`、`src/i18n/**`、`src/log/**` 等无重第三方依赖的子集

**验收**：`cd mcpp/pkgs/moyanxi-utility && mcpp build` 全绿；三个薄包各自绿；
根包子集绿。**这一阶段结束时，S1/S2/S3 的真实规模就从"预估"变成"已知"。**

### P1 — 第三方补齐

1. vendor：`gtl` / `nanosvg` / `miniaudio` / `spirv-reflect` 落到 `vendor/`
2. 索引依赖接上：`glfw` / `freetype` / `tomlplusplus` / `vulkan`
3. 本地索引：`compat.mimalloc` → `compat.msdfgen` → `compat.harfbuzz`（难度递增）
4. `build.mcpp` 实现 bin2c + `assets_summary.h`
5. 根包 `sources` 放开到全量，`[targets.xrgui_hello]` 首次链接成功
6. 测试目标落位 —— feature 门控的 `xrgui_tests`（§5.3，方案已实测）

**验收**：`mcpp build` 产出 `xrgui_hello`；`mcpp build --features tests` 产出
`xrgui_tests` 且测试全绿。

### P2 — Linux 真跑起来

1. `compat.vulkan-runtime` 接入，验证 ICD 可达
2. fontconfig 链接与 loader 可达性验证（§9.3 的待验证项）
3. 资产入仓（§8.2.1）+ `xim:slang` 包（§8.2）—— 两者并行，互不阻塞
4. assets 运行期定位（§8.4）
5. IME/窗口句柄降级实现
6. `xrgui_hello` 在 Linux X11 上出窗口

**验收**：`mcpp run xrgui_hello` 在 Linux 上渲染出界面。

### P3 —（可选）Windows / macOS 与上游化

- mcpp 侧 Windows 构建验证
- 三份 manifest 向 `Yuria-Shikibe/*` 上游化，薄包退役
- 三个 `compat.*` 描述符向 `mcpp-index` 上游化，本地索引退役

---

## 13. 风险与未决问题

| # | 风险 | 影响 | 缓解 |
|---|---|:--:|---|
| R1 | **S2 类错误的真实规模只有 P0 才知道** | 高 | P0 的设计目的就是把它变成已知；从 26k LOC 的 utility 开始，实测命中率 ~0.5% |
| R2 | `standard = "c++23"` 可能不够 —— 源码若用了 C++26 设施则要提档 | 中 | P0 首编即暴露；提到 `c++26` 是一行改动，但会作废全部 BMI 缓存 |
| R3 | harfbuzz amalgam 路线可能撞上 harfbuzz 的配置宏 | 中 | 退路：改用 `xim:harfbuzz` 系统包 + `[xlings] deps`，牺牲自包含性 |
| R4 | fontconfig 在 mcpp 沙箱 loader 下不可达 | 中 | §9.3 已列两条出路；P2 首要验证项 |
| R5 | `[runtime] deploy_files` 在根包上可能不生效 | 低 | §8.4 有两条备选；开发期靠 `mcpp run` 的 CWD 语义即可 |
| R6 | `compat.glfw` 是 X11-only，Wayland 原生缺失 | 低 | XWayland 可用；原生 Wayland 另开议题 |
| R7 | 薄包的 `../../../` 越界 glob 可读性差 | 低 | 是过渡形态，上游化后消失（§4.3） |

### 已决议（2026-08-12 review）

1. ~~测试布局~~ → **§5.3 方案 (c)**：feature 门控的 bin target。零重命名、零碰 xmake，已实测。
2. ~~slang 分发~~ → **§8.2**：做 `xim:slang` 包（上游有四平台预编译 tarball，成本很低），
   同时 §8.2.1 建议产物入仓。两者并行。
3. ~~C++ 标准~~ → **`c++23`**。
4. **msdfgen 裁剪** — 仍待验证：只做 `core` + freetype 后端、关掉 openmp/skia，
   是否满足 `src/graphic/msdf.*` 与 `src/font/adaptor.ixx` 的实际调用面？
   放到 P1 做本地索引包时实测确认。

---

## 14. 实施进展（2026-08-12）

设计写完当天开始实施。本节记录**实际发生的事**，与前文的预估对照。

### 14.1 结果一览

| 包 | 状态 | 源码修改 |
|---|:--:|---:|
| `mo_yanxi_utility`（26k LOC / 63 模块） | ✅ **全绿** | 6 处 |
| `mo_yanxi_vulkan_wrapper`（9k LOC / 33 模块） | ✅ **全绿** | 5 处 |
| `mo_yanxi_react_flow`（3k LOC / 11 模块） | ⛔ **GCC 16 bug 阻塞** | 0 |
| `xrgui` 根包 | 🔄 源码问题清零，卡在三个第三方库 | 21 个文件 |

`mcpp.toml` × 4（根包 + 三个薄包）已落地，依赖图完整解析：
`compat.glfw` / `compat.freetype` / `compat.vulkan(+headers,+runtime)` /
`marzer.tomlplusplus` / `neargye.magic_enum` 全部下载并编译成功。

### 14.2 预估 vs 实际

| 类别 | §11 预估 | 实际 | 差异原因 |
|---|---|---|---|
| S1 匿名 lambda 暴露 TU-local | 4 处 | **3 处** | 静态扫描包含了 `legacy/`（不参与构建） |
| S2 `static` 被模块链接实体引用 | 5–15 处 | **1 处** | 命中率比外推的还低；命名空间级 `static` 绝大多数是纯文件内使用 |
| S3 `export` 了 std 模板特化 | 待统计 | **4 处** | utility×2 / vkw×1 / xrgui×1 |
| S4 mimalloc 无条件 include | 1 处 | 1 处（**未修**，见 14.4） | — |
| S5/S6 噪音 | — | 用 `-Wno-interference-size` 一行解决 | — |

**S2 的预估偏保守是件好事**：它是三类里唯一无法静态定位、只能靠编译器逐个暴露的，
预估 5–15 处意味着「要迭代很多轮」，实际 1 处意味着这条路比看起来短得多。

### 14.3 设计阶段没预见到的四类问题

这些是实施才暴露的，都不在 §11 的清单里：

#### ④ GCC 16 模块 bug：`recursive lazy load`（⛔ 未解决）

```
node.spec.ixx:23:24: error: recursive lazy load [-Wtemplate-body]
node.spec.ixx:23:24: fatal error: failed to load pendings for 'std::vector'
```

**已定位到触发点**：`mo_yanxi.react_flow:manager` 分区在导出的类里声明了
`std::vector<...>` 成员（`nodes_anonymous_` / `pulse_subscriber_` /
`linear_flat_set<std::vector<node*>>`）。任何**同时** `import :manager` 且随后
惰性需要 `std::vector` 的分区都会递归。诊断实验：把 `import :manager;` 从
`node.spec.ixx` 去掉，该 TU 立刻编过。

试过并**全部无效**的 7 种规避：

| # | 尝试 | 结果 |
|---|---|---|
| 1 | 把 `import std;` 提到分区 import 之前 | 无效 |
| 2 | 在使用点前强制 `sizeof(std::vector<int>)` 触碰 | 无效（错误移到该行） |
| 3 | GMF 里 `#include <vector>` | 无效 |
| 4 | `-fno-module-lazy` | 无效（错误移到 `import :manager;` 那行，反证了根因） |
| 5 | 给 `manager.ixx` 补 `import std;`（干净重建） | 无效 |
| 6 | `--param=lazy-modules=1000` | 无效 |
| 7 | 降级 gcc@15.1.0 | **更糟** —— gcc 15 连 `import std` 都预编译不出来 |

**结论：降级不是出路，需要上游 GCC 修复或 react_flow 的分区结构重构。**
这是本次适配唯一一个真正的死结，应该向 GCC 提 bug。

#### ⑤ mcpp 扫描器禁止条件 import 与头单元

```
error: import statement inside conditional preprocessor block (forbidden in M1)
error: header units (import "h" / import <h>) are forbidden in M1
```

命中 **17 处**，几乎全部是 `#ifdef XRGUI_FUCK_MSVC_INCLUDE_CPP_HEADER_IN_MODULE`
里的 `import <plf_hive.h>;` / `import <gtl/phmap.hpp>;` / `import <spirv_reflect.h>;`。

**这不是取舍，是被迫的**：mcpp 同时禁止条件 import *和*头单元，所以 MSVC 的头单元
路径在 mcpp 下没有任何保留余地。已用脚本统一把 `#ifndef` 分支的 `#include` 改为
无条件，并删掉 `#ifdef` 的 import 分支（16 个文件）。

⚠️ **这是唯一一处影响 MSVC 行为的改动**：xmake 仍然定义那个宏，但现在没有任何代码
引用它，MSVC 会走 `#include` 而不是头单元。`xmake.lua:102` 的注释本来就写着
「msvc 新版好像没这问题了，哪天删了」—— 这次等于替它删了。**需要在 Windows 上复验。**

另外两处同类：`magic_enum` 的 `#ifdef MAGIC_ENUM_USE_STD_MODULE / import std;`
和 vkw 的 `stack_trace.cpp`。前者改用索引包 `neargye.magic_enum@0.9.8`（它用
`scan_overrides` 正好绕过），后者把 import 提到 `#if` 外。

#### ⑥ `-Wchanges-meaning`：成员名与先前用过的类型同名

```
error: declaration of '...::image_view ...::combined_image_type<ImageProv>::image_view'
       changes meaning of 'image_view' [-Wchanges-meaning]
```

MSVC 放行，GCC 报错（[basic.scope.class]）。命中 5 处：`image_view image_view{}`、
`const bitmap& bitmap`、`tag tag` ×3。修法是把**类型**写成限定名
（`vk::image_view` / `mo_yanxi::bitmap` / `binary_diff_trace::tag`），
于是非限定名不再被用来指代类型，同名成员就不改变其含义。

#### ⑦ 零散的标准符合性

- `std::exception(const char*)` 是 **MSVC 扩展**，libstdc++ 没有。vkw 的三个异常类
  改派生自 `std::runtime_error`，顺带修掉 `string_view::data()` 未必 NUL 结尾的隐患。
- 裸 `size_t`（4 处）→ `std::size_t`。`import std;` 下 `::size_t` 不保证可见。
- `import :call_stream_buffer;` → `export import`：接口分区必须由主接口单元导出。
- `module : private;`（1 处，**归因已更正**）：报错来自 **GCC 而非 mcpp**。用 5 行
  文件逐字复现 mcpp 的扫描命令后可见，GCC 16 **根本没实现**私有模块片段：编译路径
  给的是清楚的 `sorry, unimplemented: private module fragment`，而 P1689 扫描路径
  （`-fdeps-format=p1689r5 … -E`）对同一件事给的是误导性的 `module already declared`。
  mcpp 只是透传。

  **而且这处用法本身就不合规**：private module fragment 是 C++20 特性（[module.unit]），
  但「带私有片段的模块单元必须是该模块唯一的模块单元」。`mo_yanxi.font` 有两个 ——
  `font.ixx`（主接口）与 `font.cpp`（`module mo_yanxi.font;` 实现单元）—— 所以这是
  IFNDR。删掉该行修的是真问题，不是绕过工具。

### 14.4 与设计的偏差

| 项 | 设计怎么写 | 实际怎么做 | 理由 |
|---|---|---|---|
| magic_enum | vendor（submodule 已有 .cppm） | **改用索引包** `neargye.magic_enum@0.9.8` | 上游 .cppm 有条件 import，索引包用 `scan_overrides` 已解决 |
| mimalloc | 做成 feature + `__has_include` 保护 | **暂未做** | 优先把无条件缺口收敛完；`gui.alloc.ixx` 仍无条件 include |
| nanosvg | vendor | vendor + `[generated_files]` 造转发头 | 上游布局是 `src/nanosvg.h`，代码写 `<nanosvg/nanosvg.h>` |
| 子模块 manifest | 各 submodule 自带 | 薄包 + 越界 glob（§4.3 过渡形态） | 上游是第三方仓库，本次不提交 |
| 子模块源码修复 | 未讨论 | **导出为 `mcpp/patches/*.patch` + `apply.sh`** | 跨仓库改动无法随 xrgui 提交 |

### 14.5 下一步

1. **本地索引三个包**：`compat.mimalloc` → `compat.msdfgen` → `compat.harfbuzz`
   （难度递增）。这是根包链接成功前唯一剩下的工作。
2. **react_flow**：向 GCC 提 bug；同时评估把 6 个分区拆成独立命名模块的代价。
3. **上游化**：三份源码补丁向 `Yuria-Shikibe/*` 提 PR（全是可移植性修复，对 xmake
   侧同样是净收益）；扫描器的 M1 限制（条件 import / 头单元 / 私有片段诊断透传）
   向 mcpp 提 issue。
4. **Windows 复验**：确认删掉头单元路径后 MSVC 构建仍然正常。

---

## 15. 第二轮实施（三个重库进索引之后）

`compat.harfbuzz` / `compat.msdfgen` / `compat.mimalloc` 已合入 mcpp-index
（[mcpplibs/mcpp-index#206](https://github.com/mcpplibs/mcpp-index/pull/206)，CI 全绿）。
接上之后，根包越过了此前被缺库掩盖的一整层，**又暴露出 13 处源码问题**。

### 15.1 结果

```
mcpp build →  1332 个对象 / 626 个 BMI
              唯一失败：react_flow 的 :endpoint 与 :modifier 两个分区
```

**除 react_flow 外，源码符合性问题已全部清零。**

### 15.2 依赖表改成点式 `ns.name`

原先按命名空间拆成 `[dependencies]` + `[dependencies.compat]` +
`[dependencies.neargye]` + `[dependencies.marzer]` 四段。现在合成一张表：

```toml
[dependencies]
utility               = { path = "mcpp/pkgs/moyanxi-utility" }
compat.glfw           = "3.4"
compat.harfbuzz       = "14.3.0"
marzer.tomlplusplus   = "3.4.0"
neargye.magic_enum    = "0.9.8"
```

理由很简单：拆表把 12 条依赖散成 4 段，而它们的差别只是命名空间、不是角色，
一眼看不全这个工程到底依赖什么。点式 selector 表达的是同一个精确身份
（`compat.glfw` ⇒ `(compat, glfw)`），语义完全等价。

`[feature-deps.tests]` 里的 `compat.gtest` 保持独立 —— 那是**角色**差异
（feature 未激活时完全不解析），不是命名空间差异。

### 15.3 这一轮修掉的 13 处

| # | 问题 | 位置 | 性质 |
|---|---|---|---|
| 1 | `export` 了 `std::hash` / `std::formatter` 特化 | `color.ixx` ×2 | S3 复现 |
| 2 | 用全局限定名 `struct ::std::hash<X>{…}` 做定义 | `color.ixx` ×2 | GCC 不接受 |
| 3 | 类模板内定义友元函数模板 → 每次实例化都重定义 | `gui.alloc.ixx` ×2 | 真缺陷 |
| 4 | `using` 写成自身注入类名而非基类 | `task_queue.ixx` | 真笔误 |
| 5 | 自指的 requires 约束 | `instruction.general.ixx` | 见下 |
| 6 | 模块声明后经 `#include` 引入 `import` | `allocator2d` | 硬性禁止 |
| 7 | 依赖名缺 `typename` | `key_mapping_manager.ixx` ×2 | GCC 严格 |
| 8 | `std::exception(const char*)` | `policy.ixx` | MSVC 扩展 |
| 9 | `std::ifstream(const wchar_t*)` | `font.ixx` | MSVC 扩展 |
| 10 | 已在 `namespace …::msdf` 内又写 `msdf::` 限定 | `msdf.cpp` ×4 | 非法限定 |
| 11 | 构造参数与成员同名致 `vk::allocator&` 解析失败 | `image_atlas.util.ixx` | 见下 |
| 12 | `-Wchanges-meaning` 大面积命中 | 全仓 | 见 §15.4 |
| 13 | 裸 `size_t` | 见 §14 | 已修 |

两处值得单独说：

**#5 自指约束。** `quad_group` 的标量广播构造带
`!std::convertible_to<const Ty&, quad_group>` —— 判定「能否转成 quad_group」
必须先判定这个构造函数本身。GCC 报
`satisfaction of atomic constraint … depends on itself`，MSVC 从不检查。
换成非自指的 `!std::same_as<remove_cvref_t<Ty>, quad_group>` +
`!spec_of<remove_cvref_t<Ty>, quad_group>`。**真正损失的**是「Ty 自带
`operator quad_group<T>()`」这种 exotic 情形 —— 本代码库无此类型，而且要问这个
问题就绕不开问这个构造函数。这一处在源码注释里写明了。

**#6 allocator2d。** `allocator2d.ixx` 在 `export module` 之后 `#include`
`allocator2d.hpp`，而该 header 自身发出 `import std;` / `import mo_yanxi.math.vector2;`。
修法是把两条 import 提到 `.ixx` 里，并给 header 的副本加一道
`MO_YANXI_ALLOCATOR_2D_EXTERNAL_IMPORTS` 门 —— 这样调用方可以声明「imports 我已做过」。
补丁已进 `mcpp/patches/allocator2d.patch`。

### 15.4 `-Wchanges-meaning`：从逐个修改为整体开关

`vk::fence fence;`、`vk::instance instance;`、`interp interp{};`、
`resource_entity resource_entity;` …… **「成员名与其类型同名」是这个代码库的
普遍写法**。按 [basic.scope.class] 这是 ill-formed NDR，GCC 默认报错、MSVC 放行。

先前（§14.3 类别 ⑥）我按「把类型写成限定名」逐个修了 5 处。接上三个重库后，
命中面扩大到几十处，而且每一处都在别人的代码风格里。改用 GCC 为这个具体历史
写法提供的开关：

```toml
cxxflags = ["-Wno-interference-size", "-Wno-changes-meaning"]
```

已经顺手限定掉的几处保持限定形式 —— 那是更好的写法，只是不值得为它做全仓机械改动。
逐个限定仍是正解，但该由上游做。

`#11` 是同一族的极端表现：`sub_page` 有成员 `allocator2d<> allocator`，
构造参数又叫 `allocator`，GCC 在 `vk::allocator&` 处**直接解析失败**
（`expected ')' before '&'`）而不是给出 changes-meaning 诊断。参数已改名 `alloc`。

### 15.5 Linux CI

新增 `.github/workflows/mcpp-linux.yml`，与既有的 `build_and_dispatch.yml`
（Windows/MSVC via xmake）并行、互不干扰。

任务划分刻意分成两档：

- **required**：`mo_yanxi.utility` 与 `mo_yanxi.vulkan_wrapper` —— 它们是绿的，必须保持绿。
- **continue-on-error**：`react_flow` 与根包 —— 这不是「忽略失败」，而是一句关于
  **单一已知阻塞**的陈述。步骤照常运行、照常打印失败内容，所以新增的破坏在日志里可见；
  只是不能拿一个编译器 bug 去 gate 分支。react_flow 能编的那天就把它改成 required。

另外做了两件工程上必要的事：缓存 `~/.mcpp/registry/data` 与 `build-cache`
（GCC 16 工具链加 compat.* 源码约 500 MB，且只在 pin 变动时才变），
以及 `apt install libfontconfig1-dev`（`src/platform/font.ixx` 的 Linux 字体后端）。

### 15.6 react_flow 仍是唯一阻塞

结论未变，且这一轮又确认了一次：它不是「还没修」，是**七种规避全部无效的 GCC 16 bug**。
xrgui 有三处 `export import mo_yanxi.react_flow;`（`scene.ixx` /
`overlay_manager.ixx` / `text_tree.react_flow.ixx`），都在核心 GUI 层，绕不过去。

下一步只有两条：给 GCC 提 bug（需要先剥一个最小复现），或者把 react_flow 的 6 个
分区拆成独立命名模块。**后者不是机械改动** —— 同模块的分区之间可以看到彼此的
非导出实体，拆成独立模块后只有 `export` 的才可见，需要逐个核对跨分区用法。

---

## 16. clang 实验：越过了 GCC bug，却撞上 libc++

在 Linux 上把工具链换成 `llvm@20.1.7` / `llvm@22.1.8` 试了一轮。结论有两条，都值得记下。

### 16.1 `recursive lazy load` 是 **GCC 独有的**

**clang 完整编过了 react_flow。** 冷构建（`--cache off`，删掉 `target/` 与 `.mcpp/`），
`:endpoint` 与 `:modifier` 两个 PCM 都正常产出：

```
Resolved llvm@22.1.8 → …/bin/clang++
Compiling react_flow v0.1.0 (.)
Compiling utility (path)
Finished dev in 1.17s
  → pcm.cache/mo_yanxi.react_flow-endpoint.pcm
  → pcm.cache/mo_yanxi.react_flow-modifier.pcm
```

这一条把 §15.6 的判断钉死了：那不是代码问题，是 **GCC 16 模块实现的 bug**，
换一个实现就没有。给 GCC 提 bug 时这是最有力的一句。

继续往下，clang 越过了**整棵树**（含全部第三方与 196 个模块），最后只停在
`assets_summary.h` 找不到 —— 那是本设计 §8.3 里一直没实现的资产管线，与编译器无关。

### 16.2 但 libc++ 落后太多，clang 不能作为近期绕道

补上 `build.mcpp` 之后，clang 暴露的是另一类问题：**libc++ 尚未实现本代码库大量使用的
C++23 库特性**。

| 特性 | libstdc++ | libc++ | 本仓用量 |
|---|:--:|:--:|---:|
| `std::move_only_function` | ✓ | ✗ | 33 |
| `std::views::enumerate` | ✓ | ✗ | 46 |
| `std::views::stride` | ✓ | ✗ | 11 |
| `std::views::slide` | ✓ | ✗ | 3 |
| `std::is_pointer_interconvertible_with_class` | ✓ | ✗ | 4 |
| `std::const_iterator`（P2278 的**全局别名模板**，非 `Container::const_iterator` 成员类型） | ✓ | ✗ | 3 |

**合计约 100 处**，且都是标准库缺口而非本仓代码问题 —— 不是能补 shim 的量级
（`move_only_function` 与三个 range adaptor 各自都是完整设施）。

llvm 20 与 22 的 libc++ 都一样缺。**结论：GCC 仍是 Linux 上唯一可行的工具链，
react_flow 仍然阻塞。** clang 的价值在于它定位了 bug 的归属，不在于它能顶替。

> 若将来 mcpp 支持 clang + libstdc++ 组合（Linux 上的经典搭配），这个结论要重算 ——
> 那时缺的这些设施都由 libstdc++ 提供，而 GCC 的模块 bug 又不在 clang 前端里。

### 16.3 顺带查出一个 mcpp scanner 的真 bug：UTF-8 BOM

clang 一开始报的是这个：

```
error: unable to open output file '': 'No such file or directory'
```

空的输出路径。追下去是 **UTF-8 BOM**：仓库里 8 个源文件以 `EF BB BF` 开头，
mcpp 的扫描器因此认不出紧随其后的 `export module …`，模块名为空，
clang 的 `-fmodule-output=` 就拿到了空串。

**这个只在 clang 下暴露** —— GCC 的 BMI 按模块名存进 `gcm.cache/`，不走这条路径，
所以一直没人发现。已剥掉参与构建的 5 个文件的 BOM（`legacy/` 下 3 个不参与构建，未动），
GCC 侧复验仍绿。这条已补进 mcpp-community/mcpp#421。

### 16.4 `build.mcpp` 落地

§8.3 设计的资产管线已实现：扫 `properties/assets_raw/gen/**/*.svg` → bin2c →
聚合 `assets_summary.h`，全部写进 `MCPP_OUT_DIR`，不碰源码树。

三个细节：bin2c 就是十几行，比依赖 xmake 的 `utils.binary.bin2c` 更简单；
输出按文件名排序，否则目录遍历顺序不定会让内容哈希抖动、连累每个消费者重编；
**没有 SVG 时也照样生成空的 summary** —— 那正是全新检出的状态
（`properties/assets_raw/gen/` 由 `xmake xrgui.gen_icon` 产出、不入仓），
`gui.assets.cpp` 只要求这个头**存在**，空的能编过，这正是让整棵树不需要
Python/Node/slangc 就能构建的前提。

---

## 17. react_flow 解决了 —— 在项目侧，不在编译器侧

§16 的结论是「GCC 侧无解、项目侧只能部分解决」。**两条都被推翻了**，推翻它的是一个很朴素的问题：
*这套实现有 bug，那换一种实现呢？*

### 17.1 做法：消掉 `:manager` 分区

那个 bug 出在**分区 BMI 的流式化**上。所以让它没有 BMI 可流式化 ——
把 `manager.ixx` 的内容并进 `:node_interface`（所有相关分区本来就 import 它），
删掉 `:manager` 这个分区。

```
mcpp build (react_flow, 冷构建 --cache off)
    Finished dev in 4.30s        74 个 BMI，零错误
```

**`recursive lazy load` 彻底消失。** 不是绕过、不是抑制 —— 触发它的结构不存在了。

### 17.2 顺带发现两处真实的依赖缺陷

查「谁真的需要 `:manager`」时暴露出来的：

- **`:endpoint` / `:modifier` 根本不需要它。** `manager` 只出现在 `friend manager;`
  和 `on_pulse_received(manager& m)` 的引用参数上，而**三个函数体一次都没碰过 `m`**；
  `:node_interface` 早就有 `export struct manager;` 前向声明。去掉这两个 import
  后它们立刻编过 —— 这是本来就不该有的依赖。
- **7 个文件缺 `import mo_yanxi.vk.util;`。** `vk::allocator` / `vk::allocator_usage`
  住在 `mo_yanxi::vk` 命名空间，却由 `mo_yanxi.vk.util` **模块**提供，而
  `mo_yanxi.vk` 并不重导出它。MSVC 放行了这个缺口，GCC 不放。

两处都是**依赖卫生问题**，与编译器 bug 无关，修了对 xmake 侧同样有效。

### 17.3 与编译器侧的对照

| 路径 | 结果 |
|---|---|
| 改 GCC 的守卫（`attempt/relax-lazy-guard`） | ⛔ 换来 ICE，测试套件 4→17 失败 |
| **消掉分区（本节）** | ✅ **冷构建全绿** |

编译器那条路走不通的原因写在 `mcpp-community/mcpp-gcc` 里：守卫同时守着 section
顺序**和**类定义期状态，只放宽前者会让成员被塞进半成品类。

**这不改变「GCC 有 bug」这个事实** —— GCC 17 trunk 仍然复现，报告仍然该提。
改变的是：xrgui 不必等它。

### 17.4 代价与取舍

`:manager` 并进 `:node_interface` 后，后者从 780 行涨到约 1190 行。这是真实的代价：
一个分区的边界没了。换来的是整个项目能在 Linux 上构建。

如果将来 GCC 修好了，这次合并可以原样回滚 —— 补丁在
`mcpp/patches/mo_yanxi_react_flow.patch` 里，是自包含的。


---

## 18. manifest 挪回各自的模块目录

§4.3 把三个 submodule 的 manifest 集中放在 `mcpp/pkgs/`，用 `../../../external/...`
越界 glob 指回源码。理由是「不碰上游仓库」。

**那个理由已经不成立了。** 到 §17 为止，`mcpp/patches/` 里已经有四份补丁，
其中 react_flow 那份甚至删掉了整个 `manager.ixx`。既然已经在改上游源码，
manifest 放进去没有任何额外代价。

现在每个库的 `mcpp.toml` 就在它自己的源码旁：

```
external/mo_yanxi_react_flow/mcpp.toml                              sources = ["src/**/*.ixx"]
external/mo_yanxi_vulkan_wrapper/mcpp.toml                          sources = ["src/vk_wrap/**"]
external/mo_yanxi_vulkan_wrapper/external/mo_yanxi_utility/mcpp.toml
```

三处收益：越界 glob 没了；`mcpp/pkgs/` 这个容易被读成「包索引」的目录没了
（它从来不是索引——本地索引在 §7 规划过但最终没建，三个重库直接以 `compat.*`
进了 mcpp-index）；**上游化时零改动**，manifest 本来就在正确位置。

manifest 随补丁分发，所以 `apply.sh` 仍是新检出的必要一步——它本来就是。
