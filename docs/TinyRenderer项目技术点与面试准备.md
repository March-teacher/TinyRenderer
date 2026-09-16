# TinyRenderer 项目技术点、实现原理与面试准备

> 本文以当前仓库源码为准，用于项目复盘、简历描述和图形学/C++ 面试准备。

## 一、项目定位

TinyRenderer 是一个参考 tinyrenderer 教学代码学习和扩展的 C++20 CPU 软件光栅化渲染器。它不调用 OpenGL、DirectX 或 Vulkan，主要流程包括 OBJ/MTL 加载、坐标变换、近平面裁剪、重心坐标光栅化、z-buffer、透视校正插值、着色器接口、法线贴图、阴影贴图、PCF 和 SSAA。几何模板与 TGA 读写在参考代码基础上改写，不能将全部代码表述为独立原创。

项目价值不只是“渲染出一张图”，而是把 GPU 隐藏的固定功能和可编程阶段显式实现，从而能解释一个顶点如何最终成为屏幕像素。

### 30 秒面试介绍

> 我用 C++20 实现了一个不依赖图形 API 的 CPU 软件光栅化渲染器。项目支持 OBJ/MTL、多模型场景、MVP 变换、近平面裁剪、重心坐标光栅化、z-buffer 和透视校正插值，并设计了类似 OpenGL 的 vertex/fragment 着色接口。在着色方面实现了 Blinn-Phong、切线空间法线贴图和两趟 Shadow Mapping，同时使用斜率相关 bias、3×3 PCF 和 SSAA 改善阴影与锯齿。

### 2 分钟展开版本

> 项目分为资源、数学、管线、着色和场景编排几层。`model.cpp` 负责 OBJ/MTL 解析、多边形三角化、缺失法线生成与材质回退；`geometry.h` 提供向量、矩阵、齐次坐标、求逆和逆转置；`our_gl.cpp` 是核心管线，负责相机和投影矩阵、近平面裁剪、屏幕包围盒遍历、重心坐标、深度测试和透视校正；`shaders.cpp` 实现多种着色；`scene.cpp` 编排阴影趟、主渲染趟和 SSAA 降采样。
>
> 实现中比较关键的点有四个：裁剪新顶点携带相对原三角形的重心坐标，使裁剪器无需理解具体 varying；UV、法线等属性使用 `重心权重/w` 后重新归一化，避免透视纹理扭曲；法线使用逆转置变换，法线贴图通过 Darboux/TBN 标架转到世界空间；阴影使用光源视角深度图，并用斜率 bias 处理 shadow acne、3×3 PCF 软化边缘。

## 二、代码架构

| 模块 | 职责 | 面试关键词 |
|---|---|---|
| `geometry.h` | 定长向量/矩阵、点积、叉积、行列式、求逆、逆转置、齐次升降维 | 线性代数、模板 |
| `model.h/.cpp` | OBJ/MTL、索引、三角化、法线生成、材质、AABB、模型变换 | 资源管线、健壮性 |
| `texture.h/.cpp` | TGA 贴图、repeat、双线性采样、法线解码、棋盘格 | 纹理过滤 |
| `our_gl.h/.cpp` | View/Projection/Viewport、裁剪、光栅化、z-buffer、插值、降采样 | 管线核心 |
| `shaders.h/.cpp` | Depth、Flat、Gouraud、Phong、Normal/UV 调试着色 | 可编程管线 |
| `scene.h/.cpp` | 多模型、场景归一化、地板、阴影趟、主趟 | Render Pass |
| `tgaimage.*` | TGA 像素存储、RLE 读写 | 图像格式 |
| `image_io.*` | PNG chunk、CRC-32、Adler-32、deflate stored block | 文件编码 |
| `main.cpp` | 参数解析、Bresenham、线框模式、程序入口 | 工程化 |

```text
main
 └─ Scene：决定渲染几趟、矩阵和输出尺寸
     ├─ Model / Texture：提供几何、材质和纹理
     └─ Shader：计算顶点输出和片元颜色
         └─ our_gl：裁剪、插值、深度测试、光栅化
             └─ geometry：底层数学
```

