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

曾经的做法是：装 Insider 到固定路径、直接从该路径导出 vcvars（不经 vswhere，
因为路径是我们定的），然后**把 vswhere.exe 挪开**，逼 mcpp 落到第 2 步。

### ✅ 那个 workaround 已经删掉了（mcpp 2026.8.16.1）

当时写的是「**这是给 mcpp 打的补丁，不是这个仓库该有的东西**，mcpp 那边改一行
就可以整个删掉」。已经改了，也已经删了 —— mcpp#432 / #434 把顺序改成

```
VSINSTALLDIR → vswhere(-prerelease) → VS*COMNTOOLS → 标准路径
```

理由不是「加一条路径」而是「**猜测不该压过答案**」：`VSINSTALLDIR` 是有人明确
说了用哪个，vswhere 是一个排序猜测。

**删掉之后这条断言反而变强了，这是关键。** 现在 vswhere.exe **在**，
它排第一的 Enterprise 14.51 也**装着** —— 没有任何东西被拿走，
错误答案只是必须输。而 workaround 还在的时候，「`VSINSTALLDIR` 被采纳」与
「vswhere 找不到东西」现象完全一样，**根本无法区分缺陷是否真的修好了**。

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

### Windows 系统库要用 `link_lib`，不能写 `ldflags`

`ldflags` 是**原样透传**给链接器的扁平字符串（`plan.cppm` 的注释说得很直白）。
所以 `-luser32` 这种 GNU 拼法会原封不动送到 MSVC 的 `link.exe`，而它**不报错**：

```
LNK4044: unrecognized option '/luser32'; ignored
```

然后继续跑，直到最后一步以 **235 个 unresolved external** 收场——从错误现场
完全看不出是链接标志被丢了。

正确做法是 `build.mcpp` 里的 `mcpp::link_lib()` / `mcpp::link_search()`：
它们走 `Transform::LibFlag` / `LibSearchPath`，由工具链方言决定拼法
（`user32` → `user32.lib` 或 `-luser32`，目录 → `/LIBPATH:` 或 `-L`）。

需要补的不只是本仓 `add_syslinks` 的那五个。**索引包也是 GNU 拼法**：

| 包 | 声明 |
|---|---|
| `compat.glfw` | `ldflags = { "-lgdi32" }` |
| `compat.mimalloc` | `ldflags = { "-lpsapi", "-lshell32", "-luser32", "-ladvapi32", "-lbcrypt" }` |
| `compat.vulkan` | `ldflags = { "-Llib", "-lvulkan-1" }` |

在 MSVC 下这些全部无效，所以 `build.mcpp` 里把它们重新声明了一遍
（vulkan 的目录用 `mcpp::dep_dir("compat.vulkan")` 拿）。**这是在替包做事**，
等索引包改成两种链接器都能消费的形式后就该删掉。

### submodule 里的 UTF-8 BOM 会让 mcpp 认不出模块声明

症状极具误导性——报在 MSVC 头文件式的错误上，而且**看起来是偶发的**：

```
array_queue.ixx(191): error C3474: could not open output file '/interface'
```

实际的命令行是 `... /ifcOutput  /interface /TP ...`：**`/ifcOutput` 后面是空的**，
于是 cl 把下一个 token `/interface` 当成了输出文件名。

链条是：mcpp 的扫描器逐行读模块声明，而**它的源码里没有任何地方处理 BOM**。
文件若以 `EF BB BF` 紧跟 `export module X;` 开头，扫描器就认为它不提供模块
（`cu.providesModule` 为空），于是编译边拿不到 `bmi_out`；但 MSVC 的 ninja 规则
仍然无条件发 `/ifcOutput $bmi_out`，空值就这么漏了出去。

「偶发」的错觉来自 submodule 里 BOM 分两类：

| BOM 后面是 | 结果 |
|---|---|
| `module;`（全局模块片段行） | **没事**——`export module` 在后面的行上 |
| `export module X;` | **中招**——BOM 贴在声明行上 |

8 个带 BOM 的文件里只有 2 个属于后者（`array_queue.ixx`、`array_stack.ixx`），
它们又分布在不同的 feature 构建里，所以每轮挂的步骤都不一样。

CI 在打完 patch 后统一剥掉 submodule 里 `.ixx`/`.cppm` 的 BOM。剥掉是安全的：
编译行上有 `/utf-8`，没有谁靠 BOM 推断编码。**不做成 patch 文件**，因为一个内容
只有三个不可见字节的 patch 没法审。

### `cxx_runtime` 在 MSVC 上现在**真的会选 CRT 模型**（这一段已被推翻）

