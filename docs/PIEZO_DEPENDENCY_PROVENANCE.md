# 压电交付的依赖、实现与 AI 辅助来源

## 2026-10-14 12:10 Agent-OFF 链接成员阻断

最终 v43 Agent-OFF 全新安装的 `inventory-v43-off-final-v1.json` 在 3171 文件、261 目录和 18 项路径／导入／字节检查上 PASS，报告 SHA-256 为 `740fae403c4bfa070d3f80b21851a6c9e41d81f9f9d4055a53f91498bc8b505c`，实际安装树重扫亦匹配。但 Sol medium 对相同构建的 `fea-link.map`（SHA-256 `f231c73dda854f612d466b5da73f00f69e85063bcaec3f274f6b97cd220a5d5d`）独立核对后，发现旧 `rgl_cloud_ai_query_widget.obj`、`rcl_ai_query.obj`、`rcl_cloud_ai_query_request/response.obj` 已实际链接，`RCloudAiQueryWidget::submitQuery` 等符号有 RVA。`RANGE_ENABLE_AGENT=OFF` 只排除新 Agent 路径，未保护旧 Cloud AI 查询链。该包**不得作为无 AI 的发行／GUI 测试 GO**；已启动源码条件编译修补。修补后须在新的构建／安装根产生 map、PE、清单及独立重审。旧报告仍标 `INCOMPLETE_LINK_MAP_NOT_SUPPLIED`，不得改写为完整发行资格。

本文件供 PC-11 源冻结和许可证据审查使用，是当前工作树的可核对记录，不是最终交付证书，也不是法律建议。对应机器可读证据为 [piezo_dependency_provenance.json](evidence/piezo_dependency_provenance.json)。2026-10-12 核对时根仓库 HEAD 为 `9f226d8a42f55029dc661eda14b63042279a5df4`，分支 `feature/structural-profile`；根仓库及 model、solver、GUI 子模块仍有未提交修改。文件哈希仅表示此次观察的字节，不等于最终冻结哈希；最终提交、子模块提交和交付工件摘要由 root 更新。

## 2026-10-14 v43 ON 安装实物盘点增量

`build/vs2026-piezo-msvc-openmp-v43` 的全量 Release 构建退出 0，首次全新 ON 安装位于 `build/audit-20261014/piezo-delivery/install-v43`。`scripts/packaging/inventory_v43.py` 对该安装的 schema-v2 清单 `build/audit-20261014/piezo-delivery/inventory-v43-on-v2.json` 给出 `inventoryStatus=PASS`、3173 个文件、全部 18 项检查 PASS；报告 SHA-256 为 `71b2875746dbc50c260ab18d6b211f3e4af64b7d047e6c33f4414c2737f3227f`。其项目可执行档与本次 Release 同名字节一致，实际 `fea.exe`/`fea-solver.exe` 的 PE 导入含 `VCOMP140.DLL`；安装内 `vcomp140.dll` 与显式提供的本机 VC Redist 来源相同。私有 `python.exe`/`pythonw.exe` 由 `build/private-python-v3/runtime-files.json` 的 SHA 映射供应来源。首次未提供这两项映射的 `inventory-v43-on.json` 为 FAIL，失败记录保留。

此清单明确保留 `releaseQualificationStatus=INCOMPLETE_LINK_MAP_NOT_SUPPLIED`；静态 PE 导入和许可文件存在性也不能证明实际插件加载、全部组件授权或对外分发资格。正在构建的 Agent-OFF 变体尚无新安装清单；上述 ON 安装生成后仍计划修改 GUI 操作逻辑，因此其字节也不是最终同源候选。下文 v42 审查只保留历史背景，其中“尚未替换 OpenMP”一类当时状态不得覆盖本节新证据。

## 2026-10-14 私有安装许可缺口复核

v42 静态安装收据 `build/audit-20261013/piezo-delivery/pc11-install-v42-audit/install-receipt-v42.json` 记录 3167 个文件、484389983 bytes，实际 GUI 仍未运行。安装包已有 Range、Gmsh、TetGen、Qt SDK 通知和 CPython／NumPy／SciPy 完整许可文本；这只支持同机私有开发测试的材料盘点，不是可移交发布资格。`finalFreeze.accepted` 仍为 false，根仓库和子模块也尚未冻结。