架构把“提交什么几何”“怎样生成片元”“片元是什么颜色”分开，与真实图形 API 的职责边界相似。

## 三、完整渲染流程

### 1. OBJ/MTL 加载

`Model` 逐行读取 OBJ：`v` 为位置，`vt` 为 UV，`vn` 为法线，`f` 为面，`l` 为折线，`mtllib/usemtl` 负责材质绑定。

OBJ 索引是 1-based，并允许负数表示从数组末尾倒数。代码统一转成 0-based，缺失或非法属性用 `-1`。支持 `v/vt/vn`、`v//vn` 等形式。

多边形使用三角扇拆分：

```text
(v0,v1,v2,v3,v4)
→ (v0,v1,v2), (v0,v2,v3), (v0,v3,v4)
```

这种方式对凸多边形正确，对凹多边形不保证正确；完整方案可用耳切法。

如果 OBJ 缺少法线，项目生成面积加权平滑法线：

```text
n_raw = (p1-p0) × (p2-p0)
```

原始叉积模长等于三角形面积的两倍。将它累加到三个顶点后再归一化，大三角形自然获得更高权重。

### 2. 多模型场景归一化

`Scene::fit_to_unit_cube()` 计算整个场景的 AABB，将中心平移到原点，再按最长轴等比缩放到约 `[-1,1]^3`。

必须对场景整体变换。如果人头和眼球分别归一化，它们会失去原来的相对位置与尺寸。等比缩放而非逐轴缩放，是为了保持模型比例。

### 3. 坐标空间与 MVP

| 空间 | 含义 |
|---|---|
| 模型空间 | OBJ 原始坐标 |
| 世界空间 | 模型放入公共场景后的坐标 |
| 观察空间 | 相机在原点并朝 `-Z` 观察 |
| 裁剪空间 | 投影后、除以 w 前的齐次坐标 |
| NDC | 裁剪坐标除以 w，范围通常为 `[-1,1]^3` |
| 屏幕空间 | x/y 为像素，z 映射到 `[0,1]` |

主变换链：

```text
p_clip   = Projection × ModelView × p_world
p_ndc    = p_clip / p_clip.w
p_screen = Viewport × p_ndc
```

项目已将模型变换烘焙到顶点，主渲染时 `ModelView` 主要承担观察变换。

#### LookAt

```text
z = normalize(eye-center)  // 相机后方
x = normalize(up×z)        // 相机右方
y = normalize(z×x)         // 正交化后的上方
```

输入 up 只是参考方向，不一定与视线正交，所以要用两次叉积重新正交化。先平移 `-eye`，再投影到相机基。

#### 透视投影

透视矩阵令 `w_clip=-z_view`。透视除法让 x/y 除以到相机的距离，产生近大远小。FOV 越大，可见范围越广，透视畸变也越明显。

#### 正交投影

正交投影中 w 保持 1，没有近大远小。项目对平行光阴影使用正交投影，因为平行光线没有汇聚中心。

#### 视口变换

```text
x_screen = (x_ndc+1)×width/2
y_screen = (y_ndc+1)×height/2
z_screen = (z_ndc+1)/2
```

### 4. 近平面裁剪

相机后方顶点可能有 `w<=0`，直接透视除法会导致除零、坐标翻转或巨大三角形。项目使用 Sutherland-Hodgman 裁剪近平面。

近平面条件：

```text
z/w >= -1  ⇒  z+w >= 0
```

令 `d(v)=v.z+v.w`。边穿过平面时：

```text
t = d(current) / (d(current)-d(next))
hit = current + t×(next-current)
```

三角形裁剪后可能得到 0、3 或 4 个顶点，四边形再拆成两个三角形。

本项目的重要设计是让原三角形三个顶点分别携带 `(1,0,0)`、`(0,1,0)`、`(0,0,1)`。裁剪交点同步插值这组重心坐标。光栅化子三角形后，再将权重映射回原三角形，最终仍把原三角形重心坐标交给 `fragment()`。因此裁剪器不必知道 UV、法线等具体 varying，着色器也感知不到裁剪。

### 5. 背面剔除

屏幕空间二倍有向面积：

```text
area2 = (bx-ax)(cy-ay) - (by-ay)(cx-ax)
```