**写这份文档时**它在 MSVC 上是空操作，mcpp 自己会说：

```
cxx_runtime = "self-contained" is not implemented for the MSVC runtime yet
(it would need the /MT runtime); using host-coupled
```

于是曾有一版写 `self-contained` 去迁就那个被 cl 报成 `/MT` 的 std 模块，
既没害处也没作用，而真因是那条 mcpp 缺陷。

**自 mcpp 2026.8.16 起不再是这样**（mcpp-community/mcpp#422）：

| 值 | MSVC 上的含义 |
|---|---|
| `host-coupled` | `/MD` —— xmake `set_runtimes("MD")` 要的那个 |
| `self-contained` | `/MT` —— **另一个 CRT 模型**，不是同一个的更严格版本 |

所以那句诊断已经不存在了，而**今天做同样那次编辑会静默把构建切到 `/MT`**。
`mcpp.toml` 里之所以把 `host-coupled` 显式写出来（而不是靠默认），理由从
「写什么都一样」变成了「写错了会变成另一个 CRT 模型」。

`.ifc` 那一半是另一回事，已单独修好：缓存键带上了 VCToolsVersion，
一个 std 模块和它的消费者不可能再来自两个不同的 cl。

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

xmake 那段里另外两项：`add_syslinks` 落在 `build.mcpp` 的 `mcpp::link_lib`（见上面
那节，不能写 ldflags）；
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
断言产物数量，那是唯一有意义的信号。这条断言确实抓到过东西，见下。

**探测工具要按名字解析，不要问它版本。** 曾用 `<tool> --version` 的退出码判断装没装，
结果 `slangc` **根本没有 `--version`**：

```
1 | --version
  | ^^ unknown command-line option '--version'
slangc --version exit=1     where slangc exit=0
```

于是一个装好且能用的编译器被判成缺失，着色器生成被静默跳过，
表现为 `normalized icons: 40   compiled shaders: 0`。现在用 `where` / `command -v`。

**别指望在正常构建里看到 `build.mcpp` 的输出。** mcpp 把它的 stdout 和 stderr
**合并捕获**，解析成指令流，只在该程序非零退出或超时时才回显。所以诊断信息写去
stderr 只是「不污染指令流」，并不等于可见——真正的把关始终是上面那条断言。

（`slang_builder.py` 用的是 Python 3.11+ 自带的 `tomllib`，只在更老版本上才回退到
`tomli`。xlings 装的是 3.13，所以上游 workflow 里那句 `pip install tomli` 不需要。）

**除 Vulkan SDK 和 MSVC 外的工具全部由 xlings 装**——node / python / slang / mcpp
都在 xim-pkgindex 里，而 mcpp 本来就用这个 registry 解析自己的工具链和依赖包。
一条 `xlings install node python slang mcpp` 取代了四个手写的 setup 步骤，
版本也和构建自身读的是同一个索引。

### CI 两条腿:`msvc@system` 与 `msvc@<toolset>`

同一个 toolset 版本(14.52.36629),差别只在**编译器从哪来**。

| | system | managed |
|---|---|---|
| 装什么 | VS 2026 Insider Build Tools,**4分06秒**(实测 run 31980964778) | xim payload,~250 MB,落在已有的 `~/.mcpp` 缓存里 |
| 需要 vcvars / VSINSTALLDIR | 要 | 不要 |
| 那个 pin | **只能断言** | **真的是 pin** |

最后一行是加这条腿的主要理由。`aka.ms/vs/18/insiders` 永远给最新的 Insider ——
所以 system 腿只能*断言* 14.52.36629、发现微软挪了就打个 warning 然后用别的接着编。
managed 腿的 payload 是 **sha256 内容寻址**的:下个月还是同一批字节。

**那为什么不把 system 腿换掉?** 因为它是**唯一**能提出那个问题的地方:这台 runner
上装着**两个** VS —— 镜像自带的 release 版 Enterprise 14.51,和这个 job 装的 Insider ——
而 mcpp 必须在 vswhere 仍然把前者排第一的情况下,选中 VSINSTALLDIR 点名的后者。
这就是 mcpp#432/#434 那条「明确的答案压过探测」。mcpp 自己的 CI 只有一个 VS,
**posers 不出这个场景**。

managed 腿**不显式跑 `mcpp toolchain install`**。manifest 里的
`[toolchain] windows = "msvc@<toolset>"` 走的是和任何其它依赖一样的
`autoInstall` 路径(`prepare.cppm:1364`),并会带上包声明的 `xim:windows-sdk`。
手动先装一遍的话,**这条路径坏了这条腿也照样绿** —— 而"声明就够了"正是它要证明的事。
`mcpp why toolchain` 本身走 `prepare_build`,所以冷缓存下 toolset 就是在那一步拿到的。

