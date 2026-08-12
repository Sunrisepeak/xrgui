# XRGUI 的 mcpp / Windows 适配

范围：**只有 Windows + MSVC**。Linux 与 GCC 不在本次范围内。

## 立足点

上游 `xmake.lua` 是 MSVC-first 的，且它的 CI 是绿的。所以 `mcpp.toml`
**是对 `xmake.lua` 的转写，不是第二套意见**——两者若有差异，那是转写的 bug。

`.github/workflows/mcpp-windows.yml` **只跑 mcpp**。上游的
`build_and_dispatch.yml` 已经在同一个 PR、同一个 runner 镜像、同一版 MSVC 上跑
xmake 了，在这里再跑一遍等于每轮多装一次 VS 2026 却得不到新信息。工具链配置照抄
那个 workflow（VS 2026 Insider + MSVC 14.52 preview、Vulkan SDK、slang），
所以两条腿仍然可比。

xmake 本身也不装：它那两个资产任务不过是 Python 脚本的薄包装，本 job 直接调脚本。

### 运行期数据靠 `mcpp::action`（对应 xmake 的 `after_build`）

xmake 在 `after_build` 里把 `properties/assets` 和 `vk_layer_settings.txt` 拷进
目标目录。`build.mcpp` 自己做不了：它在 prepare 阶段运行，而二进制目录不在环境
契约里——`${mcpp.bin_dir}` 只作为 **action 的插值**存在。

所以声明成 action。`artifact` 是「输出是新文件、而非编译或链接输入」的那个 role，
正合这里。

有一点要说清楚，免得被角色名误导：**`artifact` 本身并不把这条边排到链接之后**。
文档说它「在链接之后运行」，前提是该 action 的**输入本身是链接产物**。这里的输入
是源文件，所以 ninja 想什么时候拷都行——这没问题，运行期数据本来就不依赖二进制。
要强行排在链接后就得点名某个具体 target，而哪些 target 存在取决于激活了哪些
feature，那样更脆。（这条是查 `build.ninja` 里实际的依赖边确认的，不是照搬文档。）

**一条 action 就够，不必两条**：一条拷贝命令可以带多个源。POSIX 上是
`cp -r <assets> <vk_settings> <bin>/`；Windows 上用 `Copy-Item`，因为它能在同一次
调用里同时处理目录和普通文件，而 `copy` / `xcopy` 各自只能管一半。

每个输出仍必须点名（mcpp 在 prepare 时就把文件集定死了）。这里可行是因为文件很少，
而且上面的着色器生成已经先跑完、产物已在磁盘上。

xmake 那段里另外两项：`add_syslinks` 已落在 `[target.windows.build] ldflags`；
`set_policy("build.optimization.lto")` 对应 `[profile.release] lto = true`，
**暂不开启**——预览版 MSVC 加上 C++ 模块，再叠 LTO，在整个构建还没绿过一次之前
不值得引入这个变量。等绿了再开。

### 资产生成在 `build.mcpp` 里

xmake 的 `xrgui.gen_icon` / `xrgui.gen_slang` 两个任务不过是 Python 脚本的薄包装，
现在由 `build.mcpp` 直接调用，所以 `mcpp build` 是自洽的，本地检出也能拿到同样的资产。

| 工具 | 谁需要 | 干什么 |
|---|---|---|
| Python | 两个生成器的驱动 | `slang_builder.py` 并行调 slangc 编 `.slang` → `.spv`（带增量哈希）；`svg_normalize.py` 管缓存与任务编排 |
| Node | **只有** `svg_normalize.py` | `npx --yes oslllo-svg-fixer`，跑浏览器内核把 SVG 描边转填充、合并路径，好让 msdfgen 能吃 |
| slangc | `slang_builder.py` | 着色器编译器本体 |

**mcpp 自己一个都不需要**——它是自包含二进制，C++ 构建只要工具链。所以两个生成器
都是「探测到工具才跑，否则跳过并打日志」：这棵树必须在没有 python/node/slangc 时
也能构建，那正是空 summary 存在的意义。

生成器失败**只告警不中止**，因为它们的退出码不可信：`svg_normalize.py` 在它的 npx
子进程崩掉时仍然返回 0。真正的把关是 CI 里的 `Assert generated assets exist`——
断言产物数量，那是唯一有意义的信号。