面积为零表示退化三角形，正负表示绕序。项目约定逆时针为正面，开启剔除时丢弃负面积三角形。它主要是 early reject 优化，封闭模型通常可跳过接近一半的面；双面材质、薄片或绕序混乱模型不适合直接剔除。

### 6. 重心坐标光栅化

先求三角形屏幕包围盒并裁到画布内，再遍历每个像素中心 `(x+0.5,y+0.5)`。重心坐标满足：

```text
P = αA + βB + γC
α+β+γ = 1
```

三个分量都非负时，采样点位于三角形内。重心坐标同时用于深度和顶点属性插值。

当前实现使用浮点有向面积，没有 GPU 常见的 fixed-point edge function 与 top-left rule。共享边可能有细微重复覆盖或数值差异，是可继续改进的工程点。

### 7. z-buffer 与 early-z

颜色缓冲和深度缓冲一一对应，深度初值为正无穷，较小 z 更靠近相机：

```text
if candidate_z >= stored_z: discard
else: shade; stored_z=candidate_z; write_color
```

项目先深度测试再执行 `fragment()`，相当于软件 early-z，能避免对遮挡片元执行法线贴图、阴影查询和高光计算。

z-buffer 与三角形提交顺序无关，但透明物体仍需排序和混合，单靠深度测试无法正确处理。

### 8. 透视校正插值

透视投影含除以 w，是非仿射的，因此不能直接用屏幕重心坐标插值 UV、法线和世界位置。正确权重为：

```text
q_i = (λ_i/w_i) / Σ(λ_j/w_j)
attribute = Σ(q_i×attribute_i)
```

省略后，倾斜表面会出现经典的仿射纹理扭曲。

为什么深度不使用这组权重？因为屏幕空间线性变化的是投影后的 `z/w`，而 `screen.z` 已经完成透视除法，所以可直接用屏幕重心坐标线性插值。若插值观察空间原始 z，则需要不同推导。

### 9. 可编程着色器接口

`IShader` 提供：

```cpp
virtual vec4 vertex(int iface, int nthvert) = 0;
virtual bool fragment(const vec3& bar, TGAColor& color) = 0;
```

- `vertex()` 每顶点执行，返回裁剪坐标，并保存 UV、法线、世界位置等 varying；
- 光栅化器生成透视正确的重心坐标；
- `fragment()` 自行插值 varying 并计算颜色；
- 返回 false 可丢弃片元。

它模拟了真实 GPU 的可编程接口。真实 GPU 还会自动完成 varying 插值、导数、并行调度等工作。

## 四、着色模型

### Flat Shading

整个三角形共用几何面法线：

```text
n = normalize((p1-p0)×(p2-p0))
diffuse = max(0,n·l)
```

计算简单、能突出棱面，但不能表现光滑曲面。

### Gouraud Shading

在三个顶点计算光强，片元阶段只插值光强。成本较低，但高光是尖锐的非线性项：高光中心若位于三角形内部而非顶点上，插值可能让它完全消失，结果依赖网格密度。

### Phong Shading + Blinn-Phong Lighting

应区分：Phong Shading 是插值法线后逐片元算光；Phong Lighting 用反射向量算高光。本项目是 Phong Shading 搭配 Blinn-Phong 高光。

```text
n = normalize(αn0+βn1+γn2)
diffuse = max(0,n·l)
v = normalize(eye-world_position)
h = normalize(l+v)
specular = pow(max(0,n·h),shininess)×spec_map
```

插值后的法线必须重新归一化，因为单位向量的线性组合一般不再是单位向量。

最终近似合成：

```text
color = albedo×[ambient+(1-ambient)×diffuse×visibility]
      + Ks×specular×visibility
```

环境光不受阴影影响，因为它粗略模拟间接光。该模型不是能量守恒的 PBR。当前已在光照前将漫反射颜色从 sRGB 解码为线性值，输出前重新编码为 sRGB；纹理过滤与 SSAA 平均仍在编码后的颜色上完成，尚不是全流程线性颜色处理。

## 五、纹理与法线贴图

### 双线性采样

UV 转纹素中心坐标：

