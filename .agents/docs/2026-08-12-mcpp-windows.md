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

### 编译器：14.52 不是偏好，是硬要求；而让 mcpp 用上它需要一个 workaround

**镜像自带的 14.51.36231 根本编不了本仓**，实测：

```
mo_yanxi_utility/src/utility/math/basic/vector2.ixx(67):
  fatal error C1001: Internal compiler error.
  (compiler file '...\CxxFE\sl\p1\c\template.cpp', line 26415)
  note: IFC import detected.
```

卡在实例化 `math::vector2<float>::ceil`。所以两条腿都必须是 14.52 preview。

麻烦在于 **mcpp 不接受外部指定编译器**。它解析 `msvc@system` 的顺序
（`src/toolchain/msvc.cppm`）是：

1. `vswhere -latest -products * -requires ...VC.Tools.x86.x64`
2. `VSINSTALLDIR` / `VS*COMNTOOLS`
3. `Program Files\...` 标准路径

第 1 步**没带 `-prerelease`**，Insider 实例对它不可见，于是永远返回 release 通道的
VS 2026 Enterprise；而第 2 步——唯一能被调用方控制的那个——**只在第 1 步找不到时才执行**。
所以光导出 vcvars 完全无效，这一点也是实测的：曾有一版照抄了 Insider 安装 +
vcvars 导出，**每轮多花约 7 分钟，构建日志里的 cl 一个字都没变**。

（顺带：导出 vcvars 也不会造成 14.51 的 cl 去读 14.52 的头。mcpp 会按它自己选中的
toolset **合成** `INCLUDE`/`LIB`（`msvc.cppm:117`），不继承环境里的。）

所以现在的做法是：装 Insider 到固定路径、直接从该路径导出 vcvars（不经 vswhere，
因为路径是我们定的），然后**把 vswhere.exe 挪开**，逼 mcpp 落到第 2 步。
mcpp 只在那一个字面路径找 vswhere，本 job 也没有别处用它。

**这是给 mcpp 打的补丁，不是这个仓库该有的东西。** mcpp 那边改一行——
给 vswhere 加 `-prerelease`，或让显式设置的 `VSINSTALLDIR` 优先——
这一步就可以整个删掉。删之前，构建前有一条断言：mcpp 若没解析到 14.52 就立刻失败，
而不是四十分钟后以 C1001 告终。

### mcpp 版本必须钉死，且下限是 2026.8.15.1

这条曾经伪装成「缓存问题」，查了很久。症状是一串

```
warning C5050: _MSVC_MT is defined in module command line and not in current command line
```

然后在 `corecrt_malloc.h` 上以
`error C2375: 'free': redefinition; different linkage` 真正炸掉。

真因是 **mcpp 编 std 模块时一个 runtime flag 都没传**，cl 于是按自己的默认取
`/MT`，而工程的 TU 拿到的是 `/MD`——即 mcpp#422，修复落在
**2026.8.15.1**（`src/toolchain/dialect.cppm` 的 `msvc_crt_flag`）。

而 CI 里那句不带版本的 `xlings install mcpp` **装的是 2026.8.11.3**，差一点，
于是冷缓存下依然复现。所以现在 `xlings install mcpp@<ver>` + `xlings use` +
校验 `mcpp --version`——mcpp 本身就是这个 job 要测的东西，不该由「那一小时索引
给了什么」决定。

缓存键里同时带 mcpp 版本和**探测到的 MSVC toolset**：`~/.mcpp` 里放的是 `.ifc`，
而 `.ifc` 和消费它的 TU 必须出自同一个 cl。

### `cxx_runtime` 在 MSVC 上只有一个可选项

mcpp 自己会说：

```
cxx_runtime = "self-contained" is not implemented for the MSVC runtime yet
(it would need the /MT runtime); using host-coupled
```

也就是 MSVC 上写什么都会落到 `host-coupled`（`/MD`）。曾有一版写
`self-contained` 想去迁就那个被 cl 报成 `/MT` 的 std 模块——那个设置是空操作
（写什么都退回 `/MD`），而真因是上面那条 mcpp 缺陷。现在直接写 `host-coupled`，
正是 xmake `set_runtimes("MD")` 要的。

