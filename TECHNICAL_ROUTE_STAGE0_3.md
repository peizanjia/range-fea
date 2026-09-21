# Stage 0—3 技术路线与主要算法实现报告

状态：Stage 0、Stage 1、Stage 2、Stage 3 已完成当前规划范围  
更新时间：2026-09-21

## 1. 目标与范围

前三个开发阶段不是把程序改造成只支持结构分析的单用途软件，而是在保留原有热、流体、电磁、声学等能力的前提下，建立一条可验证、可扩展的结构有限元主线：

```text
可重现构建与测试
  -> 能力注册与产品边界
  -> 统一自由度、单元、材料接口
  -> 一维/二维/三维成熟网格后端
  -> 约束、载荷、动力学与控制耦合
```

当前直接成果是：软件核心已经能够在不依赖 GUI 的情况下描述常用一阶/二阶杆梁单元、平面/轴对称单元和一阶实体单元，生成简单杆、梁、板/壳中面和实体所需网格，并由中央能力注册表说明哪些能力已实现、测试、验证和对产品暴露。

## 2. Stage 0：构建、测试与交付基线

### 2.1 运行时部署

Windows 构建把以下运行时作为显式交付依赖处理：Qt GUI/OpenGL/网络及测试运行时、MSVC 运行时、LLVM OpenMP 运行时，以及启用 Gmsh 时的 `gmsh-4.15.dll` 与独立许可证。

测试程序与正式 GUI/CLI 使用相同的部署逻辑，修复了 `tst_structural_product_profile.exe` 因找不到 `Qt6Core.dll` 而反复弹窗的问题。ZIP、Inno Setup 安装器、静默安装/卸载、独立安装树 CLI 与隐藏 GUI 冒烟已经在当前 Windows 主机验证；正式发布前仍需在不安装开发工具的干净 Windows 虚拟机终验。

### 2.2 数值与测试门禁

采用“单元测试 + 解析解 + 收敛证据”的门禁，而不是只检查求解是否结束：

1. 数据契约：序列化、单位、节点顺序、自由度和能力元数据；
2. 单元级：形函数分片一致性、导数和、Jacobian、质量矩阵和拓扑；
3. 公式级：patch test、热膨胀、刚度/质量对称性；
4. 系统级：杆、梁、膜、厚壁圆筒和实体解析解；
5. 产品级：structural profile 只暴露完成验证的组合。

## 3. Stage 1：能力注册中心与软件边界

`RCapabilityRegistry` 是 GUI、CLI、后续 Agent 和求解器共同查询的单一事实来源。每项能力分开记录：

- `registered`：文件和模型层认识该概念；
- `implemented`：存在实际算法路径；
- `tested`：有自动化测试；
- `validated`：有解析解、独立基准或收敛证据；
- `exposed`：当前产品配置允许用户使用。

这种分层避免“枚举里有名字就声称支持”，也避免 structural profile 全局禁用原有非结构功能。`RSolver` 默认仍使用全部已注册求解器，结构工作区只是显式可选策略。

## 4. Stage 2：统一单元信息与求解内核

### 4.1 自由度与方程编号

`RDofDescriptor` 为 `UX/UY/UZ/RX/RY/RZ`、温度、电势和压力提供稳定 ID、单位、物理场与分量。`REquationBook` 使用 `(nodeId, dofType)` 建立连续方程号，不再假设地址恒为 `3*node+component`。

因此同一模型可同时存在三平移自由度的连续体节点和六自由度的梁节点。约束消元后会确定性重排，局部坐标变换按每个节点的平移块和转角块分别处理。

### 4.2 统一 ElementDescriptor

每种单元通过 `ElementDescriptor` 声明拓扑、阶次、节点数、自然坐标维数、每节点自由度、可用公式、积分规则、一致/集中质量能力、所需材料、载荷、结果和 capability 状态。求解器据此决定局部矩阵维数、全局方程映射和前置检查。第三方单元或未来 Agent 不需要解析求解器内部的 `switch` 才能知道单元能力。

### 4.3 等参有限元公共算法

对杆、面和体单元统一采用等参映射：

```text
x(xi) = sum_i N_i(xi) * x_i
u(xi) = sum_i N_i(xi) * u_i
J = dx/dxi
dN/dx = dN/dxi * J^-1
```

线弹性单元的一般积分形式为：

```text
K_e = integral(B^T * D * B) dV
M_e = integral(rho * N^T * N) dV
f_e = integral(N^T * b) dV + integral_boundary(N^T * t) dS
```

集中质量由一致质量按既定规则行和化；局部单元矩阵通过块对角旋转矩阵变换到全局坐标。二维厚度只进入刚度、质量和等效载荷积分，不错误地乘入应力恢复。