```text
x = u×width-0.5
y = (1-v)×height-0.5
```

减 0.5 用于将 UV 换算到纹素中心坐标。`1-v` 用于对应 OBJ 的 UV 方向与 TGA 加载后的顶部行序。项目读取周围 2×2 纹素，先沿 x 再沿 y 插值，并采用 repeat 环绕。

双线性过滤只能重建当前 mip 层，纹理缩小时仍会闪烁或产生摩尔纹；完整方案需要 mipmap、三线性或各向异性过滤。

### 材质回退

查询大致按以下优先级：

```text
当前面的 MTL 贴图
→ 按文件名约定发现的模型贴图
→ 材质常量或默认值
```

这让着色器不必关心资源来源，也让不完整 OBJ 仍能渲染。

### 切线空间法线贴图

```text
n_tangent = normalize(texture.rgb×2-1)
```

切线空间 x/y/z 分别对应 T/B/N。默认法线 `(0,0,1)` 映射到 RGB 后约为 `(0.5,0.5,1)`，所以法线图通常偏蓝。

项目没有读取逐顶点切线，而是根据三角形边、UV 差和当前法线现场构造 Darboux 标架：

```text
e1·T=du1, e2·T=du2, N·T=0
e1·B=dv1, e2·B=dv2, N·B=0
TBN=[T B N]
n_world=normalize(TBN×n_tangent)
```

优点是不增加顶点属性；缺点是当前实现每片元求 3×3 逆矩阵，成本高，对退化/镜像 UV 的处理也较简化。生产实现通常离线生成带 handedness 的逐顶点切线，并在片元阶段正交化。

### 法线为什么乘逆转置

法线必须保持与切向量垂直。若 `t'=Mt`，取 `n'=(M⁻¹)ᵀn`：

```text
n'ᵀt' = nᵀM⁻¹Mt = nᵀt = 0
```

因此非均匀缩放下，法线不能直接乘模型矩阵。项目在 `Model::transform()` 使用逆转置矩阵。

## 六、Shadow Mapping

### 两趟渲染

第一趟从光源视角只渲染深度，并保存：

```text
shadow_matrix = Viewport_light × Projection_light × View_light
```

第二趟将当前片元世界坐标变换到阴影图屏幕空间。若当前深度明显大于阴影图记录的最近深度，说明光源与当前点之间有遮挡物。

平行光使用正交投影。阴影趟关闭背面剔除，因为背面同样会挡光。阴影图外按可见处理，避免出现方形暗边。

### Shadow Acne 与 Bias

阴影图离散采样会让倾斜表面上的主相机片元错误地遮挡自己。项目使用斜率相关偏移：

```text
slope = clamp(1-n·l,0,1)
bias = base_bias×(1+8×slope)
```

表面越倾斜，bias 越大。bias 太小会有 acne，太大产生 Peter Panning，即阴影与物体分离。

### 3×3 PCF

项目对周围 9 个纹素分别做深度比较，再求可见比例。PCF 过滤的是 0/1 比较结果，不是先平均深度再比较。固定 3×3 核只是有限软化，不是物理正确的面积光源软阴影；可进一步实现 Poisson、PCSS、VSM 或 CSM。

## 七、抗锯齿、画线与图像输出

### SSAA

倍率为 s 时，先渲染 `sW×sH`，再平均每个 `s×s` 块。时间与内存近似增长 `s²`。它能同时改善几何、纹理和高光走样，但比只增加覆盖样本的 MSAA 更昂贵。

当前直接平均 8-bit 颜色。严格 gamma-correct 流程应先将 sRGB 转到线性空间平均，再转回 sRGB。

### Bresenham

`main.cpp` 保留四个画线版本：固定 t 采样、按 x 采样、交换主/次轴、最终整数误差累积。Bresenham 的核心是维护理想直线与当前像素的误差，超过阈值时让次轴前进，全程只需整数加减和比较。

### TGA/PNG

TGA 支持 RLE。PNG 编码器手工完成 signature、IHDR/IDAT/IEND、CRC-32、Adler-32 和 zlib/deflate stored block。stored block 只封装不压缩，因此格式正确、零依赖，但文件偏大。

## 八、C++ 与工程技术点