两处顺序上的坑,都不会自己报出来:

1. **toolset 安装必须排在 cache 之后。** payload 落在 `~/.mcpp`,而那正是 cache 恢复的
   目标 —— 先装就是先下 250 MB 再被覆盖。
2. **改写 manifest 的那步必须自我断言。** 万一 `windows = "msvc@system"` 那行改了名或换了
   格式,替换会静默失效,managed 腿就变成 system 腿的副本 —— **照样绿,但什么都没证明**。

## 对应关系

| xmake.lua | mcpp |
|---|---|
| `add_xrgui_target_options()` | `[target.windows.build] cxxflags`（`/bigobj`、`/utf-8`）|
| `add_xrgui_core_deps()` | `[build]` 的 sources / include_dirs / defines + `[dependencies]` |
| `add_xrgui_default_stack()` | `src.backends/*` 与 `gui.config` 的 glob |
| `add_requires(...)` | 索引包，一条对一条 |
| `target("xrgui.example")` 等三个 | `[targets.*]` |
| `rule("media.svg_to_bin")` + `before_build` | `build.mcpp` |
| `add_syslinks(...)` | `build.mcpp` 的 `mcpp::link_lib`（**不是** ldflags）|

三个 submodule 各自的 `xmake.lua` 同样逐条转写到 `mcpp/<name>/mcpp.toml`。
清单放在 `mcpp/` 而不是 submodule 里，因为那是别人的仓库；glob 因此要越界指回
`../../external/...`。将来上游化时，把文件挪进 submodule 根、去掉前缀即可，其余不变。

## 必须改的源码

分两类。**前五处是 mcpp 要的**，后三处**和 mcpp 无关**——是 MSVC 从 14.52.36510
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

5. `src/graphic/msdf.{cpp,ixx}` 的 `#define MSDFGEN_USE_CPP11` 加
   `XRGUI_MSDFGEN_NO_CPP11` 守卫。`compat.msdfgen` 是按 `MSDFGEN_USE_CPP11=OFF`
   构建的，并**通过每个公开头都会包含的 `msdfgen-config.h` 告知消费者**——这正是
   那个包让两边对「哪些声明存在」达成一致的机制。自己 `#define` 就单方面破坏了它：
   重载在我们的 TU 里被声明、在库里却不存在，链接期才以
   `unresolved external symbol Contour::addEdge(EdgeHolder&&)` 暴露。
   xrepo 的 msdfgen 是开着这个宏构建的，所以 xmake 保持原样、逐字节不变。

### 后三处：14.52.36629 收紧了模块边界

**两条腿都需要**，改的也不是逻辑，只是把本来就在用的东西显式写出来。
旧 MSVC 让名字跨模块边界泄漏，新版不再泄漏，仅此而已。

6. `typesetting.rich_text.argument.ixx` **`import mo_yanxi.math.vector2;`**。
   它用 `math::vec2`，而已有的 import 里没人 re-export 它：`graphic.color`
   re-export 的是 `math.vector4`，而 `vector4` 只是普通 import 了 `mo_yanxi.math`。
   漏了它就是 `error C2039: 'vec2': is not a member of 'mo_yanxi::math'`。

7. `typesetting.segmented_layout.ixx` **`#include <gch/small_vector.hpp>`**。
   `gch::small_vector_iterator` 的 `operator-(it, it)` 是靠 ADL 找到的命名空间作用域
   模板；`mo_yanxi.typesetting.rich_text` 只在**全局模块片段**里 include 了
   small_vector，而 purview 从没点名过的声明会被丢弃、不写进 BMI。本 TU 要对这些
   迭代器实例化算法，就得自己看见那个头。

8. `typesetting.rich_text.ixx` 里 `rich_text_fallback_style::operator==`
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

   同一条规则还命中了第二处：`typesetting.ixx` 里
   `feature_stack_ = { features.begin(), features.end() }`——
   `std::vector` 的范围构造要靠 `operator-(a, b)` 算容量，而这是**类模板的成员**，
   同样在导入方实例化。也改成 `assign(data(), data() + size())`。

   **判据**：gch 容器的迭代器只要出现在「会在别的模块里实例化」的代码里
   （类模板成员、函数模板、BMI 里的 inline 体调用的模板），就会中招。
   本仓其余 `small_vector` 都是非模板类的私有成员或 `.cpp` 里的局部变量，
   在自己的 TU 里用完，所以没事。

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