### 4.4 各单元节点约定、形函数与积分

#### TRUSS1 / BEAM1

两节点线性自然坐标 `xi in [-1,1]`：

```text
N1 = (1-xi)/2
N2 = (1+xi)/2
```

TRUSS1 使用轴向应变 `epsilon = du_local/dx`。BEAM1 是三维 Euler—Bernoulli 框架单元，每节点 `UX/UY/UZ/RX/RY/RZ`，局部刚度包含轴向 `EA/L`、扭转 `GJ/L`，以及两个弯曲平面的 `12EI/L^3`、`6EI/L^2`、`4EI/L`、`2EI/L` 标准子块。

截面契约包括 `A, Iy, Iz, J, Wy, Wz, Wt` 和局部 Y 参考方向。极纤维应力包络为：

```text
sigma_max = |N/A| + |My|/Wy + |Mz|/Wz
tau_t = |T|/Wt
sigma_vm = sqrt(sigma_max^2 + 3*tau_t^2)
```

#### TRUSS2 / BEAM2

三节点顺序统一为 `[端点0, 端点1, 中点]`：

```text
N1 = xi(xi-1)/2
N2 = xi(xi+1)/2
N3 = 1-xi^2
```

采用三点 Gauss 积分。TRUSS2 直接走通用等参杆公式；BEAM2 当前实现为两个精确 BEAM1 跨 `[0,2]` 与 `[2,1]` 的装配，中点是实际六自由度框架节点。这一实现稳定且可验证，但不是单跨高阶剪切可变形梁；Timoshenko 梁属于后续独立公式。

#### TRI3 / TRI6

TRI3 采用面积坐标 `L1=1-xi-eta, L2=xi, L3=eta`，即 `N=[L1,L2,L3]`。TRI6 节点顺序为三个角点加边中点 `(0-1, 1-2, 2-0)`：

```text
N1=L1(2L1-1), N2=L2(2L2-1), N3=L3(2L3-1)
N4=4L1L2,     N5=4L2L3,     N6=4L3L1
```

采用三点三角形积分。TRI3/TRI6 共用平面应力、平面应变和轴对称装配框架。

#### QUAD4 / QUAD9

QUAD4 使用标准双线性形函数 `Ni=(1 +/- xi)(1 +/- eta)/4`。QUAD9 以一维二次 Lagrange 多项式张量积构造，节点顺序为四角点、四条边中点、中心点，采用 `3x3` Gauss 积分。二次单元的边界生成、法向翻转、邻接判断和可视化三角化均保留边中点，不再退化成一阶拓扑。

#### TET4 / WEDGE6 / HEX8

- TET4：四节点线性四面体；
- WEDGE6：三角形三点乘轴向两点，共六点积分；
- HEX8：三线性形函数与 `2x2x2` Gauss 积分。

WEDGE6、HEX8 已覆盖数值体积、外表面生成、仿射场 patch test、质量矩阵和三维轴向解析解。

### 4.5 二维公式

平面应力使用：

```text
D = E/(1-nu^2) * [[1,nu,0],[nu,1,0],[0,0,(1-nu)/2]]
```

平面应变使用：

```text
D = E/((1+nu)(1-2nu))
    * [[1-nu,nu,0],[nu,1-nu,0],[0,0,(1-2nu)/2]]
```

轴对称首版固定全局 XY 为子午面，X 为半径、Y 为旋转轴，体积权重为 `2*pi*r*det(J)*w`，应变向量为 `[epsilon_r, epsilon_z, epsilon_theta, gamma_rz]`。负半径、偏离子午面和退化 Jacobian 会在装配前被拒绝。

### 4.6 材料温度与内部状态接口

`RMaterialEvaluationContext` 通过稳定 `RVariableType` 键传入温度及未来耦合场，同时携带时间、步长、总应变、应变增量和已提交材料点状态。返回值 `RMaterialEvaluationResult` 包含属性值、行为 ID 和试算状态。

当前 `builtin.table.v1` 根据材料属性自身的 key 类型查表插值，并把 committed state 原样复制为 trial state。调用是无副作用的，未来塑性、压电、拉索或损伤本构必须只写试算状态，由非线性迭代在收敛后提交、失败时回滚。当前实现并不宣称已经具备这些非线性本构。

## 5. Stage 3：成熟网格工具接入

### 5.1 后端边界

`RMeshGenerator` 是统一入口，`RMeshGenerationRequest/Result` 不暴露 Gmsh 或 TetGen 类型：