1. `vec<n>`、`mat<r,c>` 使用非类型模板参数，在编译期确定维度；`vec<2/3/4>` 特化提供 x/y/z/w。
2. 行列式使用递归模板，`dt<1>` 是递归终点；逆矩阵通过余子式/伴随矩阵实现，适合小矩阵教学，不适合大矩阵高性能计算。
3. 阴影缓冲和多态着色器使用 `std::unique_ptr`，通过 RAII 明确生命周期；`Lighting::shadow_map` 是不拥有对象的观察指针。
4. 深度缓冲是一维连续数组，通过 `x+y*w` 索引，顺序扫描缓存局部性较好。
5. 面属性索引使用多个并行 vector，类似 Structure of Arrays。
6. 输入处理覆盖 OBJ 正负索引、缺省属性、缺失法线、零光向量、退化三角形、接近零的 w、相机 up 退化和地板 z-fighting。

当前像素热路径仍含虚调用、`pow`、9 次阴影查询和 TBN 逆矩阵，目标偏向清晰而非极限性能。

## 九、复杂度与性能

设三角形数 T，第 i 个三角形包围盒面积为 B_i，SSAA 倍率 s：

- 顶点阶段约 `O(T)`，共享顶点目前按面重复执行；
- 光栅化约 `O(ΣB_i)`，是主要成本；
- 阴影趟增加一次类似光栅化；
- PCF 对每个相关片元增加 9 次深度读取；
- SSAA 使主缓冲、像素扫描和着色成本近似乘 `s²`；
- 空间为主颜色/深度 `O(WHs²)` 加阴影图 `O(S²)`。

推荐优化顺序：

1. 将三角形分桶到互不重叠的 screen tile，多线程处理，避免深度缓冲竞争；
2. 缓存共享顶点变换结果；
3. 把边函数、TBN、材质指针等每三角形不变量移出片元循环；
4. 使用增量 edge function，避免每像素重复算面积；
5. tile 级覆盖和深度粗测；
6. SIMD 一次处理多个像素；
7. mipmap 改善远处走样与缓存访问；
8. 评估片元数学使用 float 而非 double。

并行化时不要直接随意按三角形分线程，因为多个三角形可能竞争同一像素的颜色与深度。tile-based 或屏幕条带划分更易保证互斥；着色器实例中的 varying 也必须线程私有。

## 十、调试思路

### 画面全黑

1. wire 模式验证几何和相机；
2. 关闭背面剔除检查绕序；
3. normal 模式检查法线方向；
4. 提高 ambient 排除光方向；
5. 关闭 shadow 排除阴影矩阵/bias；
6. 检查深度范围和比较方向。

### 纹理歪斜

检查透视校正、裁剪空间 w、TGA 原点与 v 方向、半纹素中心，以及裁剪新顶点的属性重建。

### 法线接缝或黑块

检查插值后归一化、逆转置、OBJ 的独立法线索引、退化 UV 导致 TBN 不可逆、镜像 UV handedness。

### 阴影异常

- 条纹：bias 太小；
- 漂浮：bias 太大；
- 锯齿：分辨率低或 PCF 核小；
- 薄面漏光：阴影趟错误剔除背面；
- 位置错误：空间或矩阵顺序错误；
- 随相机/光移动闪烁：可做 texel snapping。

## 十一、高频面试问答

### 1. 为什么自己写软件光栅化器？

它能把图形 API 隐藏的步骤显式化。自己实现裁剪、重心坐标、深度、属性插值和着色后，可以解释顶点到像素的全过程，并定位纹理扭曲、法线错误和阴影 acne，而不是只会调用 API。

### 2. 最有挑战的部分？

推荐讲“裁剪与 varying 解耦”：裁剪新顶点携带相对原三角形的重心坐标，子三角形光栅化后再映射回原三角形，让通用光栅器无需理解着色器属性。

### 3. 如何验证正确性？

使用 wire、normal、UV、depth 中间量可视化，分别关闭 normal map、specular、shadow 做 A/B 测试；再构造单三角形、相交三角形、跨近平面三角形和棋盘格平面作为最小测试。

### 4. 与 OpenGL 如何对应？

