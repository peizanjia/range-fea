# Range FEA 结构计算基线

更新：2026-09-17。基线提交：主仓库 `6994854`，所有子模块保持该提交记录的固定版本。

本文只记录已经在本机实际执行的构建、测试和结构计算结果。类型已登记、源码存在、自动测试通过和本机运行通过是不同层次的证据，不能互相替代。

## 1. 已验证环境

- Windows 11 25H2，x64。
- Visual Studio Community 2026 18.10，MSVC `19.51.36257.0`，工具集目录 `14.51.36231`。
- Windows SDK `10.0.26100.0`。
- CMake `4.3.3`，generator 为 `Visual Studio 18 2026`。
- Qt `6.11.1 msvc2022_64`，根目录为 `D:\ProgramData\Qt\6.11.1\msvc2022_64`。
- OpenMP 使用 MSVC 的 LLVM runtime 选项 `-openmp:llvm`。
- CMake 源码根目录是仓库内的 `src`，不是 Git 仓库根目录。

Qt 初始安装缺少 HTTP Server。第零阶段通过 Qt Maintenance Tool 为同一 Qt 6.11.1 安装补齐 `Qt6HttpServer` 和它依赖的 `Qt6WebSockets`，没有升级 Qt 主版本。

## 2. 可复现构建命令

```powershell
$rangeRoot = 'D:\YOGA\Documents\Projects\range-fea'
$qtRoot = 'D:\ProgramData\Qt\6.11.1\msvc2022_64'
$buildRoot = "$rangeRoot\build\vs2026"

cmake -S "$rangeRoot\src" -B $buildRoot `
  -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="$qtRoot" `
  -DQt6_DIR="$qtRoot\lib\cmake\Qt6" `
  -DRANGE_BUILD_TESTS=ON

cmake --build $buildRoot --config Debug --parallel -- /nodeReuse:false
$env:PATH = "$qtRoot\bin;$env:PATH"
ctest --test-dir $buildRoot -C Debug --output-on-failure

cmake --build $buildRoot --config Release --parallel -- /nodeReuse:false
ctest --test-dir $buildRoot -C Release --output-on-failure

cmake --install $buildRoot --config Release `
  --prefix "$rangeRoot\build\stage0-install\release"
```

使用 `/nodeReuse:false` 是为了让命令结束时同步回收 MSBuild 工作节点。不要同时从两个终端或 Visual Studio 对同一配置和同一 build 目录发起构建；两个构建实例同时写 PDB 会触发 MSVC `C1041`。

配置阶段唯一非阻塞提示是未找到 Inno Setup，因此不能生成 Windows 安装器 `.exe`。它不影响编译、CTest、`cmake --install` 或直接运行程序。

## 3. 自动测试结果

配置使用 `RANGE_BUILD_TESTS=ON`。现有 CTest 共 26 项：

- `range-base-lib`：7 项。
- `range-cloud-lib`：4 项。
- `range-model-lib`：8 项。
- `range-solver-lib`：7 项。

实际结果：

| 配置 | 通过 | 失败 | 总耗时 |
|---|---:|---:|---:|
| Debug | 26 | 0 | 27.46 s |
| Release | 26 | 0 | 11.82 s |

结构核心 `tst_solver_stress` 和特征值核心 `tst_eigen_value_solver` 在 Debug 下单独运行也通过。前者覆盖四面体杆静力/模态、桁架静力/模态、von Mises、局部滚子方向、重力反力、局部非零位移和矛盾约束诊断。

## 4. 三组固定结构基线

### 4.1 四面体轴向杆静力

固定入口：`src/range-solver-lib/tests/tst_solver_stress.cpp` 的 `staticAxialBar()`。

- 长度 `2 m`，截面 `0.1 m × 0.1 m`。
- 弹性模量 `2.0e11 Pa`，泊松比 `0`。
- 端部轴向力 `1000 N`。
- 解析位移 `FL/(EA) = 1.0e-6 m`。
- 解析轴向应力和 von Mises 应力 `F/A = 1.0e5 Pa`。
- 固定端合反力 `-1000 N`。
- 测试容差：位移/反力 2%，应力 5%。

### 4.2 四面体轴向杆模态

固定入口：同一测试文件的 `modalAxialBar()`。

- 与静力杆使用相同几何和材料，密度 `8000 kg/m^3`。
- 固支-自由杆一阶轴向频率解析值为 `sqrt(E/rho)/(4L) = 625 Hz`。
- 测试要求频率为有限正数、与解析值误差小于 2%，且振型位移不是全零。

### 4.3 GUI/CLI 三维梁文件

固定输入：`src/fea/data/Beam.rbm`。求解器会改写输入链接并生成结果文件，因此只能对它的副本运行。

Debug 和安装后的 Release 求解器都完成计算：

- 未知量：2250。
- 共轭梯度：635 次迭代收敛。
- 最大位移：约 `1.203e-5 m`。
- 最大 von Mises 应力：约 `5.23987e5 Pa`。
- 结果文件：`Beam-00000.rbm`，入口 `Beam.rbm` 变为结果链链接。

Debug build-tree GUI 和 Release install-tree GUI 均成功读取该结果链。运行日志确认：

- 模型成功加载。
- OpenGL `4.6.0`，renderer 为 Intel Arc Graphics。
- main shader 和 flat shader 均链接成功。

GUI 冒烟由隐藏窗口完成，只证明启动、模型读取和 OpenGL 初始化，不替代人工检查模型树、颜色图例、拾取、相机操作和结果切换。

## 5. 运行方式

build-tree 的 Qt DLL 被部署到配置目录下的 `qtlib` 子目录，Windows loader 不会自动搜索该子目录。直接运行 build-tree 程序或 CTest 前应临时把 Qt `bin` 放到 `PATH`：

```powershell
$env:PATH = 'D:\ProgramData\Qt\6.11.1\msvc2022_64\bin;' + $env:PATH
```

Release install-tree 的 Qt DLL 和插件已经复制到 `bin`，可直接启动：

```powershell
& 'D:\YOGA\Documents\Projects\range-fea\build\stage0-install\release\bin\fea.exe'
```

## 6. 已知限制和下一步

1. `fea-solver.exe` 直接依赖 `libomp140.x86_64.dll`，当前 install-tree 没有携带该文件。本机 `C:\Windows\System32` 已有该 DLL，所以本机验证通过；尚不能据此声明安装目录可在没有 VS/LLVM OpenMP runtime 的干净机器上运行。
2. `windeployqt` 在当前 Release 链接结果中没有部署 Qt HTTP Server/WebSockets；现有 GUI/求解器启动不依赖它们，但后续 agent/HTTP 功能启用后要重新检查安装树。
3. Inno Setup 未安装，因此没有生成正式安装器。
4. 本轮没有在 Visual Studio 图形界面中手工设置并命中断点；命令行 Debug 二进制和 PDB 已生成，但交互式断点仍需一次人工确认。
5. 现有 CTest 没有 GUI 自动化或文件级 golden result；梁模型属于进程、文件 I/O 和数值收敛冒烟。
6. Stage 1 前先修复或明确打包 LLVM OpenMP runtime，再建立 structural-only product profile 和 capability registry。

因此，第零阶段的“配置、Debug/Release 构建、自动测试、CLI 求解、GUI/OpenGL 和安装树本机运行”已经形成可复现基线；“干净机器可分发”和“VS 交互式断点”仍是明确待办。