```text
几何/seed/尺寸场/单元族/命名指派
  -> RMeshGenerator
       -> TetGen：既有封闭三角面四面体化
       -> Gmsh 4：线、样条曲线、二维面、映射四边形、三维实体
  -> 节点、单元、命名集合、质量、映射结果
```

现有 TetGen 路径被保留并增加立方体闭合表面回归。新增 Gmsh 4.15.2 是可选 SDK：Windows 官方 SDK 使用 MinGW ABI，因此 MSVC 构建采用 Gmsh 官方提供的 C API C++ 包装层；运行时 DLL 与许可证进入部署/安装规则。未配置 `RANGE_GMSH_ROOT` 时，TetGen 和其他软件功能仍可构建。

### 5.2 一维、二维和三维生成

- 直线按指定划分数或 `ceil(length/targetSize)` 划分，`Progression` 系数实现单向偏置 seed；
- 样条曲线由插值点交给 Gmsh 几何内核离散；
- 一阶/二阶线网格映射为 TRUSS1/TRUSS2 或 BEAM1/BEAM2；
- 非映射矩形区域使用 Gmsh 二维非结构网格，边界曲线作为几何约束；
- 映射区域对相对边设置相同 transfinite seed，对曲面设置 transfinite interpolation；
- 开启 recombination 后生成 QUAD4/QUAD9，否则生成 TRI3/TRI6；
- 三维箱体使用 OpenCASCADE 几何与 Gmsh 四面体算法，映射为 TET4；
- 二次体单元尚未进入 Range 单元枚举，因此请求二阶体网格会明确拒绝，而不是静默降阶。

局部尺寸场使用 Gmsh `Ball` 背景场，多个局部场通过 `Min` 组合。区域和边界使用 physical group，导入后转为不依赖后端的命名集合。

### 5.3 命名集合与重网格映射

网格结果同时返回节点 ID 与目标维单元 ID 的命名集合。材料、约束、传感器和执行器不保存易失的旧单元编号，而保存 `stable assignment id + assignment kind + named set`。重网格后，同名集合存在则转入 `transferredAssignments`，不存在则进入 `rejectedAssignments`，必须由调用者诊断，禁止悄悄丢失载荷或控制对象。

这是当前几何级映射策略；一般 CAD 修改后的最近点/投影映射及误差容差属于后续扩展。

### 5.4 网格质量

后端统一计算：所有积分点的最小 Jacobian、真实拓扑边的最大长宽比 `l_max/l_min`、三角形相对 60 度/四边形相对 90 度的最大角度歪斜、最小角，以及非正 Jacobian 单元数。非正 Jacobian 被计入无效单元，不能用绝对值掩盖方向或翻转问题。

## 6. 当前验证证据

| 能力 | 主要证据 |
| --- | --- |
| DOF/方程 | 混合 3/6 DOF、约束停用后重排、局部变换 |
| 一阶/二阶线面单元 | 分片一致性、导数和、积分权重、拓扑与解析 patch |
| 梁 | 双轴悬臂、端弯矩、指定转角、扭转、模态 |
| 平面/轴对称 | 平面应力/应变解析条带、自由热膨胀、Lamé 圆筒收敛 |
| 实体 | TET4 基线、WEDGE6/HEX8 patch 与轴向解析解 |
| 材料 | 温度插值、缺失字段诊断、提交状态到试算状态传递 |
| TetGen | 闭合立方体表面四面体化 |
| Gmsh | 偏置二次梁线、样条曲线、受约束三角形、映射四边形、箱体四面体、局部尺寸场 |
| 重映射 | 材料/约束/传感器/执行器命名集合成功与拒绝路径 |
| 质量 | Jacobian、长宽比、歪斜度、最小角和无效计数 |

最近一次跨阶段聚焦回归为 5/5 通过：`tst_element`、`tst_material`、`tst_mesh_pipeline`、`tst_structural_product_profile`、`tst_solver_stress`。

## 7. 能力边界与下一阶段

完成 Stage 3 不表示已经复制 Abaqus 的完整前处理：

- 当前 Gmsh 垂直切片覆盖参数化线/曲线、矩形面和箱体，不是完整 CAD 装配/布尔建模器；
- 板和壳当前共享二维中面网格，通用板壳弯曲公式尚未实现；
- 二次实体、接触、大变形、材料非线性仍未实现；
- 命名集合映射已建立稳定契约，一般几何改变后的投影映射仍需扩展；
- GUI 尚未改造成 Abaqus 式模块工作流，Agent 侧栏也按规划留到命令层稳定之后。

下一步是 Stage 4：在现有 DOF、命名集合和材料状态基础上，补齐多点/线性方程约束、参考点与刚性/分布/Tie 耦合、基础激励、初始状态，以及带单位、方向和坐标系元数据的传感器/执行器目录。