Model 对应几何/纹理资源；`IShader::vertex` 对应顶点着色器；`rasterize` 对应裁剪、透视除法、视口和光栅器；`fragment` 对应片元着色器；RenderTarget 对应 framebuffer；阴影趟和主趟对应两个 render pass。

### 5. 重心坐标有哪些用途？

判断点在三角形内、插值深度和顶点属性，以及在本项目中保存裁剪新顶点相对原三角形的位置。

### 6. 为什么 UV 不能直接线性插值？

透视投影的除以 w 是非线性操作。必须使用 `(λ/w)/Σ(λ/w)` 恢复透视正确权重，否则斜面纹理扭曲。

### 7. Flat、Gouraud、Phong 区别？

Flat 每面一个法线；Gouraud 在顶点计算光照再插值强度；Phong 插值法线后逐片元计算。质量与成本通常依次增加，Gouraud 容易丢失三角形内部高光。

### 8. Phong Shading 与 Phong Lighting 一样吗？

不一样。前者指逐片元插值法线；后者指使用反射向量计算高光。本项目采用 Phong Shading 加 Blinn-Phong 高光。

### 9. 为什么法线贴图偏蓝？

默认切线空间法线为 `(0,0,1)`，映射到 RGB 后为约 `(0.5,0.5,1)`。

### 10. Normal Map 与 Height/Bump Map 区别？

Normal Map 直接存三维法线；Height/Bump Map 存高度，需要通过高度梯度推导法线。MTL 命名可能混用，本项目按法线贴图解码。

### 11. 双线性为什么仍有摩尔纹？

它只过滤当前 mip 层的 2×2 纹素，一个屏幕像素覆盖大量纹素时没有正确低通，需要 mipmap、三线性或各向异性过滤。

### 12. Shadow Mapping 优缺点？

优点是适用任意可光栅化几何，容易接入；缺点是离散采样产生 acne、Peter Panning、锯齿、范围浪费和透视走样。

### 13. PCF 是模糊深度图吗？

不是。PCF 对邻域内多次深度比较的 0/1 结果求平均。先平均深度再比较会混合不同遮挡面的深度，含义不同。

### 14. SSAA 与 MSAA？

SSAA 每子样本完整着色，能改善几何、纹理和 shader aliasing，但成本高；MSAA 主要增加覆盖/深度样本，片元着色次数通常更少，主要改善几何边缘。

### 15. 什么是 z-fighting？

两个面深度极近，量化和浮点误差使它们交替通过测试。可增加间距、polygon offset、改善 near/far、提高精度或使用 reversed-Z。项目让地板略微下沉来避免重合。

### 16. 为什么用 `unique_ptr`？

它表达唯一所有权并用 RAII 自动释放。阴影缓冲和多态着色器都是作用域内独占对象。Lighting 中裸指针只是非拥有观察者，生命周期由 `Scene::render()` 保证。

### 17. 虚函数影响性能吗？

有间接调用成本，但项目优先架构清晰。高性能版本可用模板静态多态或按 shader 特化光栅化循环，让编译器内联片元函数。

### 18. 浮点误差常出现在哪？

退化面积、w 接近零、裁剪交点、共享边重心、深度相等、矩阵求逆和退化 UV 的 TBN。当前用 epsilon 做部分保护，完整实现还需 top-left rule 和统一误差策略。

### 19. 为什么 z-buffer 用一维数组？

连续内存易分配，按行访问缓存友好，避免二维指针的额外间接访问，索引公式也简单。

### 20. 如果继续做，优先加什么？

当前优先完善参数检查、加载失败提示与基础测试，再考虑 mipmap 和透明贴图裁切。已有光照前后的 sRGB 转换，后续可以完善过滤和降采样的线性空间处理。多线程和 PBR 属于进一步学习方向，不作为当前已实现能力。

## 十二、设计取舍与改进方向