### 上游那次绿和今天不是同一个编译器

| | 上游绿 (2026-06-24) | 今天 |
|---|---|---|
| MSVC | 14.52.**36510** | 14.52.**36629** |
| xmake | 3.0.9 | 3.1.0 |

`aka.ms/vs/18/insiders` 永远给最新的 Insider，没法钉到 36510。而 36629
收紧了模块边界的名字泄漏，**未改动的 master 在它上面编不过**——这一点用一个只含
一个 markdown 文件的探针分支单独验证过，与本 PR 无关。下面「必须改的源码」里
后三条就是被它逼出来的，每一条都只是把本来就在用的头/模块显式写出来。

因此两条腿都只断言 **14.52 这条线**，不钉具体 build 号：钉死等于把微软的发布节奏
变成本仓的红 CI。build 号漂移只打 warning，但它仍是这个 job 无故变红时第一个该
怀疑的东西。

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

## 必须改的源码

分两类。**前四处是 mcpp 要的**，后三处**和 mcpp 无关**——是 MSVC 从 14.52.36510
漂到 14.52.36629 之后，未改动的 master 自己就编不过了（见上面那张表）。

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

### 后三处：14.52.36629 收紧了模块边界

**两条腿都需要**，改的也不是逻辑，只是把本来就在用的东西显式写出来。
旧 MSVC 让名字跨模块边界泄漏，新版不再泄漏，仅此而已。

5. `typesetting.rich_text.argument.ixx` **`import mo_yanxi.math.vector2;`**。
   它用 `math::vec2`，而已有的 import 里没人 re-export 它：`graphic.color`
   re-export 的是 `math.vector4`，而 `vector4` 只是普通 import 了 `mo_yanxi.math`。
   漏了它就是 `error C2039: 'vec2': is not a member of 'mo_yanxi::math'`。

6. `typesetting.segmented_layout.ixx` **`#include <gch/small_vector.hpp>`**。
   `gch::small_vector_iterator` 的 `operator-(it, it)` 是靠 ADL 找到的命名空间作用域
   模板；`mo_yanxi.typesetting.rich_text` 只在**全局模块片段**里 include 了
   small_vector，而 purview 从没点名过的声明会被丢弃、不写进 BMI。本 TU 要对这些
   迭代器实例化算法，就得自己看见那个头。

7. `typesetting.rich_text.ixx` 里 `rich_text_fallback_style::operator==`
   **从 `= default` 改成写出函数体**，同一个机制的另一面。

   它的成员 `features` 是 `gch::small_vector`，而 gch 把容器的 `operator==`
   写成**自由函数模板而非 hidden friend**——于是它不是从导出类 decl-reachable 的，
   同样被丢弃。**defaulted 的比较是在 odr-use 处合成的**，也就是在每一个比较
   `layout_config` 的导入方里；那里找不到 gch 的 `operator==`，重载决议就去够
   small_vector 的**私有 allocator 基类**，cl 报 `error C2243`。

   这条最难查的地方在于**报错位置和该改的文件不是一个**：cl 指向模板定义处
   `ui.util.ixx:140`（`try_modify`），而实例化点在 `text_edit.ixx` / `label.ixx`
   /……——每加一个比较 `layout_config` 的地方就多一处。

   所以不在导入方逐个补 `#include`（那是打地鼠：修好 `text_edit.ixx`，下一轮就轮到
   `label.ixx`）。但**只写出函数体还不够**——`lhs.features == rhs.features` 调的
   `gch::operator==` **自己也是函数模板**，照样在导入方实例化，把 `std::equal`
   和它对 `operator-(a, b)` 的需求一起带过去；而那个 `operator-` 是 gch 的自由函数
   模板，同样不在 BMI 里。于是错误换了个样子重来一遍，这次是一大片 `<xutility>`
   内部的 C2794 / C3376 / C2062。

   真正的根治是**连 gch 的迭代器一起绕开**：按 `data()` / `size()` 比较，
   参与运算的是 `const hb_feature_t*`，没有任何东西需要从 BMI 里找。
   语义完全一样。

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