按 v42 PE import 表，`fea.exe` 导入 Qt Core/Gui/Multimedia/Network/OpenGL/OpenGLWidgets/PrintSupport/Widgets 与 `libomp140.x86_64.dll`；`fea-solver.exe` 导入 Qt Network/Core、Gmsh 和该 libomp。包内另有 Qt PDF、SVG 等共 10 个 Qt DLL、14 个插件，应按最终安装字节逐项映射许可／通知。`Qt6HttpServer.dll` 未见于 v42 包或 `fea.exe` import，但当前 `range-cloud-lib` 经 `range-gui-lib` 存在 `Qt6::HttpServer` PUBLIC 链接路径，需从最终链接／动态加载证据核实是否进入产品；[Qt 6.11 授权页](https://doc.qt.io/qt-6.11/licensing.html) 将该模块列为 GPLv3。

v42 的 `libomp140.x86_64.dll` 与本机 Visual Studio `debug_nonredist` 副本哈希相同；[Microsoft 2026 再分发清单](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution) 不将该目录列为可再分发运行库，故不能凭此私有安装给出跨机移交许可。`D3Dcompiler_47.dll`、`dxcompiler.dll`、`dxil.dll` 虽有安装文件哈希，但尚未建立每个文件的确切来源版本与适用通知；[DirectXShaderCompiler 官方仓库](https://github.com/microsoft/DirectXShaderCompiler) 的许可不能直接证明包内三份二进制的来源。

2026-10-14 对可替代运行库做了唯读核查：本机 VC Redist x64 中有 `vcomp140.dll`（版本 `14.51.36247.0`，SHA-256 `95d4ce4a6802d1e18b5e0e1722cc30ea72ca7e033f83828f05c0b7b993fe7cbf`），[Microsoft 再分发清单](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)列有 VC OpenMP 包，[CMake InstallRequiredSystemLibraries 文档](https://cmake.org/cmake/help/v3.30/module/InstallRequiredSystemLibraries.html)说明可通过 `CMAKE_INSTALL_OPENMP_LIBRARIES` 纳入安装。但现有源码调用 `omp_get_thread_limit`，并在并行上下文与网格求最大体积处使用两处 `reduction(max:)`；本机 MSVC `omp.h`／`vcomp140.dll` 不提供前者，普通 `/openmp` 的 2.0 支持范围也不包含后者。**只替换 DLL 或编译旗标会导致构建／行为风险**。需局部相容化、全新 Release 构建、PE 导入与并行数值／性能回归后，才能判断是否消除该阻断；当前没有执行这些改动或验证。

2026-10-14 对 v42 shader 二进制补充逐字节追溯：安装的 `D3Dcompiler_47.dll`（SHA-256 `e994847e01a6f1e4cbdc5a864616ac262f67ee4f14db194984661a8d927ab7f4`）、`dxcompiler.dll`（`97abd3b3083607dc29cc3f8eae56c39c349b76ee3c20deeb2f66258708e55bd8`）、`dxil.dll`（`fda4dde92222ddb8564c3173ce48faa56f7e1c7b46b0b67dada1182ee07a4123`）均与 v42 所用 `build/vs2026/Release/qtlib` 的对应文件一致。D3DCompiler 还与 Qt 6.11.1 SDK `bin` 副本同 SHA；DXC/DXIL 的 PE 版本均为 `1.8.2502.11`、Microsoft 签名有效，但目前本机 Qt／WinSDK／VS／Edge 候选中没有同 SHA 的更上游文件。[Qt 6.11.1 windeployqt 源码](https://github.com/qt/qtbase/blob/v6.11.1/src/tools/windeployqt/utils.cpp#L372-L461)对各 DLL 独立按 Windows SDK Redist、Qt bin、PATH 顺序查找；历史 v42 未保存所选来源清单，不能仅凭 Qt kit 中同名／版本或签名推断实际输入。最终重新部署应保存官方 `windeployqt --list source` 的绝对来源路径及 SHA，并对应纳入 [DXC 原版 LICENSE](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.8.2502/LICENSE.TXT)／[ThirdPartyNotices](https://github.com/microsoft/DirectXShaderCompiler/blob/v1.8.2502/ThirdPartyNotices.txt)等适用文件；当前 v42 包中未找到 DXC 专项 notice，故来源／通知门槛仍未关闭。

同日将 [Microsoft 官方 v1.8.2502 发布包](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2502)及其页面链接的 [NuGet 1.8.2502.8 包](https://www.nuget.org/packages/Microsoft.Direct3D.DXC/1.8.2502.8)下载到隔离审计目录，逐个从归档流计算 x64 DLL 的 SHA-256。两包内 `dxcompiler.dll` 都是 17986592 bytes／`f79943b02f73b621421b58a569fd6ee120d0023fb6bfa23844ecad6b00d6a295`，`dxil.dll` 都是 1508896 bytes／`62736e99aa534a942873f92b881a71b2cb6ad62b8d7481ea7a8ad8e9888a8056`，**均与 v42 字节不符**；审计收据为 `build/audit-20261013/piezo-delivery/pc11-dxc-official-v1.8.2502/candidate-comparison.json`。因此不能以版本号相近为由把 v42 两个 DLL 归入这两个官方包，也不能直接套用它们的通知。实际来源和分发材料仍需在下一次部署前闭合。

进一步以与 v42 安装版 `fea.exe` **同 SHA** 的 `build/vs2026-lazy-v41/Release/fea.exe` 调用当前 Qt 6.11.1 `windeployqt --dry-run --list source --force-openssl`：来源清单仅列出 Qt kit 的 `D3Dcompiler_47.dll`，标准错误明确提示当前找不到 `dxcompiler.dll`／`dxil.dll`，仅在应用使用 Direct3D 12 某些功能时才会成为问题。原始输出保存在同一审计目录的 `windeployqt-v42-exe-current-source-list.*.txt`。这是**当前环境的同 EXE 重放**，不是 v42 构建当时的历史来源证明；结合 CMake 从既有 `qtlib` 整目录安装的规则，残留旧部署文件被复制进 v42 是一种待验证解释。最终应在全新空目录部署并保留 `--list source` 收据，检查 DXC/DXIL 是否实际进入新包；如未进入，仍需真实 GUI／绘图／相关渲染路径验证其缺席不影响目标功能。

`range-ai-lib` 没有独立 LICENSE 或 SPDX 声明，其[上游仓库](https://github.com/Range-Software/range-ai-lib)目前也未列许可证。当前 Release `fea.vcxproj` 明确把静态 `range-ai-lib.lib` 列为链接输入，`src/fea/src/agent_widget.cpp` 直接引用其 `RAgent` 符号；v42 安装 `fea.exe` 与该 Release 执行档 SHA 相同。未取得 linker map，故不能逐个证实归档成员，但不能仅凭安装包没有独立 AI DLL 判定 AI 代码未进入产品。根 GPLv3 文本也不能自动代替独立子模块的明确授权。若保留 Agent，需记录适用于该 commit、静态链接及交付方式的授权；否则应构建并验证不含 Agent/AI 库的压电交付变体。上述缺口与完整回归、独立终审均未关闭，PC-11-A01 维持 pending。

## 第三方基线与许可证据

Range FEA 及既有库是本次工作的第三方／既有工程基线。根仓库 `origin` 为 `peizanjia/range-fea`，`upstream` 为 `Range-Software/range-fea`；子模块的实际 remotes 和 HEAD 已逐项记录。`src/Config.cmake` 仍标注作者 Tomáš Šoltys、版本 1.3.0；该标签不是本次 PC-11 已验收工件身份。根 `LICENSE` 与下列六个子模块 `LICENSE` 是 GNU GPL Version 3 文本；不能据此认定其中每个捆绑文件都只有同一种许可。

| 子模块 | 当前 HEAD | 当前工作树 |
|---|---|---|
| range-base-lib | `53dee04a1ffe4b4487535ed554a371f5a68bd10d` | 核对时无修改 |
| range-model-lib | `30dba899a522866886f2e042e611620abda649d6` | 有修改／未跟踪文件 |
| range-solver-lib | `60c05bb5e3d1aac08ab4231e3522db3eaabcf14a` | 有修改／未跟踪文件 |
| range-gui-lib | `a5be2bc36037265ac930438b10aa22c63f0dca8e` | 有修改 |
| range-cloud-lib | `48d34a832582a14be6e6bce042c5f64cf6189771` | 核对时无修改 |
| range-build-tools | `ec4cdb2bba0a3cd7424ac85027f8c300f18d369f` | 核对时无修改 |
| range-ai-lib | `a28bfecbe094d4e15cb5ccfceeb55bb25d21be05` | 核对时无修改；未找到独立 LICENSE |

`range-ai-lib` 缺少独立许可声明，不能自动替它声明 GPL 授权；许可范围核实为待办。它是既有应用中的 AI 接口库，不代表确定性 FE／NumPy／SciPy 后端必须调用在线模型。

Qt 实际构建包为 6.11.1，依据本机 `D:/ProgramData/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake`。本机 `D:/ProgramData/Qt/Licenses/LICENSE` 说明 LGPLv3、GPLv3、商业许可及第三方组件的不同路径。构建组件见 `src/CMakeLists.txt`，包括 Core、Concurrent、HttpServer、Network、Widgets、OpenGL、OpenGLWidgets、Multimedia、PrintSupport、LinguistTools，以及测试用 Test。这里只核实实际版本和总许可文本；最终部署模块／插件、第三方通知、选用许可路径和对应交付义务尚未逐项冻结，不声称拥有商业授权。

其他已存在的网格依赖也不能漏记：`src/TetGen/tetgen.h` 标明 1.6.0；其 `LICENSE` 声明 AGPL-3.0-or-later／商业双许可。配置的 Gmsh SDK 为 4.15.2，本地 `build/deps/gmsh-4.15.2-Windows64-sdk/share/doc/gmsh/LICENSE.txt` 为 GPL-2.0-or-later 并列出特定例外。最终许可证兼容性及分发选择仍需审查；本文件不是完整平台 SBOM。

2026-10-12 补充部署核查：v10 私有安装中的 Qt DLL／插件与本机 SDK 的 SHA-256 相符，但供应商 SPDX 所列 SHA-1 与实际 SDK 字节不符。因此保留 SPDX 作为供应商组件／许可声明，不能把它当作已部署字节的认证。已为下一次安装加入本机同版本 Qt Docs 中 62 个模块／第三方归属说明 HTML，保留原模块目录；安装后的逐字节复核仍待执行。D3DCompiler、DXCompiler／DXIL 的二进制对应通知仍未确认。

当前 `libomp140.x86_64.dll` 与本机 Visual Studio `debug_nonredist` 中的文件逐字节一致。[Visual Studio 2026 发布清单](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution) 将该目录列为开发工具并排除在可分发文件之外；[OpenMP 编译选项说明](https://learn.microsoft.com/en-us/cpp/build/reference/openmp-enable-openmp-2-0-support?view=msvc-170) 也说明这一 LLVM 运行库的分发限制。当前安装仅用于本机开发／测试，不能据此声称已有对外分发授权。已更正 CMake 中误称其为可分发运行库、建议通过普通 VC Redistributable 获取它的说明，未更换运行库或编译选项。

## 私有运行时与实际数值后端

`build/private-python-v2/runtime-lock.json` 与打包锁文件固定 CPython 3.13.5、NumPy 2.2.6、SciPy 1.15.3（win_amd64），每个归档／wheel 的来源 URL 与 SHA-256 均记录在 JSON。实际本地证据为 CPython `LICENSE.txt`、NumPy 和 SciPy 的 `*.dist-info/LICENSE.txt`，保留完整文本而不是只写库名称。NumPy／SciPy 主许可文本为三条款 BSD 型；wheel 的完整通知还包含 OpenBLAS、LAPACK、GCC runtime exception 等条目，sdist-only 条目不能直接等同于已安装组件。

安装文件显示 NumPy wheel 声明 OpenBLAS 0.3.29（64 位整数配置），SciPy wheel 声明 OpenBLAS 0.3.28（32 位整数配置）；依据各包实际 `__config__.py` 与 `numpy.libs`／`scipy.libs` DLL，配置文件只作为构建声明，不冒充本次全部 BLAS 运算的动态追踪。DLL 哈希和完整许可证据见 JSON。

原生 C++ 内核负责实际 FE plant、组装、时间推进、恢复和接受记录。本次本地 Python 源码调用 SciPy 广义模态 `eigh`、ZOH 块指数 `expm`、离散 Riccati `solve_discrete_are`，MPC 调用 `optimize.minimize(method='SLSQP')` 并执行可行性、KKT 和 `nnls` 乘子检查。此处报告看见的调用与源码，不是外部 API 正确性背书，也不把第三方求解器说成自行发明。

旧安装样本 `build/cw-foundation-install-20261011` 中的 Range／CPython／NumPy／SciPy 许可与当前来源逐字节核对，结果写入 JSON；这不是最终 PC-11 私有安装验收。Range 安装的 `doc/LICENSE.txt` 由 `src/range-build-tools/LICENSE.txt.in` 插入根 `LICENSE_TEXT` 并添加应用版本／版权头生成，故其原始字节与根 `LICENSE` 不同是预期结果，不表示丢失许可。实际 v9 安装许可与 `build/vs2026/LICENSE.txt` 一致，并完整包含根许可正文；JSON 分别保留原始比较、生成来源和正文包含检查。CPython／NumPy／SciPy 的完整许可仍按原文件逐字节检查。

私有开发运行时曾存在未验证字节码，诊断见 `build/audit-20261012/piezo-delivery/scientific-runtime-bytecode-20261012-184735-diagnosis.md`。root 于 2026-10-12 18:58:45.4776354+08:00 按已审计清单受控清理，收据记录删除 388 个文件、49 个空目录，剩余字节码条目为 0。收据 `build/audit-20261012/piezo-delivery/runtime-bytecode-cleanup-receipt.json` SHA-256 为 `e24b0f76be711862533f18988d0b5e255b7971fa244585409a4c7d3f3301ef9b`。随后 `python-piezo-final-v9.log` 记录实际 150/150 通过、112.047 s，日志 SHA-256 为 `5372bbcd7f62fb447e80c3413b563b4b303e490a0fe065d5ad9e71b32cfd5621`。这只证明所列清理和 Python 测试，不关闭最终干净安装／科学矩阵／完整回归资格；最终 `cleanlinessAccepted` 仍为 false。任何后续私有 Python 测试／诊断启动都须显式加 `-B`，不放宽字节码拒绝门槛。

实际 v9 安装的 Range 生成许可、Gmsh 及私有 Python 清单中的 10 个许可／notice 文件已找到并核对。Qt 本地供应商的 5 个通用文件和 TetGen 许可尚未复制到该安装样本；待安装规则补齐后需再次检查实际最终包。`D:/ProgramData/Qt/Licenses/Copyright.txt` 是 Kitware/CMake 通知，`COPYING.txt` 是 Apache 2.0 文本，不能把这 5 个文件统称为所有运行时 Qt 模块的许可。SDK `sbom` 包含原始 SPDX／CycloneDX 记录和内嵌 `hasExtractedLicensingInfos` 文本，本机没有单独的 extracted-license 目录。安装这些供应商记录不等于完成实际 DLL／插件的许可映射；本文件仍不声明完整覆盖。

## 本次实现、公式和 AI 辅助的边界

JSON 按 Git 实际状态列出新增候选文件与已修改文件、临时 SHA-256。新增候选覆盖材料／电学端口、轴向及分层梁板内核、原生运行／重启、数值后端、认证结果、GUI 工作台及相应测试；修改文件包括 Range 模型格式、组装／推进、任务管理、结果 broker 等既有入口。已修改文件明确是基线上的派生集成。Git 未跟踪只表示当前未提交，不能作为作者身份或原创性的证明。

[交付合同](PIEZO_CONTROL_DELIVERY.md) 中的 Abaqus、COMSOL、TI 和 MITC4 参考用于公式、假设和背景定位。本任务未重新读取外部全文，因此不据此宣称其内容已逐条验证或没有代码复制。局部解析材料夹具、质量／惯量、刚体与 patch、网格收敛、Riccati／KKT 残差等测试在当前源码中可见；它们体现校核方法，存在测试文件不等于本轮通过，也不证明完全独立的作者来源。

本项目使用 Codex／ChatGPT 辅助需求、编码、测试、诊断、审查和文档，并采用多代理协作。当前可定位会话 ID 和说明写入 JSON；确切模型、提示、工具调用及代理记录尚未冻结，root 应在最终证据中保存可获得的记录。AI 辅助和独立推导意图均不构成 clean-room、零侵权或专有所有权保证；不能把整个 Range 工程、Qt 或 NumPy／SciPy 声称为本次原创。

## 最终冻结前待办

1. 核实 `range-ai-lib` 的明确授权，审查 TetGen／Qt 的许可选择及全部部署组件／插件通知。
2. 在最终私有安装中保留上游版权、完整许可证、第三方通知及适用源码／修改说明；完成分发条件审查。
3. 完成干净运行时、完整回归、独立终审、真实安装 GUI／20k 输出与内存等仍开放的 PC-11 门槛。现有计划快照保留 pending，不用此文档改写阶段完成状态。
4. 由 root 冻结并提交最终根源码／子模块、依赖及 AI 辅助证据，再填写 JSON 的 `finalFreeze`；当前所有 final commit／artifact hash 字段均为 null。

本文件支持准确说明哪些部分来自已有工程、哪些属于此次修改和校核；不认证法律合规，不保证零侵权，也不认证整个工程原创。