| 当前实现 | 原因 | 改进 |
|---|---|---|
| CPU 单线程 | 清晰易调试 | tile 多线程、SIMD |
| 只显式裁近平面 | 先解决 w<=0 | 完整六平面齐次裁剪 |
| 包围盒逐像素 | 简单 | 增量边函数、tile coarse test |
| 每片元构造 TBN | 无需顶点切线 | 离线切线、handedness、正交化 |
| Blinn-Phong | 直观 | Cook-Torrance PBR、IBL |
| 单平行光 | 阴影清晰 | 点光/聚光、多光源、cubemap |
| 固定 3×3 PCF | 成本可控 | Poisson、PCSS、VSM、CSM |
| SSAA | 统一改善走样 | MSAA、TAA、FXAA |
| 无 mipmap | 资源简单 | mip chain、LOD、导数 |
| 光照已做 sRGB 转换，过滤仍在线性化之前 | 保留现有采样接口 | 将纹理过滤与降采样也放在线性空间 |
| 无透明 | 避免排序混合 | transparent pass、OIT |
| 扇形三角化 | 简单 | 耳切法 |
| PNG stored block | 零依赖 | 完整 deflate 或成熟库 |

## 十三、简历描述参考

- 参考 tinyrenderer 教学代码，使用 C++20 学习并扩展 CPU 软件光栅化流程，包含齐次变换、近平面裁剪、背面剔除、重心坐标、深度测试与透视校正插值；
- 实现 OBJ/MTL、多材质、多模型、面积加权法线生成、双线性纹理采样与资源回退；
- 设计类 OpenGL 的 vertex/fragment 接口，实现 Flat、Gouraud、Phong/Blinn-Phong 与切线空间法线贴图；
- 使用两趟渲染实现平行光 Shadow Mapping，通过斜率 bias 缓解自遮蔽，并用 3×3 PCF 软化边缘；
- 实现 SSAA、深度/法线/UV 调试视图，以及零第三方依赖的 TGA/PNG 输出。

简历中的每个动词都应能展开为：为什么做、公式是什么、代码在哪里、可能出什么错、替代方案是什么。

## 十四、复习路线

第一轮，能口述数据流：

```text
OBJ → World → View → Projection → Clip
→ Near Clip → Perspective Divide → Viewport
→ Rasterization → Depth Test → Fragment → Image
```

第二轮，掌握五组公式：叉积法线、LookAt、重心坐标、透视校正、Blinn-Phong/阴影深度比较。

第三轮，深入四个难点：

1. 近平面裁剪为何必要，裁剪后 varying 怎么处理；
2. 为什么属性要透视校正，而投影后深度可屏幕线性插值；
3. TBN 怎么构造，法线为什么乘逆转置；
4. Shadow Mapping 为什么产生 acne，bias 和 PCF 分别解决什么。

第四轮，准备两个工程化改进，并说清瓶颈、修改位置、并发安全和预期收益。

适合白板练习的题：手写有向面积与重心坐标、推导透视校正、证明法线逆转置、写 Bresenham、写 z-buffer 循环、写单平面 Sutherland-Hodgman、画出两趟 Shadow Mapping。

## 十五、项目边界

当前没有实现：GPU 实时窗口、完整六平面裁剪、MSAA/TAA、mipmap、全流程线性颜色过滤与 HDR、PBR/IBL、透明排序混合、骨骼动画、点光 cubemap shadow、凹多边形通用三角化、多线程/SIMD、完整 deflate 压缩。光照前后的 sRGB 转换已经实现。

面试中不要过度声称。更好的表达是：“当前为了突出主光栅化流程选择了 X，它会带来 Y 限制；继续迭代时会在 Z 模块使用 W 方案。”

## 十六、源码阅读顺序

1. `main.cpp`：入口与命令行；
2. `scene.h/.cpp`：一帧的 render pass；
3. `our_gl.h/.cpp`：裁剪、光栅化、深度与插值；
4. `shaders.h/.cpp`：varying 与光照；
5. `model.h/.cpp`：模型和材质来源；
6. `texture.h/.cpp`：纹理过滤；
7. `geometry.h`：底层数学；
8. `image_io.cpp`、`tgaimage.cpp`：图像落盘。

最终应能完整回答：一个 OBJ 顶点经过哪些坐标空间、判断和插值，如何成为 PNG 中的一个 RGB 像素？如果还能解释透视校正、逆转置、阴影 bias 三个“为什么”，项目面试准备就比较扎实。
