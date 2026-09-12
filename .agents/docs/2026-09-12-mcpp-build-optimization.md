# XRGUI 的 mcpp 构建:对齐 2026.9.12.2 的优化方案(待 Review)

状态:**提案,未实施。** 本文只陈述现状、mcpp 新版本提供了什么、以及建议怎么改;
每一条都标了前提版本、收益、风险和验证方法。需要拍板的点集中在最后一节。

前一份文档 `2026-08-12-mcpp-windows.md` 记录的是「怎么让它编过」;这一份记录的是
「编过之后,哪些东西现在可以交还给 mcpp」。

## 0. 结论先行

本仓钉的是 mcpp **2026.9.9.1**,索引里最新是 **2026.9.12.2**(xim-pkgindex #824 已
把它标为 latest),中间隔了 2026.9.10.1 / .10.2 / .11.1 ~ .11.4 六个版本。这六个版本
里有五样东西正好对着 `build.mcpp` 现在手写的五块代码:

| # | 现在 build.mcpp / mcpp.toml 里手写的 | mcpp 现在提供的 | 最低版本 | 建议 |
|---|---|---|---|---|
| P1a | `stage-runtime-data` action:`cp -r` / `Copy-Item` 两套命令,逐文件点名输出(71 行) | `[runtime] deploy_files` + `deploy = [{from, to}]`,打包时也自动带上 | 2026.9.12.2 | **做**,整块删掉 |
| P1b | `[target.windows.runtime]` 里替 glfw / mimalloc / vulkan 三个索引包重写的系统库,以及 build.mcpp 里 vulkan-1 的搜索目录(36 行) | 包描述文件里的 `libraries` / `link_library_dirs`,由方言渲染 | 早已可用;**索引包没改** | **做**,先给 mcpplibs 索引提 PR,再删本仓副本 |
| P1c | `linux-desktop` feature + `--features linux-desktop` | `[target.linux.dependencies]`,按解析后的目标求值 | 早已可用(docs/22) | **做**,manifest 里那句「mcpp 没有平台条件依赖表」已过时 |
| P1d | `svg_normalize.py` 靠 PATH 上的 `npx`;CI 手工 `xlings install node python slang` | `[xlings.workspace]` 声明 `xim:node`;声明了 `[xlings] subos` 的工程,build.mcpp 的 PATH 前缀就是该环境的 bin | 2026.8.25.1 | **做**,最后一个宿主依赖 |
| P2 | `slang_builder.py`:python 进程池、`--oneshot` 每次全量、失败只告警、`.spv` 落盘再按 cwd 加载 | `mcpp:plugins` 的 `rules-slang`:每个 shader 一条 ninja 边,`-depfile` 增量,失败即构建失败,slang 版本由规则自带;消费者 `import xrgui.shaders;`,存储方式(嵌入 / 落盘)是构建侧的开关 | 2026.9.7.1 + plugins **0.7.0**(已发布,索引 PR mcpplibs/mcpp-index#401) | **做**。上游缺口已在 0.7.0 补齐(§4.2);`render_context.cpp` 改成 accessor 调用,带宏守卫 |
| P3 | `svg_normalize.py` + `npx oslllo-svg-fixer`(python + node,栅格化再描摹) | 自写工具 `mcpp/svg_outline`,`tools = [...]` 构建,每个图标一条 action | 2026.8.5.1 | **做了**(§4.3):python 和 node 退出,bin2c 一并进工具 |
| P4 | 没有打包;CI 直接上传 `target/**/bin/` | `mcpp pack`、`--format appimage/msi`、`[resources] icon`、`windows_subsystem` | 2026.9.11.1 / .12.2 | **全做**:`[package]` 元数据、`mcpp pack` 进 CI、AppImage、MSI、`.ico`、GUI 子系统 |
| P5 | 只有 Windows CI | Linux 已经能编能跑(e1eb3b4) | — | **做**一条 Linux 腿,同时是 P2/P4 在 clang 上的验证场 |

不建议做的:workspace 化、私有 SubOS、submodule 改 git 依赖、用 `[hooks]` 打补丁
(§6)。submodule 补丁**留在 build.mcpp**,因为 mcpp 没有对应机制,而它的正确归宿
是上游仓库。

## 1. 现状:build.mcpp 在干六件事

480 行,按块拆开:

| 行 | 块 | 行数 | 性质 |
|---|---|---|---|
| 1–37 | 头注释 | 37 | 说明 |
| 41–77 | `note()`、`have_tool()` | 37 | `have_tool` 是 PATH 探测,与「工具已声明」的前提冲突 |
| 79–114 | `apply_patch()` | 36 | **保留** |
| 116–155 | `xpkg_tool()` | 40 | 保留,但 PATH 回退可删 |
| 157–169 | `run_generator()` | 13 | 保留(图标仍是生成器) |
| 171–222 | bin2c 四个 helper | 52 | 保留(P3 不做) |
| 226–262 | main:补丁 | 37 | **保留** |
| 264–309 | main:两个生成器 | 46 | 着色器那一半(14 行)由 P2 接管 |
| 311–358 | main:bin2c + summary | 48 | 保留 |
| 360–430 | main:`stage-runtime-data` action | 71 | **P1a 整块删除** |
| 432–467 | main:vulkan-1 搜索 | 36 | **P1b 整块删除** |
| 469–480 | 尾 | 12 | 保留 |

P1 + P2 做完约 **480 → 350 行**。行数不是重点,重点是删掉的是**三种机制**:
按平台分叉的 shell 拷贝命令、PATH 探测、依赖机器状态的链接搜索路径。剩下的每一
行都只回答一个 mcpp 没法回答的问题(补丁、图标归一化、bin2c)。

manifest 侧同样有三处「替别人做事」的副本:`[target.windows.runtime]` 里三个包的
系统库、`linux-desktop` 这个假 feature、以及没写进去的 node。

CI 侧:`xlings install node python slang` 手工装三样(其中 python / slang 已在
manifest 里,mcpp 自己会装);`Assert generated assets exist` 存在的唯一理由是生成器
失败不报错。

## 2. 2026.9.9.1 → 2026.9.12.2:与本仓相关的变化

只列本仓用得上的,按版本:

| 版本 | 变化 | 对本仓的意义 |
|---|---|---|
| 2026.9.10.1 | 构建程序的对象不再进入依赖的映像;重复符号检查按绑定分流 | 无直接影响 |
| 2026.9.11.1 | `mcpp pack --format <name>` 分派到包;`${mcpp.stage_dir}`;`MCPP_PKG_*` / `mcpp::package_*()` 进构建程序 | P4 |
| 2026.9.11.2 | 暂存对分派格式是服务不是前置条件;扫描器不再读注释里的 `import` | 后者是扫描器修正,本仓 submodule 里有大量注释掉的 import,值得留意 |
| 2026.9.11.3/.4 | wasm / Android / iOS 各行;`.mcpp-toolchain.json` | 无直接影响 |
| 2026.9.12.2 | `[runtime] deploy`;`windows_subsystem` / `windows_entry`;**快路径比较 `--toolchain`**;xlings 安装失败时回显其 stderr | P1a、P4;快路径那条是本地切工具链时的正确性修复;最后一条让 CI 里「provision 失败」终于能看到 xlings 说了什么 |

插件包 `mcpp:plugins`(仓库 `mcpp-community/mcpp-plugins`)现在 **0.6.0**,索引
`mcpplibs/pkgs/m/mcpp.plugins.lua` 已收录,`min_mcpp` 2026.9.7.1。成员里与本仓相关:
`rules-slang`(2026.9.7.1+)、`tools-embed`(2026.9.5.4+)、`dist-appimage` /
`dist-wix`(2026.9.11.1+)。

另外两条本仓一直在等的索引侧事实,**今天仍然没变**(查的是本机
`~/.mcpp/registry/data/mcpplibs` 里的描述文件):

```
compat.glfw.lua:164      ldflags = { "-lgdi32" }
compat.mimalloc.lua:106  ldflags = { "-lpsapi", "-lshell32", "-luser32", "-ladvapi32", "-lbcrypt" }
compat.vulkan.lua:441    ldflags = { "-Llib", "-lvulkan-1" }
```

所以 P1b 的第一步在索引仓,不在本仓。

## 3. P0:把 pin 抬到 2026.9.12.2

所有后续项的前提。改三处:`.github/workflows/mcpp-windows.yml` 的 `MCPP_VER`、
同文件 cache key(已含版本,自动失效)、`mcpp.toml` 顶部注释。`mcpp.lock` 不动,
它记录而不约束(docs/05 §「Current limitations」)。

风险:冷缓存一次。验证:两条 Windows 腿绿,`mcpp why toolchain` 断言不变。

## 4. 方案

### 4.1 P1:纯删代码,不碰源码

#### P1a 运行期数据 → `[runtime] deploy`

`properties/assets` 里除生成的着色器外只有 **4 个文件**(font / images /
markdown / test.csv),加 `vk_layer_settings.txt`。写成:

```toml
[runtime]
deploy_files = ["properties/vk_layer_settings.txt"]          # 放在可执行文件旁
deploy = [
  { from = "properties/assets/font/SourceHanSansCN-Regular.otf", to = "assets/font" },
  { from = "properties/assets/images/logo.png",                  to = "assets/images" },
  { from = "properties/assets/markdown/preview.md",              to = "assets/markdown" },
  { from = "properties/assets/test.csv",                         to = "assets" },
]
```

删掉 build.mcpp 360–430 行。`mcpp pack` 会把这两个键的文件放到打包后可执行文件旁
的同一相对位置(2026.9.12.2),所以 P4 不用再为它写 `[pack] include`。

两点要在第一次跑的时候验证,而不是假设:

1. `from` 文档写的是「文件」;如果实测接受目录,四行并成一行。如果不接受,给
   mcpp 提一个「`from` 支持目录或 glob」的 issue,本仓先按文件写。
2. 着色器 `.spv` 是生成物,manifest 里写不出它的文件名。所以 **P1a 依赖 P2**:
   着色器进了二进制,这个目录就不存在了。P2 之前做 P1a,action 得为 spv 保留。

xrgui 按 `current_path()` 找 `assets/`,不按可执行文件所在目录 —— 这是应用侧的事,
和今天的 action 一样,从 `bin/` 里启动才找得到。打包成 bundle 后这一点会变成问题
(§4.4)。

#### P1b 三个索引包的 Windows 链接 → 包描述文件

先在 `mcpplibs` 索引仓改三个配方,把 GNU 拼法换成方言中立的:

```lua
-- compat.glfw      windows:  libraries = { "gdi32" }
-- compat.mimalloc  windows:  libraries = { "psapi", "shell32", "user32", "advapi32", "bcrypt" }
-- compat.vulkan    windows:  libraries = { "vulkan-1" },  link_library_dirs = { "lib" }
```

合并后本仓:

- `[target.windows.runtime] libraries` 只剩 xmake `add_syslinks` 的五个(`gdi32` /
  `psapi` / `bcrypt` 三行删掉;`shell32` / `user32` / `advapi32` 与本仓自己的重合,
  留着);
- build.mcpp 432–467 行整块删除。`VULKAN_SDK` 回退一起删:mcpp 图里 `compat.vulkan`
  永远在,机器上的 SDK 从来不该是答案(这是「能不依赖 host 就不依赖 host」那条规则)。

验证:Windows 两条腿链接通过;`ninja -t commands` 里看到 `vulkan-1.lib` 来自
compat.vulkan 的 `/LIBPATH:`。

#### P1c `linux-desktop` feature → 平台条件依赖

```toml
[target.linux.dependencies.freedesktop]
fontconfig = "2.15.0"
```

删 `[features] linux-desktop = {}` 和 `[feature-deps.linux-desktop]`。Linux 上的命令
从 `mcpp build --features linux-desktop` 变成 `mcpp build`。docs/22 §「What a project
writes」:selector 下接受 `dependencies` / `dev-dependencies` / `build-dependencies` /
`feature-deps.<f>`,按**解析后的目标**求值,Windows 构建连下载都不会发生。
manifest 里「`[target.<pred>]` 只有六个键、没有依赖表」那段注释要一起删。

#### P1d 声明 node,CI 少一步

```toml
[xlings.workspace]
"xim:python" = "3.13.12"
"xim:slang"  = "2026.14.1"
"xim:node"   = "24.20.0"        # svg_normalize.py 的 npx;索引的 latest,即 CI 今天 `xlings install node` 装到的
```

生效机制:本仓声明了 `[xlings] subos = "default"`,所以 build.mcpp 的 PATH 已经是
`<该环境的 bin>:<mcpp 自己的 PATH>`(docs/23 §2)。`svg_normalize.py` 里的 `npx`
不用改一个字。

CI 里 `xlings install node python slang` 那一步删掉,只留 `xlings install mcpp@$VER`
+ `xlings use`。后面那段「装了但不在 PATH 上就先挂」的检查只留 mcpp。三个工具改由
mcpp 在第一次 `mcpp build` 时按 manifest 供给 —— python / slang 今天已经是这样,
只是 CI 又装了一遍。

#### P1e 删 PATH 回退,把「跳过」说出来

P1d 之后三个工具全部声明,`have_tool()` 和 `python3` → `python` 的回退是死代码:
mcpp 要么把包装上,要么(`--offline`)拒绝并点名。删掉 41–77 行的 `have_tool` 和
272–274 行的回退。

同时把生成器**跳过**时的 `note()` 换成 `mcpp::warning()`。前一份文档已经承认
`note()` 在成功构建里看不见;`warning` 是「程序处理正确但用户想知道」这种情况的
通道,会被缓存回放,并带包名前缀。「python 没装、图标没归一化」这句话第一次能
在绿构建里被看到。

### 4.2 P2:着色器进构建图(`rules-slang`)

#### 今天的形状

`slang_builder.py` 读 `config.toml`(17 个 shader、5 个公共 flag、1 个逐 shader flag、
2 个 include 目录、1 个 alias),开 `-j 30` 进程池调 slangc,`--oneshot` 所以
**每次 build.mcpp 重跑都全量编 17 个**;失败只 `WARNING`;`.spv` 落到
`properties/assets/shader/spv/`,运行时按 cwd 加载。这就是 CI 里
`Assert generated assets exist` 存在的原因。

#### 目标形状

```toml
[build-dependencies.mcpp]
plugins = { version = "0.6.0", features = ["rules-slang"] }   # rule_module 蕴含 host-module

[build]
accel   = "vulkan1.3"          # default_application.cpp: apiVersion = VK_API_VERSION_1_3 → 规则推出 spirv_1_6
sources = [
  # ... 现有条目 ...
  { glob = "properties/assets_raw/shader/slangs/**/*.slang", accel = "vulkan1.3" },
  # lib/ 是被 import 的模块,ui_structs.slang / slide_line.slang 不在 config.toml 里 —— 都不是入口
  "!properties/assets_raw/shader/slangs/lib/**",
  "!properties/assets_raw/shader/slangs/ui/ui_structs.slang",
  "!properties/assets_raw/shader/slangs/ui/slide_line.slang",
]
defines = [ "...", "XRGUI_EMBEDDED_SHADERS" ]   # 见下面「源码改动」
```

```cpp
// build.mcpp
import mcpp.rules.slang;
...
mcpp::rules::slang::options o;
o.base_dir = "properties/assets_raw/shader/slangs";
o.includes = { "properties/assets_raw/shader/slangs/lib",
               "properties/assets_raw/shader/slangs/ui" };
o.optimization = "3";
o.module_name  = "xrgui.shaders";
if (!mcpp::rules::slang::compile(o)) return 1;
```

规则做的事:每个 `.slang` 一条 `role = "source"` 的 action(排在编译之前),
`slangc -depfile` 声明 `import` 依赖,`-source-embed-style u32` 产出头文件,再由
`mcpp::plugins::surface` 生成一份声明 —— 消费者写
`xrgui::shaders::ui::draw::vert()` 拿到 `{code, size_bytes}`。slang 的版本由规则自带
(`xim:slang >= 2026.14.1`,在 `cfg(accelerator = "vulkan")` 下才安装),本仓
`[xlings.workspace]` 里那行 `xim:slang` 可以删,要钉版本时再写回来、工程侧胜出。

收益:17 条并行边;改一个 `lib/*.slang` 只重编 import 它的;失败**让构建失败**并点名
那条边;`Assert generated assets` 的 spv 那半删掉;python 不再是着色器的依赖;
`properties/assets/shader/spv` 这个运行期目录消失,`mcpp pack` 没有额外东西要收。

#### 插件已经有什么,Slang 规则还缺什么

`mcpp::plugins::surface`(插件包的公共生成层)已经把「产物怎么到达程序」拆成两个
互相独立的轴,`rules-spirv`、`rules-slang`、`tools-embed` 都经它生成:

| 轴 | 取值 | 含义 |
|---|---|---|
| 调用面 `surface` | `module_` / `c_header` | 消费者 `import xrgui.shaders;` 调 `xrgui::shaders::ui::draw::vert()`,或 `#include` 一个头文件调同名函数 |
| 存储 `storage` | `header` | 字节以 C 数组进源码,编进二进制;任何工具链都行,**默认** |
| | `object` | 字节经 `.incbin` 进目标文件,不过 C++ 编译器;需要 GAS 汇编器,MSVC 上自动退回 `header` |
| | `sidecar` | 文件放在产物旁,accessor 在**运行时**按路径打开;文档明说这是 shader 热重载和超大载荷要的形状 |

关键性质:**三种存储下消费者写的代码一模一样**。`tests/spirv-consumer` 和
`tests/spirv-sidecar` 的 `main.cpp` 都是 `pkg::shaders::scale_comp()`,差别只在
`build.mcpp` 里一行 `opt.storage = storage::sidecar`。所以「嵌入还是落盘」不是源码
问题,是构建侧一个开关,以后随时可以切。

**2026-09-12 已在 mcpp-plugins 0.7.0 补齐**(mcpp-community/mcpp-plugins#17,
tag `v0.7.0`,GitCode 镜像已发布且逐字节一致,索引 PR mcpplibs/mcpp-index#401):
`options::storage`、`options::extra_args`、`options::per_file`、`profile_for` 的 1.4
行。三种存储都在本机 mcpp 2026.9.12.2 下实测过;CI 新增 `slang-sidecar` 夹具和
「拼错 `per_file` 键必须拒绝」的反向腿。下表保留作为对照,「缺口」一列现在读作
「0.7.0 里对应的字段」:

| config.toml | 规则里 | 缺口 |
|---|---|---|
| `include_dir` | `options::includes` | 无 |
| `-O3` | `options::optimization` | 无 |
| `-fvk-use-gl-layout` `-fvk-use-entrypoint-name` `-emit-spirv-directly` `-floating-point-mode fast` `-g` | 0.6.0 没有透传字段 | **G1,已补**:`options::extra_args`,原样追加在规则自己的 flag 之后、`-o` 之前 |
| `.spv` 落盘 | 0.6.0 只有嵌入 | **G3,已补**:`options::storage`,与 `rules-spirv` 同一个枚举;`object` / `sidecar` 下不加 `-source-embed-*`,直接产出 `.spv` |
| `frag_mask_apply.slang` 单独 `-emit-spirv-via-glsl` | 见下 | **G2,已补**:`options::per_file["<glob 里的路径>"].extra_args`;键拼错会拒绝并列出本次编译的 shader |
| `alias = "ui/blit/basic"` | 名字由文件名推导 | 不需要;消费者改叫 `lane_merge` |
| 默认 profile | 0.6.0 的 `profile_for()` 没有 `1.4` 这一行 | 已补(1.4 → spirv_1_6);本仓是 1.3,不受影响 |

**G2 是什么。** `config.toml` 里 17 个 shader 共用 5 个 flag,只有
`ui/draw/frag_mask_apply.slang` 多一个 `-emit-spirv-via-glsl`(先转 GLSL 再出
SPIR-V,是绕某个直出 SPIR-V 的问题的 workaround)。规则的 `options` 是**一次
`compile()` 调用共用一套**,没有逐文件覆盖。规则有 `compile(span<文件>, options)`
重载,看起来可以调两次(16 个 + 1 个),但**每次 `compile()` 都会重写一遍
`xrgui.shaders` 这个生成模块,且只写它自己那一批** —— 第二次调用会把第一次的 16 个
accessor 覆盖掉。所以要么上游加逐文件覆盖(`options::per_file`),要么那一个 shader
用另一个模块名(`xrgui.shaders_glsl`,消费者 import 两个模块,难看)。

0.7.0 的 `per_file` 已经能写它。做 P2 时仍值得先试一次:用 slang 2026.14.1 不带这个
flag 编 `frag_mask_apply.slang`,能编过且渲染正确,这条 per-file 项就不用写。

#### 源码改动(一处,带宏守卫,与存储方式无关)

`gui.config/default/render_context.cpp` 143–153 行,13 个
`shader_modules.emplace_back(device, shader_spv_path / "ui.draw.vert.spv")`。
`mo_yanxi_vulkan_wrapper/src/vk_wrap/objects/shader.ixx:27` 已经有
`shader_module(VkDevice, std::span<const T> code)` 这个构造函数,所以每行是一个
替换:

```cpp
#ifdef XRGUI_EMBEDDED_SHADERS
	const auto vert = xrgui::shaders::ui::draw::vert();     // {code, size_bytes},嵌入或落盘都是这个调用
	auto& draw_shader_vert = shader_modules.emplace_back(ctx.get_device(),
		std::span<const std::uint32_t>{vert.code, vert.size_bytes / 4});
#else
	auto& draw_shader_vert = shader_modules.emplace_back(ctx.get_device(), shader_spv_path / "ui.draw.vert.spv");
#endif
```

宏由 mcpp.toml `[build] defines` 给出,xmake 路径一个字不变 —— 和
`XRGUI_MINIAUDIO_IMPL_PROVIDED` / `XRGUI_MSDFGEN_NO_CPP11` 是同一种做法。宏名叫
`XRGUI_EMBEDDED_SHADERS` 不准确,因为落盘也走这条分支;叫 `XRGUI_SHADER_SURFACE`
更贴切。

module 调用面要 `MCPP_LANGUAGE_MODULES=1`,即 manifest 写 `[language] modules = true`;
本仓今天只写了 `[build] module_extensions`,没写 `[language]`。不写的话默认是
`c_header` 面 —— 也能用,但既然全仓都是模块,建议写上,或在 `options::surface` 里
显式选 `module_`。

**存储方式的默认值**:建议 `header`(嵌入)。理由:`mcpp pack` 不用再收任何东西,
程序不依赖 cwd,MSVC 也支持。要热重载时在 `build.mcpp` 里切 `sidecar`,源码不动。

#### 验证

- `ninja -t commands` 里 17 条 `slangc` 边,参数与今天 `slang_builder.py --show-cmds`
  打出的逐字对得上(除了 `-o` 和嵌入相关的三个 flag);
- 改 `lib/sdf.slang`,只有 import 它的边重跑;
- 故意写坏一个 shader,`mcpp build` 非零退出并点名该边;
- Windows 两条腿 + Linux(P5)跑 `xrgui_hello` 出图。

### 4.3 P3:图标 —— 一个 `mcpp/` 下的 C++ 工具,python 和 node 全部退出

**原方案说不换 `tools-embed`,理由仍然成立**(它产出 `unsigned char`,消费方是上游代码
要 `char[]`)。实施时换了个角度:问题不在 bin2c,在它前面那一步。

`svg_normalize.py` 调的 `npx oslllo-svg-fixer` 做的是**栅格化再描摹**:依赖
`oslllo-svg2` 和 `oslllo-potrace`,把 SVG 渲成位图再用 potrace 描回矢量。这就是它
需要 node 和浏览器内核的原因,而结果是描摹出来的近似。需要它只因为 `msdf.cpp` 的
`process_nsvg_basic` 只收填充路径,而 40 个图标里 38 个是描边画的(宽 3,圆角接头和
端帽)。

`mcpp/svg_outline/` 是一个 `kind = "bin"` 的 path 包,`main.cpp` 一百多行,只依赖
`compat.nanosvg`(xrgui 运行时读 SVG 用的同一个包):nanosvg 解析,贝塞尔按容差拍平,
**每段一个胶囊轮廓**(矩形加两个半圆,全部同向),填充路径原样拷过去。不做布尔并:
xrgui 调的 `generateMSDF` 用默认 `GeneratorConfig`,`overlapSupport = true`,同向重叠
轮廓在非零环绕下就是并集,圆角接头由相邻胶囊的端帽自然得到。输出既可以是 SVG,也可以
直接是 `--embed` 的字节列表,所以 bin2c 也搬进了工具。

接法是文档 §4.2 里 protoc 那种:`[build-dependencies] svg_outline = { path =
"mcpp/svg_outline", tools = ["svg-outline"] }`,mcpp 按宿主编译一次、全局缓存;
`build.mcpp` 用 `mcpp::dep_bin` 拿到路径,每个图标一条 `role = "source"` 的 action,
`assets_summary.h` 在 prepare 期就能写全(名字集合已知),字节在边跑完后到位。

代价与验证:胶囊比描摹曲线多边,容差 0.15(48 单位图标上约三分之一像素)下一个图标
5 KB 左右,原来 1.5 KB;渲染对照 40 个图标逐个与原始描边的栅格结果一致(两处几何 bug
就是这样抓到的:端帽半圆扫错方向、逆向弧的控制点没翻号)。7 个图标里有个别元素不是
圆角接头,工具按圆角渲染并各报一次。`[xlings.workspace]` 清空,CI 里「图标存在」的
断言删除:一条边失败就是构建失败。

### 4.4 P4:打包

今天 CI 上传的是 `target/**/bin/`:Linux 上那是 RPATH 指向沙箱的开发产物,不是能拷
走的东西(docs/10 开头那句)。

**基础部分**:

```toml
[package]
name        = "xrgui"
version     = "0.1.0"
description = "..."
license     = "..."            # 仓库 LICENSE
authors     = ["..."]
repo        = "https://github.com/Yuria-Shikibe/xrgui"
```

这五个字段从 2026.9.11.1 起进构建程序(`MCPP_PKG_*`),每一种安装包格式都要它们,
不写就得在成员 options 里再写一遍。然后 CI 里 `mcpp pack`(Linux:vendored tar;
Windows:先实测 `mcpp pack` 今天产出什么 —— docs/10 说 DLL bundling 还在 roadmap,
`.zip` 一级),上传 `target/dist/` 而不是 `bin/`。P1a 之后运行期数据会被自动带上。

**格式分发(决定:现在就做)**:

| 项 | 需要 | 说明 |
|---|---|---|
| `mcpp pack --format appimage` | `features += "dist-appimage"`,一张 PNG 图标(有内置占位),categories | `xim:appimagetool` 由成员自己在 `cfg(linux)` 下声明,离线可用 |
| `mcpp pack --format msi` | `features += "dist-wix"`,runner 上 `dotnet tool install --global wix` | WiX 不可再分发,成员只定位不安装 |
| `[resources] icon = "properties/assets/images/xrgui.ico"` | 从 `properties/assets/images/logo.png` 生成一个 `.ico` 提交进仓 | Windows 可执行文件图标 + 版本信息(从 `[package]` 推导),非 PE 目标零成本;AppImage 的 PNG 图标直接用 `logo.png` |
| `[targets.xrgui_hello] windows_subsystem = "windows"`,`xrgui_example` 同 | 2026.9.12.2 | **决定:切。** 去掉控制台窗口;`xrgui_tests` 保持控制台。后果要知道:GUI 子系统下 stdout 不再接到父终端,xrgui 现在打到 stdout 的日志在 Windows 上看不见了,需要日志走文件或 `OutputDebugString`,这是应用侧的事 |
| `[profile.release] lto = true` | — | 前一份文档说「绿了再开」,现在绿了。预览版 MSVC + 模块 + `/GL` 仍是未知量,单独一个 PR,量了再合 |

**应用侧的一个前提**:xrgui 用 `current_path()` 找 `assets/`。vendored bundle 有一个
顶层入口包装,它是否 `cd` 进 `bin/` 要实测;`self-contained` 模式下还有
`/proc/self/exe` 指向 loader 的问题(docs/10)。最稳的做法是 xrgui 按可执行文件所在
目录解析资源,但那是应用改动,本文只标出来。

### 4.5 P5:Linux CI 腿

Linux 已经能编能跑(e1eb3b4、855c03b),但没有 CI。加一个 `mcpp-linux.yml`:
ubuntu-24.04,`xlings install mcpp@$VER`,`mcpp build`,`--features tests` + 运行,
`mcpp pack`。

manifest 需要一行 `[toolchain] linux = "llvm@22.1.8"` —— 本地 port 用的是 clang 22 /
libc++,本机 `~/.mcpp/registry/data/xpkgs/xim-x-llvm/` 下装的正是 22.1.8(另有 20.1.7)。
合并前用 `mcpp why toolchain` 确认一次。

这条腿也是唯一能量 `bmi_schedule = "on"` 的地方(MSVC 上 mcpp 明说不动;clang 上
mcpp 自己量到 cold 86.7s → 35.7s)。先量,不盲开:文档写的是「调度错了是静默的」。

## 5. 改完之后 build.mcpp 长什么样

```cpp
import mcpp;  import std;
import mcpp.plugins;  import mcpp.rules.slang;  import mcpp.dist.appimage;  import mcpp.dist.wix;

namespace {
void apply_patch(...);                 // 不变
std::string xpkg_tool(...);            // 只查声明的包,没有 PATH 回退
void run_generator(...);               // 失败走 mcpp::warning
// bin2c 四个 helper                   // 不变
}

int main() {
    // 1. submodule 补丁(不变)
    // 2. 图标归一化:python 来自 xpkg_dir,npx 来自声明了 node 之后的 PATH;
    //    python 缺席 -> mcpp::warning
    // 3. bin2c + assets_summary.h(不变)
    // 4. 着色器:一次 mcpp::rules::slang::compile(),c_header 调用面
    // 5. 打包格式:appimage / wix 各声明一次,`mcpp pack --format` 时才提交
}
```

374 行(原 480;多出来的是注释,记录 CI 上量到的东西)。没有了:`have_tool`、`stage-runtime-data`、vulkan-1 搜索、slang
生成器、`-j 30`。mcpp.toml 没有了:`linux-desktop`、三个包的系统库副本、
`xim:slang`。CI 没有了:`xlings install node python slang`、spv 断言、三个工具的
PATH 检查。

## 6. 不做的事

| 项 | 为什么不 |
|---|---|
| `[workspace]` 化三个 `mcpp/mo_yanxi_*` 包 | 它们共享的只有 `module_extensions = [".ixx"]` 一行;`[workspace.build]` 能省的就这一行,换来一层新概念 |
| `[xlings] subos = "<私有名>"` | 隔离的代价是第一次构建先装一遍环境;本仓的工具版本已经全部钉在 `[xlings.workspace]`,`default` 够用 |
| submodule → `git =` 依赖 | 补丁问题不会因此消失;补丁的归宿是上游 PR |
| `[hooks]` 打补丁 | 实验性;`build_start` 在 prepare **之后**开,而补丁要在扫描**之前**;失败只告警 |
| `tools-embed` 替换 bin2c | §4.3 |
| `bmi_schedule = "on"` 直接开 | MSVC 上无效;clang 上先量 |

## 7. 上游清单

| 仓库 | 内容 | 阻塞谁 |
|---|---|---|
| `mcpplibs` 索引 | compat.glfw / compat.mimalloc / compat.vulkan 的 Windows 链接改 `libraries` / `link_library_dirs` | P1b |
| `mcpp-community/mcpp-plugins` | **已完成**:0.7.0(#17)带 `extra_args` / `per_file` / `storage` / 1.4 行;索引 PR mcpplibs/mcpp-index#401 | P2(不再阻塞) |
| `mcpp-community/mcpp` | `[runtime] deploy` 的 `from` 是否接受目录 / glob —— 先实测再决定要不要提 | P1a(非阻塞) |
| xrgui 上游(`properties/`) | `svg_normalize.py` 传出子进程失败 | 无,顺手 |

## 8. 顺序

**决定:本仓的所有改动都进 fork 上的一个 PR
[Sunrisepeak/xrgui#8](https://github.com/Sunrisepeak/xrgui/pull/8),在那上面验证;
往上游提交时再拆。** 下面的顺序是同一个分支上 commit 的顺序,不是 PR 的划分:

1. P0 pin + P1c + P1d + P1e(只动 manifest / CI / build.mcpp 小块)。
   验证:两条 Windows 腿绿;本地 Linux `mcpp build` 不带 `--features`。
2. P5 Linux 腿,尽早上:后面每一步在 clang 上的验证场。
3. 索引仓 PR(P1b 前置)合并后:删三个包的副本和 vulkan-1 块。
4. mcpp-plugins 0.7.0 已进索引后:P2(`plugins = { version = "0.7.0", ... }`)+ P1a + 删 action。
5. P4:`[package]` 元数据、`.ico`、`windows_subsystem`、`mcpp pack` 与
   `--format appimage` / `--format msi` 进 CI。
6. LTO 与 `bmi_schedule`,各量一次再决定。

索引仓和插件仓的改动天然在别的仓库,是 #8 里第 3、4 步的前置,不算拆分。

每一步都保持前一份文档的纪律:xmake 那边一个字不动(第 4 步的 `#ifdef` 除外,
且默认分支就是 xmake 今天的行为)。

## 9. 已定与未定

已定(2026-09-12 review):`windows_subsystem` 切;单 PR #8;打包格式现在就做;
Linux 腿跟第一步一起上。

还在讨论的两点:

1. **P2 存储方式的默认值**:`header`(嵌入,我的建议)还是 `sidecar`(落盘)。
   两者源码相同,只是 `build.mcpp` 一行;区别在 `mcpp pack` 要不要收文件、程序是否
   依赖 cwd、以及要不要热重载。
2. **G2 已不阻塞**(0.7.0 有 `per_file`)。做 P2 时先试 `frag_mask_apply.slang`
   去掉 `-emit-spirv-via-glsl` 能不能编过;能就少写一项。

## 10. 实施记录(2026-09-12,PR #8)

全部落在 `feat/mcpp-support`,按 commit 拆:文档、构建系统、着色器调用面、
Linux 端口修复。上游两个 PR 先行:mcpp-plugins 0.7.0(#17,已进索引)、
mcpplibs/mcpp-index#402(三个包的 `runtime.libraries`)。

实测偏离与发现,每条都改进了文档上面的写法:

1. **调用面用 `c_header`,不用 `module_`。** 消费者 `render_context.cpp` 是模块
   实现单元;`#ifdef` 包住一个 `import` 会被 mcpp 的扫描器拒绝(和 vulkan_wrapper
   那个补丁是同一条规则),而全局模块片段里 `#ifdef` 包住 `#include` 没问题。
   两种调用面生成的 C++ 完全相同(`xrgui::shaders::ui::draw::vert()`),只是到达
   方式不同。宏名 `XRGUI_SHADER_SURFACE`。
2. **`bloom.merge.slang` 按名排除。** slang 2026.14.1 下无论什么 flag 都报
   `duplicate modifier`(三处 `restrict readonly RWTexture2D`);仓库里没有任何
   代码加载它;旧流程只告警,所以 17 个 `.spv` 里从来就没有它。现在失败会让构建
   失败,只能排除,等上游修着色器。
3. **`frag_mask_apply` 不需要 `-emit-spirv-via-glsl`**,实测直出 SPIR-V 编过,
   所以没有 `per_file` 项;上游加的这个字段本仓暂时用不上。
4. **`mcpp pack` 要点名 target。** manifest 声明了两个 `bin`,默认 feature 集里
   只有一个,不点名时 pack 挑到了 `xrgui_example` 然后拒绝。CI 里写
   `mcpp pack --profile dev xrgui_hello`。`--profile dev` 是为了复用已有构建,
   不再按 release 全量编一遍。
5. **Linux 上 examples / tests 从没编过。** 端口 commit 只覆盖库和 hello。四处
   clang 严格性修复,与端口 commit 同类:三个缺失的 `import`(`mo_yanxi.vk.util`、
   `mo_yanxi.math.interpolation`、`mo_yanxi.math.matrix3`)和
   `inout_animator.ixx` 里一个 `int` 作 `float` 非类型模板实参。54 个测试通过。
6. **`[build-dependencies.mcpp]` 必须放在 `[dependencies]` 的最后一条之后。**
   TOML 表头一直作用到下一个表头,放早了 `neargye.magic_enum` 就变成了
   `mcpp.neargye.magic_enum`。踩过一次,记下来。
7. **本机 mcpp 对 xrgui 默认解析到 gcc 16。** 端口是用 clang 22 做的,manifest
   现在钉 `linux = "llvm@22.1.8"`,CI 断言 `mcpp why toolchain` 的答案。
8. **`shader_module` 的 span 构造函数不设 `name`**,所以 `post_process_pass` 报错
   信息里的着色器名在嵌入路径下是空的。只影响一条日志,接受。
9. **本机数字**(缓存热,依赖已编):`mcpp build` 45 s 墙钟、4m20s CPU;
   `xrgui_hello` 在 RTX 4080 上完整初始化并运行;vendored tar 17.7 MB,含
   `bin/assets/*`、`vk_layer_settings.txt`、`HOST-REQUIREMENTS`;AppImage 产出在
   `target/.build-mcpp/out/xrgui-x86_64.AppImage`。

**CI 上又发现的五件事**(本地 Linux 全绿之后,Windows 两条腿和 Linux 腿各自暴露的):

10. **Linux 腿的安装脚本要 `/dev/tty`**,runner 没有;`XLINGS_NON_INTERACTIVE=1`
    是它自己的开关。装完 mcpp 后 SubOS 的 bin 不在 PATH 上,要再导出一次。
11. **MSVC 14.52 拒绝 mcpp-plugins 0.7.0 的 host module**(36629 和 36725 都是):
    `filesystem(1572): error C2801: '_Path_iterator<...>::operator ==' must be a
    non-static member`,STL 自己的 hidden friend。三轮探针定位:去掉规则里的
    `lexically_normal` 没用,去掉 `lexically_relative` 也没用,`declare.cppm`
    同样的 import 集合却编过,它唯一的区别是不碰 `std::filesystem`。形状是
    「lib 根的 BMI 里带着 `_Path_iterator` 的实例化,和 `import std` 一起进到一个
    再碰 `path` 的单元」。修在 lib 根:路径拆成字符串比较。发布为
    **mcpp-plugins 0.7.1**(#18,mcpp-index #403);0.7.0 在这个 toolset 上对任何
    构建程序碰路径的消费者都不可用。中间用 `git = ... rev = ...` 指向修复 commit
    做 CI 验证,再换回版本号 —— 注意 squash 合并并删分支后那个 rev 就 clone 不到了。
12. **Windows 的 python 载荷是根目录下的 `python.exe`**,Linux 是 `bin/python3`。
    删掉 PATH 回退之后这个差异才显形:图标没生成,`assets_summary.h` 为空,
    `gui.assets.cpp` 每个 `svgs::icons::` 都报错。`xpkg_tool` 现在两个名字、两个
    位置都找。
13. **两处端口代码 MSVC 14.52 不接受**,而 Windows 腿自端口落地后一直是红的
    (最后一次绿是 5006775,之前的红是 `xim:python` 当时没有 Windows 构建):
    `renderer.cpp` 的 `.clearValue = {.color = param}` 改成直接取 union 成员
    `param.vk`;`object_pool.ixx` 里为 clang 加的显式实例化
    `template struct any_pool<...>` 让 MSVC 在 `label.ixx` 调用它的成员模板时报
    C2672 加 `<end Parse>`,改成 `#if defined(__clang__)`。
14. **上游 xmake 的 `build` job 今天也红**,原因是它的 Setup Slang 步骤用匿名 GitHub
    API 查 latest release,撞了 runner IP 的限流;与本 PR 无关,重跑即可。

**简洁性自查**(CI 全绿后通读 `mcpp.toml`、`build.mcpp`、两个 workflow):

- `build.mcpp` 里剩下的每一块都是 mcpp 没有对应键的事:submodule 补丁、图标归一化
  和 bin2c、一次规则调用、两个打包成员的声明。没有 shell 分叉、没有 PATH 探测、
  没有依赖机器状态的路径。可以再删的只有注释里对旧着色器流程的叙述,已删。
- `mcpp.toml` 全是数据;唯一「聪明」的地方是带约束的 glob 加四条排除,替代方案是
  逐个列 16 个文件,更长不更清楚,保留。
- 两个 workflow 各只装一样东西(mcpp),其余由 manifest 供给;断言只剩「图标存在」
  和「工具链是钉的那个」,都是 mcpp 自己说不出口的事。
- 没做的:把图标也交给插件(§4.3 的理由不变)。

15. **图标生成器换成 `mcpp/svg_outline`**(见 §4.3):python、node、npx、
    `svg_normalize.py` 全部退出 mcpp 这条链;`build.mcpp` 285 行。

尚未定的:LTO、`bmi_schedule`。`bmi_schedule` 在本机量了一次,**读数无效**:
`mcpp clean` 只清 `target/`,全局构建缓存仍然把绝大多数目标文件直接交回来
(off 25.3 s / on 27.1 s,但 on 的 CPU 时间翻倍,说明它量的是指纹变化带来的
缓存未命中,不是调度)。要量真的冷构建得 `[build] cache = "off"` 跑两遍,放在
CI 绿之后单独做。LTO 在 MSVC 上只能在 CI 量。两者都不随本次改动开启。
