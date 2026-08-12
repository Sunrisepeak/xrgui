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
