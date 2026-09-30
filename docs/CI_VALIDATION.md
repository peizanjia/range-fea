# Windows 回归验证

独立的 **Windows Regression (unsigned)** 工作流会在拉取请求、推送和手动触发时运行。它使用仓库现有的 Qt 6.11.1 和 MSVC 2022 配置，安装 Python 3.13 以及 Stage 9 所需的 NumPy/SciPy，然后以 `RANGE_REQUIRE_NUMERIC_TESTS=ON` 配置并构建 Release 版本。该任务仅有仓库读取权限，不读取仓库密钥，不签名或发布安装包，并始终上传 CMake、构建、CTest 测试清单和测试日志。

数值测试依赖检查默认关闭，供本地开发按需启用。启用后，CMake 要求同时设置 `RANGE_BUILD_TESTS=ON`，找到 Python 3.10 或更高版本，并能成功导入 NumPy 和 SciPy。未启用时，如果缺少这些 Python 包，本地配置仍可跳过数值集成测试。

## 本地复现

请在已配置 MSVC 2022 的 PowerShell 会话中运行，并确保可使用 Ninja 和 Qt 6.11 或更高版本。如果 CMake 未能自动找到 Qt，请将 `CMAKE_PREFIX_PATH` 设为 Qt 安装前缀。下面使用独立构建目录，配置与 CI 相同：

如果 MSVC LLVM OpenMP 运行库 `libomp140.x86_64.dll` 不在 `System32`，请将 `RANGE_MSVC_LLVM_OPENMP_RUNTIME` 指向 Visual C++ Redistributable 或 MSVC 工具链中的该文件。

```powershell
python -m pip install -r src/fea-solver/examples/stage9/stage9-requirements.txt
$pythonExe = (Get-Command python -ErrorAction Stop).Source
cmake -S src -B build/ci-windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DRANGE_BUILD_TESTS=ON `
  -DRANGE_REQUIRE_NUMERIC_TESTS=ON `
  "-DPython3_EXECUTABLE=$pythonExe"
cmake --build build/ci-windows --config Release --parallel 2
New-Item -ItemType Directory -Force build/ci-windows/ci-logs | Out-Null
python scripts/ci/verify_required_ctest.py `
  --build-dir build/ci-windows `
  --configuration Release `
  --manifest scripts/ci/required-ctest-tests.json `
  --inventory-out build/ci-windows/ci-logs/ctest-inventory.json
$env:OMP_NUM_THREADS = '4'
ctest --test-dir build/ci-windows -C Release `
  --no-tests=error --output-on-failure -j2 --timeout 180
```

测试清单校验器会将 CTest `--show-only=json-v1` 输出中的测试名称与必需测试清单逐项比较。缺少必需测试、必需测试被禁用、清单格式错误或为空、CTest 清单格式错误或为空，以及 CTest 查询失败，都会导致校验失败。额外注册的测试可以保留，因此新增普通测试无需同步修改总数。当前必需清单包含启用数值依赖时的 91 个测试，以及 `tst_model_command_session`、`tst_model_command_cli` 和 `tst_modal_command_widget`。

该工作流不验证可选的 PyTorch 训练或学习策略有限元集成、MATLAB 实机控制器测试以及可选的 Gmsh 运行时。这些功能需要独立环境和验证。