（`slang_builder.py` 用的是 Python 3.11+ 自带的 `tomllib`，只在更老版本上才回退到
`tomli`。xlings 装的是 3.13，所以上游 workflow 里那句 `pip install tomli` 不需要。）

**除 Vulkan SDK 和 MSVC 外的工具全部由 xlings 装**——node / python / slang / mcpp
都在 xim-pkgindex 里，而 mcpp 本来就用这个 registry 解析自己的工具链和依赖包。
一条 `xlings install node python slang mcpp` 取代了四个手写的 setup 步骤，
版本也和构建自身读的是同一个索引。

## 对应关系

| xmake.lua | mcpp |
|---|---|
| `add_xrgui_target_options()` | `[target.windows.build] cxxflags`（`/bigobj`、`/utf-8`）|
| `add_xrgui_core_deps()` | `[build]` 的 sources / include_dirs / defines + `[dependencies]` |
| `add_xrgui_default_stack()` | `src.backends/*` 与 `gui.config` 的 glob |
| `add_requires(...)` | 索引包，一条对一条 |
| `target("xrgui.example")` 等三个 | `[targets.*]` |
| `rule("media.svg_to_bin")` + `before_build` | `build.mcpp` |
| `add_syslinks(...)` | `[target.windows.build] ldflags` |

三个 submodule 各自的 `xmake.lua` 同样逐条转写到 `mcpp/<name>/mcpp.toml`。
清单放在 `mcpp/` 而不是 submodule 里，因为那是别人的仓库；glob 因此要越界指回
`../../external/...`。将来上游化时，把文件挪进 submodule 根、去掉前缀即可，其余不变。

## 四处必须改的源码

前三处都是同一个原因：**mcpp 的 M1 扫描器禁止条件预处理块里出现 import**，
即使那个分支根本不激活；它也不支持头单元。

1. **`XRGUI_FUCK_MSVC_INCLUDE_CPP_HEADER_IN_MODULE` 整个删除**（16 个文件，全是删除）。
   这个宏在 `#include <plf_hive.h>` 和 `import <plf_hive.h>;` 之间二选一。
   mcpp 两种都不行，所以 `#include` 分支转正。`xmake.lua` 自己的注释早就写着
   「msvc 新版好像没这问题了，哪天删了」，所以那条 `add_defines` 也一并去掉。
   **对 xmake 没有行为变化**：它本来走的就是这个 include。

2. **`mcpp/patches/mo_yanxi_vulkan_wrapper.patch`**：把
   `stack_trace.cpp` 里一条 import 提到 `#if defined(__cpp_lib_stacktrace)` 之外。
   全仓扫下来这是**唯一**还在条件块里的 import。六行，CI 应用。

3. **magic_enum 改走索引包**，不再编 submodule 的 `module/magic_enum.cppm`——
   那个文件把 `import std;` 包在 `#ifdef` 里。索引包正好为此带了 `scan_overrides`，
   比给上游打补丁便宜。

第四处是**重复符号**，不是扫描器：

4. `src.backends/miniaudio/audio.cpp` 的 `MINIAUDIO_IMPLEMENTATION` 加了
   `XRGUI_MINIAUDIO_IMPL_PROVIDED` 守卫。`compat.miniaudio` 会编上游自带的
   `miniaudio.c`，再实例化一次会让每个 `ma_*` 符号出现两遍——而且是**链接期**
   才报，暴露得很晚。xmake 不定义这个宏，其构建逐字节不变。

## 两处接口差异

**nanosvg**：本仓写的是 `<nanosvg/nanosvg.h>`，因为 xrepo 把头文件装得比上游深一层。
`compat.nanosvg` 跟随上游布局，所以用 `[generated_files]` 生成两个转发头，
而不是去改每一处调用点。

**VMA**：取自已检出的 submodule，**不用** `compat.vulkan-memory-allocator`——
`vk_wrap/util/vma.cpp` 已经定义了 `VMA_IMPLEMENTATION`，用包会重复。

## 测试

xmake 的 `xrgui.tests` 目标链接的是 `src/` 的一个手挑子集加 `src.tests/`。
mcpp 的测试发现路径是 `tests/**`，与本仓布局不符，所以这里把它做成一个
feature 门控的普通 bin：

```
mcpp build --features tests
target/<triple>/<fp>/bin/xrgui_tests.exe
```

CI 直接跑这个可执行文件。它链接的是整个 xrgui 库（xmake 那个目标只链子集），
是超集，能过说明库本身也是好的。
