# 构建与开发说明

XRGUI 有两条构建链：`xmake.lua` 是上游的、以 MSVC 为准的那条；`mcpp.toml` 是它的转写，
两者构建同一棵源码树，产物等价。本页说清楚各自要什么、怎么跑、生成的资产去了哪里。

## 环境要求

| | xmake | mcpp |
|---|---|---|
| 编译器 | Visual Studio 2026 预览版 MSVC 14.52（镜像自带的 14.51 会 ICE） | Windows 同左，由 `VSINSTALLDIR` 指定；Linux 和 macOS 用 `mcpp.toml` 配置的 `llvm@22.1.8`，mcpp 自动安装 |
| Vulkan SDK | 需要（`$VULKAN_SDK`） | 不需要；`compat.vulkan` 来自索引 |
| Python / Node / slangc | 需要，生成图标和着色器 | 不需要；着色器由 `mcpp.rules.slang` 编译，图标由仓库自带的 `mcpp/svg_outline` 工具处理 |
| 第三方库 | xrepo | mcpp 索引，版本配置在 `mcpp.toml` |

mcpp 的安装见 [mcpp 的 README](https://github.com/mcpp-community/mcpp)；机器上装好它之后，
第一次 `mcpp build` 会下载并安装上面所有东西，之后是增量的。

## 用 xmake

```powershell
git submodule update --init --recursive
xmake quickstart          # 配置、生成资产、doctor、构建并运行 xrgui.hello
xmake doctor              # 只检查环境
xmake -b xrgui.example && xmake run xrgui.example
xmake -b xrgui.tests   && xmake run xrgui.tests
```

资产生成是两个 task：`xmake xrgui.gen_icon`（`svg_normalize.py` + node）和
`xmake xrgui.gen_slang`（`slang_builder.py` + slangc），产物落在 `properties/assets_raw/gen`
和 `properties/assets/shader/spv`，运行时按工作目录加载。

## 用 mcpp

```bash
git submodule update --init --recursive
mcpp run xrgui_hello                          # 构建并启动
mcpp run --features examples --release xrgui_example
mcpp run --features tests xrgui_tests
mcpp pack --profile dev xrgui_hello           # vendored tar（Windows 为 zip）
mcpp pack --profile dev --format appimage xrgui_hello   # Linux
mcpp pack --profile dev --format msi xrgui_hello        # Windows，需要 PATH 上有 WiX 6
```
showcase 在非 `NDEBUG` 构建里会打开 Vulkan validation layer，需要机器上有 Vulkan SDK 的
`VK_LAYER_KHRONOS_validation`；mcpp 不装 SDK，所以没有它的机器用 `--release` 运行 showcase
（xmake 的 debug 配置有同样的前提，`xmake doctor` 会检查）。

与 xmake 那条链不同的地方，全部在 `mcpp.toml` 和 `build.mcpp` 里能看到：

- **着色器编进二进制**。`properties/assets_raw/shader/slangs` 下的入口文件由
  `mcpp.rules.slang` 逐个编成 SPIR-V 并嵌入，`render_context.cpp` 和
  `templates/showcase/src/main.cpp` 在 `XRGUI_SHADER_SURFACE` 下通过生成的
  `xrgui.shaders.h` 取得它们；不定义这个宏（xmake）时走原来的 `.spv` 文件。
- **图标不经 node**。`mcpp/svg_outline` 把描边图标几何地转成填充轮廓并直接输出字节列表，
  每个图标一条构建边。
- **运行期数据由 `[runtime] deploy` 放到可执行文件旁**，`mcpp pack` 也会带上。
- **`build.mcpp` 只做 mcpp 没有对应键的事**：给两个 submodule 打 clang 需要的补丁
  （补丁的归宿是上游仓库）、按图标声明构建边并写汇总头、在 macOS 上加一个私有 include 目录。

## 模板

`templates/hello` 和 `templates/showcase` 是 mcpp 的项目模板，同时也是 `xrgui_hello`
和 `xrgui_example` 两个目标的入口文件本身——两条构建链都指向这里，所以模板不会和示例脱节。

```bash
mcpp new myapp --template xrgui:hello         # 默认模板
mcpp new myapp --template xrgui:showcase      # 依赖 xrgui 的 examples feature
mcpp new --list-templates xrgui
```

模板通过包名解析，前提是 xrgui 已发布到 mcpp 索引。在此之前，同样的项目可以手写
`mcpp.toml`，用 `xrgui = { path = "<本仓库路径>" }` 依赖本地检出。

模板生成的 `mcpp.toml` 带一段 `[toolchain]`（Linux 和 macOS 都是 `llvm@22.1.8`）：消费者的工具链
决定依赖用什么编译，而 xrgui 在 Linux 和 macOS 上只用这个 clang 构建过；不配置的话
Linux 机器默认的 gcc 会去编 xrgui，在第一个私有模块片段上就停下来。Windows 上默认的 `msvc@system`
就是 xrgui 自己用的。

同样由消费者决定的还有设备轴：模板里的 `[build] accel = "vulkan1.3"` 不是装饰，依赖自己的
`accel` 不会设置整个构建的加速器集合，不写它 xrgui 的着色器就不会被路由到 Slang 规则，
生成的 `xrgui.shaders.h` 也就不存在。

## 目标与依赖

| 目标 | xmake | mcpp | 说明 |
|---|---|---|---|
| 最小示例 | `xrgui.hello` | `xrgui_hello` | `templates/hello/src/main.cpp` |
| showcase | `xrgui.example` | `xrgui_example`（`--features examples`） | `templates/showcase/src/main.cpp` + `src.examples/` |
| 单元测试 | `xrgui.tests` | `xrgui_tests`（`--features tests`） | gtest |

三个 submodule（`mo_yanxi_utility`、`mo_yanxi_vulkan_wrapper`、`mo_yanxi_react_flow`）
在 mcpp 下是 `mcpp/` 里的 path 包，各自的 `mcpp.toml` 是对那个仓库 `xmake.lua` 的转写；
其余第三方库与 `xmake.lua` 的 `add_requires` 一一对应。

## 平台支持

| 平台 | xmake | mcpp | CI |
|---|---|---|---|
| Windows / MSVC 14.52 | 参考构建 | 支持 | `build_and_dispatch.yml`；`mcpp-windows.yml` 两条腿（机器上的 VS，和 mcpp 自己安装的 toolset） |
| Linux / clang 22 / libc++ | 未验证 | 支持 | `mcpp-unix.yml`：构建、测试、tar 和 AppImage |
| macOS / clang 22 / libc++ | 未验证 | 支持（不打包） | `mcpp-unix.yml`：构建、测试；打包暂不做（mcpp 内建的闭包遍历还不处理 Mach-O 程序，`.app` 是插件的一个独立成员） |

`src/platform/` 的三个分支是 `_WIN32`、`__linux__`、`__APPLE__`：字体在 macOS 上走 CoreText，
线程命名在 macOS 上暂不生效。

macOS 构建另有三处只对 Apple 生效的配置，都在 mcpp 这条链里：`mcpp/patches/` 给
`rect_ortho.ixx` 的 SSE 代码一个标量回退（arm64 没有 `<immintrin.h>`）；`mcpp/darwin/assert.h`
把 SDK `assert()` 里的 `__builtin_expect` 去掉（clang 22 在模块之间会把这个内建函数报成歧义），
由 `build.mcpp` 只在 macOS 上加到本包的 `-I`；`cxx_runtime = "host-coupled"` 只配置给 MSVC
（对应 xmake 的 `/MD`），Linux 和 macOS 用 mcpp 的默认值，静态链接工具链自带的 libc++——
macOS 系统的 libc++ 比 clang 22 的头文件旧，缺符号。

## Windows 上 GUI 程序的控制台

mcpp 构建的 `xrgui_hello` 和 `xrgui_example` 是 `/SUBSYSTEM:WINDOWS`，不再弹出控制台窗口，
代价是它们打到 stdout 的日志在终端里看不到。需要看日志时用 `xrgui_tests` 或 xmake 构建的版本。

## 相关链接

- mcpp 仓库与手册：<https://github.com/mcpp-community/mcpp>，`docs/` 目录按章节编号（`04` 是 `mcpp.toml`，`30` 是 `build.mcpp`，`10` 是打包）。
- 包索引：<https://mcpplibs.github.io/mcpp-index/>；构建插件（Slang 规则、AppImage / MSI 成员）：<https://github.com/mcpp-community/mcpp-plugins>。
- 给 AI 助手用的入口：mcpp 仓库里的 `.agents/skills/mcpp-usage/SKILL.md` 是使用指南，`.agents/skills/mcpp-contributing/SKILL.md` 是贡献流程。让助手上手 mcpp，把下面这段发给它即可：

  ```
  Read .agents/skills/mcpp-usage/SKILL.md and the docs/ directory of the
  https://github.com/mcpp-community/mcpp repository,
  then tell me how to create a C++23 module project with dependencies using mcpp.
  ```
