# EasytimeSplat - 4D Gaussian Splatting for Unreal Engine

基于 [PICO Splat](https://github.com/Pico-Developer/splat) 升级的 **4D Gaussian Splatting (4DGS)** 渲染插件，支持动态高斯场景渲染。

---

## 版本说明

| 项目 | 版本 |
|------|------|
| 基础框架 | [PICO Splat](https://github.com/Pico-Developer/splat) |
| UE 版本 | 5.4+ |
| 渲染 API | DirectX 12 / Vulkan |

---

## 功能特性

### 1. 高斯文件格式支持

#### 3D Gaussian Splatting (3DGS)
| 格式 | 扩展名 | 支持状态 | 说明 |
|------|--------|----------|------|
| PLY | `.ply` | ✅ 完全支持 | 标准 3DGS PLY 格式 |
| 自定义 Asset | `.uasset` | ✅ 完全支持 | UE 原生资产格式 |

#### 4D Gaussian Splatting (4DGS)
| 格式 | 扩展名 | 支持状态 | 说明 |
|------|--------|----------|------|
| PLY4 | `.ply4` | ✅ 完全支持 | 4DGS 动态序列格式 |
| 自定义 Asset | `.uasset` | ✅ 完全支持 | UE 原生资产格式（包含时序数据） |

### 2. 渲染特性

#### 核心渲染
| 特性 | 支持状态 | 说明 |
|------|----------|------|
| 各向异性高斯 | ✅ | 支持 3D 椭圆高斯分布 |
| 球谐函数 (SH) | ✅ | DC + SH1/SH2/SH3 实时渲染 |
| 透明度混合 | ✅ | 标准 Alpha 混合渲染 |
| 深度排序 | ✅ | CPU 异步 / GPU 同步双模式 |

#### 4DGS 特性
| 特性 | 支持状态 | 说明 |
|------|----------|------|
| 帧插值 | ✅ | GPU 实时插值任意帧 |
| 自动播放 | ✅ | 30fps 自动播放序列 |
| 生命周期控制 | ✅ | 支持高斯点生命周期渐变 |
| 透明度控制 | ✅ | 整体 Opacity 调节 (0-1) |

#### 变换与缩放
| 特性 | 支持状态 | 说明 |
|------|----------|------|
| Actor Transform | ✅ | 完整的位置/旋转/缩放支持 |
| 高斯缩放 | ✅ | Actor Scale3D 正确影响高斯形状 |
| 相机跟随 | ✅ | 自动视锥剔除和排序 |

### 3. 数据存储格式

#### 压缩格式支持
| 数据类型 | 格式选项 | 默认 | 说明 |
|----------|----------|------|------|
| 位置 | UNorm10 / Float16 / Float32 | UNorm10 | 32-128 bits |
| 协方差 | Float10 / Float16 / Float32 | Float10 | 64-256 bits |
| 深度 | InvertedUInt16 | - | 16 bits 深度排序 |

#### 4DGS 数据结构
```cpp
// 4DGS 资产包含以下时序数据
- XYZBank:     位置关键帧数组
- RotBank:     旋转关键帧数组  
- DCBank:      漫反射颜色关键帧数组
- LifetimeMuW: 生命周期参数 (mu, w)
- Scales:      缩放参数
```

### 4. 性能优化

| 特性 | 说明 |
|------|------|
| CPU 异步排序 | 后台线程排序，不阻塞渲染 |
| GPU 同步排序 | 计算着色器实时排序 |
| 视锥剔除 | 自动视锥体裁剪 |
| LOD 支持 | 基于距离的球谐函数级别调整 |

---

## 使用说明

### 导入 4DGS 资产

1. 将 `.ply4` 文件拖入 Content Browser
2. 选择 **Splat4DAsset** 类型导入
3. 自动生成 UE 原生 `.uasset` 文件

### 场景中使用

1. 放置 **Splat4DActor** 到场景
2. 在 Details 面板设置:
   - **Current Frame**: 当前显示帧
   - **Is Play**: 自动播放开关
   - **Max SH Degree**: 球谐函数级别 (0-3)
   - **Opacity**: 整体透明度 (0-1)

### 蓝图控制

```cpp
// 设置当前帧
Splat4DActor->CurrentFrame = 50;

// 控制透明度
Splat4DActor->Opacity = 0.5f;

// 播放控制
Splat4DActor->bIsPlay = true;
```

---

## 技术架构

### 渲染管线
```
[4D Interpolation CS] -> [Transform CS] -> [Sort] -> [Render VS/PS]
     ↑                        ↑              ↑           ↑
  XYZBank/              Position/      Index      Screen
  RotBank/              Covariance     Buffer     Space
  DCBank                Transform
```

### 核心着色器
| 着色器 | 功能 |
|--------|------|
| `Interpolate4DCS` | 4D 数据实时插值 |
| `ComputeTransformCS` | 视图空间变换计算 |
| `ComputeDistanceCS` | 深度距离计算 |
| `RenderSplatVS` | 高斯顶点展开 |
| `RenderSplatPS` | 高斯片段着色 |

---

## 配置选项

### 项目设置 (Engine.ini)
```ini
[/Script/EasytimeSplatRuntime.SplatSettings]
SortingMethod=CPUAsynchronous  ; 或 GPUSynchronous
CovarianceFormat=Float10       ; 协方差精度
PositionFormat=UNorm10         ; 位置精度
SplatRadius=TwoSqrt2           ; 高斯半径 (2√2σ 或 3σ)
```

---

## 已知限制

| 限制 | 说明 |
|------|------|
| 显存占用 | 4DGS 资产较大，建议合理控制序列长度 |
| 移动端 | GPU 排序在部分移动端设备上性能受限 |
| 碰撞检测 | 使用凸包近似，复杂形状可能不精确 |

---

## 许可

基于 [PICO Splat](https://github.com/Pico-Developer/splat) 开发，遵循原始项目许可协议。

Copyright (c) 2025 Easytime Technology Co., Ltd.
