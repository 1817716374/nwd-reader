# nwd-reader

独立的 C++20 Navisworks 文件解析库，面向模型导入、格式转换和工程数据处理，提供**几何、属性、材质、纹理、结构树、保存视点、选择集、动画和施工任务**的读取接口。

支持读取 NWD、NWC 中的嵌入模型，以及 NWF 引用的模型文件。无需安装 Navisworks，不依赖 Autodesk SDK 或运行时。

## 功能

- **几何**：读取三角带、折线、点、文字及圆、圆柱等参数化几何，保留坐标索引、法线、颜色和 UV。
- **实例与变换**：共享几何通过实例引用，保留变换、单位和对象关联，避免按实例复制顶点。
- **属性与结构树**：读取分区、模型层级、属性分组及有类型的属性值，提供子节点和实例查询索引。
- **共享字段定义**：读取 common schema、分区引用及外部几何描述中的有类型字段、GUID和包围盒。
- **材质**：读取环境色、漫反射、高光、自发光、光泽度和透明度，以及资产节点、参数和贴图连接。
- **纹理**：保留内嵌文件的原始字节，支持外部纹理查找、路径重映射和可用状态查询。
- **引用加载**：递归加载 NWF 引用，复用同一源文件，同时保留各次引用的独立节点、分层材质覆盖、对象变换和纹理映射记录。
- **视点与选择集**：读取视点目录、相机与裁剪设置、批注、外观覆盖、静态选择和有类型的搜索条件。
- **动画**：读取场景、轨道、关键帧、时间和插值开关，以及平移、旋转、缩放、颜色、不透明度和相机参数。
- **脚本**：读取脚本启用状态、条件的取反/括号/AND/OR、动作顺序，以及键盘、定时器、变量、碰撞、动画、脚本和热点事件；保留消息、模型加载、变量设置、属性提取、视点和动画动作的数据与共享引用。
- **TimeLiner**：读取任务层级、计划与实际日期、进度、费用、数据源字段映射、仿真外观和列设置，以及旧式任务类型、外观和默认状态定义。
- **碰撞**：读取测试、结果、结果组、规则、审批与模拟事件，以及验证选择中保存的路径编号。
- **轴网**：读取系统坐标框架、轴线直线段和圆弧、楼层标高、活动系统与锁定层。
- **数据库链接**：读取保存的 SQL、连接字符串、开关和字段映射，保留共享名称与原始编码字节。
- **文件数据库**：在内存中读取 SQLite 表结构、行、列、原始单元格类型、外键和定义，不执行源视图或外部数据库查询。
- **空间树**：读取原生空间层级及分片槽引用，保留父子结构并关联模型实例。
- **旧式 Presenter**：读取 LightWorks 实体树、有类型的着色参数、编码图像、灯光、材质槽分配和纹理映射，保留归档内共享身份与路径关联。
- **灯光与文档信息**：读取现代灯光资产、位置/目标点、图纸目录、默认图纸、发布字段、缓存配置、超链接、序列号和几何压缩参数。
- **节点 GUID**：读取 GUID 仓库和独立 GUID 属性，将节点的原始索引关联到来源模型的仓库，支持根、内联分区和共享节点。
- **文档辅助数据**：读取统计报告、节点覆盖、工具与图形设置。
- **读取状态**：保留块目录，区分已解析、部分解析、读取失败和尚未由产品模块处理的内容。
- **并行读取**：支持几何记录、压缩块和属性页并行处理；线程数可配置。

## 相机参数

`CurrentView::parts & 1` 表示保存了相机；保存视点和动画相机帧复用 `Camera` 结构。`projection` 为 0 时是透视，为 1 时是正交。位置和 XYZW 四元数保持文件中的数值。

`aspect_ratio()` 返回宽高比；`height_field()` 在透视下是完整的垂直视角（弧度），在正交下是完整的画面高度；`near_distance()`、`far_distance()` 返回相机中保存的近、远距离。距离与位置保留来源坐标单位，读取相机本身不会将它们转换到 NWF 项目的世界坐标。

`up_offset_factor()`、`right_offset_factor()` 分别是垂直、水平画面偏移系数，乘以对应方向的完整画面尺寸得到偏移量。它们返回 `std::optional<double>`：旧格式未保存字段时为空，保存的零值仍是有效值。原始六项参数同时保留在 `parameters`，修改原始数组会反映到命名访问接口。

## 视点状态

`CurrentView::named_state()` 按文件版本和 `parts` 返回具名状态：世界上方向、焦点距离、线性/角速度、导航工具、倾斜范围、照明、绘制模式、首选视角与图元标志。各字段使用 `std::optional` 区分未保存和有效零值；`serialized_tool` 保留原始工具编号，`tool` 提供转换后的现代编号。

`render_settings` 包含新版视点保存的近远距离及类型、画面适配、水平缩放、光圈直径和快门时间，与 `Camera` 参数分别保留。模式和图元标志使用原始数值；快门时间单位为秒。读取状态不会计算有效渲染相机或执行导航。

距离类型为 0 固定、1 约束、2 自动；画面适配为 0 遮罩、1 完整画面、2 保持垂直范围、3 保持水平范围、4 裁切画面。绘制模式为 0 完整、1 预览、2 着色、3 线框、4 隐藏线。未知的渲染设置模式保留数值。

原始 `state` 数组保持可访问。`named_state()` 按需解释，无堆分配；原数组改变后重新调用即可取得对应值，数组与 `parts`、`wire_version` 不一致时抛出 `nwd::Error`。

`CurrentView::named_viewer()` 读取导航角色的半径、高度、实际高度、眼高偏移、avatar、第一/第三人称模式、两种视角间的角度/距离/倍率，以及碰撞、重力、自动下蹲设置。版本 50 前没有保存重力值和终端速度，版本 55 前没有保存自动下蹲；接口以空 `optional` 表示这些字段缺失。

整个 viewer 缺失时返回空 `optional`。其中 `avatar` 使用借用的 `std::string_view`，保留 NULL 与空字符串区别；使用期间须保留来源视点，并避免修改其字符串存储。`ViewerState::scaled(factor)` 可按显式比例返回尺寸、第三人称距离、重力值和终端速度的缩放副本，保留角度、倍率、标志及缺失字段；比例由调用方提供。

## 裁剪数据

`CurrentView::named_clip_settings()` 返回保存的链接状态、当前平面索引、裁剪范围，以及按版本可选的模式、启用标记、裁剪盒和 XYZW 四元数。只有视点而没有裁剪集合的记录返回空 `optional`。范围与裁剪盒分别保存，向量和四元数保留原值。

`named_clip_plane(index)` 返回 `ClipPlane`：`alignment` 保留原始标识，`value` 在旧格式中是 `LegacyClipPlane`（启用标记、距离、法向及原始辅助数值），新版是 `ClipPlaneFrame`（状态、位置、方向与 X 方向）。新版状态 0 为默认、1 为启用、2 为禁用；集合模式 0 为平面、1 为盒。接口提供保存参数，不自动求有效裁剪几何。

`clip_declared_plane_count` 保存原始计数；旧格式的 0 仍伴随一个独立保存的首平面，实际记录数以 `clip_planes.size()` 为准。动画裁剪关键帧复用相同数组与计数接口，可用 `clip_plane_fields(record, version)`、`clip_settings_fields(settings, version)` 按文档版本取得具名数据。

## 加载后的裁剪与有效平面

`CurrentView::loaded_clipping(profile)` 返回独立的 `LoadedClipSet` 快照；保存视点复用同一接口。动画裁剪关键帧使用 `AnimationKeyFrame::loaded_clipping(wire_version, profile)`，版本来自所属 `Document::version()`。也可调用 `loaded_clip_set(planes, settings, declared_count, wire_version, profile)` 处理原始裁剪数组。裁剪记录完全缺失时返回空 `optional`；存在时必须提供原始计数，避免把旧格式的 0 猜成 1。

`ClipLoadProfile::reader_2017` 和 `reader_2026` 显式选择两种加载规则，不根据文件年份自动选择。支持裁剪线流版本 18 起的字段：旧平面保留原法向长度并补齐未保存的默认槽；116 起的平面按保存方向构建三轴帧。107 前缺少盒，118 前缺少盒朝向。2026 规则还修复现代集合中非空但体积不正的盒及含 NaN 的盒四元数。`box_defaulted`、`orientation_defaulted`、`box_reset`、`orientation_nan_reset` 分别记录默认与修复；`profile`、`wire_version`、`serialized_plane_count` 保留加载来源。原始字段保持可用。

```cpp
if (auto clip = view.loaded_clipping(nwd::ClipLoadProfile::reader_2026)) {
    auto active = nwd::active_clip_planes(*clip);
    for (uint32_t i = 0; i < active.count; ++i) {
        const auto& plane = active.planes[i];
        // dot(plane.normal, position) = plane.distance
        // plane.slot 是原始六平面槽号；保留侧为 >=。
    }
}
```

`active_clip_planes` 返回固定容量六个方程及有效数量。关闭集合、未知模式或空盒返回 0，原始未知模式仍保留在快照中。平面模式只选状态恰为 1 的槽；其法向可能非单位或退化。盒模式返回带朝向的六个面，顺序为 -Y、+Y、-Z、+Z、+X、-X。计算使用快照中的 `box_transform`；保存的非单位四元数沿用加载规则，不擅自归一化。修改快照中的盒或朝向不会自动更新缓存矩阵，建议从原始字段重新加载。

加载和查询成功路径无堆分配，也没有共享可变状态，可由调用方并行处理独立集合。结构不一致、无效读取配置及实际计算所需的非有限/溢出/坍缩几何抛出 `nwd::Error`。输出采用来源坐标系；NWF 最终放置、完整原生变换标志及更早线流仍有限制，不能据此视为所有版本的完整运行时复现。完整用法见 [examples/clipping.cpp](examples/clipping.cpp)：`nwd_clipping_example model.nwd 2026`。

## 分区中保存的方向

`partition_orientation_fields(object, wire_version)` 从已经解码的分区对象返回原始 `up`、`north` 和可选 `front`。根分区可通过 `path_object(model, 0)` 获取；内联分区使用所属对象图中的 type32 对象。版本取自所属 `Scene::version` 或 `Document::version()`，不是产品年份。

```cpp
const auto& partition = nwd::path_object(scene.models.at(0), 0);
const auto directions = nwd::partition_orientation_fields(partition, scene.version);
// directions.up / directions.north: std::array<double, 3>
if (directions.front) {
    // 文件中存在 front；零向量也保留为存在。
}
```

字段访问支持内部线流版本 53 起的已解码布局，109 前 `front` 为空。这个字段范围不扩展整文件读取的兼容范围。返回值保留零向量、非单位向量和非有限值，不进行方向修补、单位换算或 NWF 放置；它表示分区保存的方向，不能直接当作最终项目方向。错误对象类型、版本或数字字段数量抛出 `nwd::Error`，没有序列化内容的合成层级根也会被拒绝。成功路径只复制固定数量的数字，不分配堆内存；独立对象可由调用方并行查询。

分区解码按各字段的线流门槛消费数据：方向标志从 45 起、up/north 从 53 起、源文件名从 57 起、语言信息从 80 起。未保存的字符串槽保留 `nwd::none`，未保存的方向不补进原始数字数组；整数槽中的缺省 0 须结合版本区分是否保存。内部版本 0～27 的已支持对象类别可从分区内嵌场景入口读取；更早格式中的其他类别仍有兼容限制。这些规则针对分区记录，不表示同版本的所有容器、几何和属性布局均已支持。

示例 [examples/partition_orientation.cpp](examples/partition_orientation.cpp) 读取 NWD/NWC 各模型的根分区：`nwd_partition_orientation_example model.nwd`。

## 相机坐标变换

`transformed_camera(camera, matrix)` 接收列主序的 4×4 仿射矩阵，返回相机副本。矩阵应使用相机所在坐标系的单位；调用方负责确定项目放置和单位换算。

函数将输入矩阵作用于相机位置和姿态，并分解组合姿态，支持旋转、非均匀缩放、剪切和镜像。原始 XYZW 四元数参与矩阵构造，不先归一化；返回的四元数表示分解后的正旋转，其符号不固定为 W 非负。正交相机的高度、近距离和远距离乘以组合姿态的三个主尺度绝对值的平均数；透视参数保持原值。宽高比、偏移系数及偏移字段存在性均保留。

```cpp
auto camera_in_target = nwd::transformed_camera(saved_camera, placement);
```

非仿射、非有限位姿、奇异或数值不可靠的分解，以及非有限的变换后距离会抛出 `nwd::Error`。函数无共享可变状态，可由调用方并行处理独立相机；它不自动加载 NWF 放置上下文或变换裁剪集合。完整例子见 [examples/camera.cpp](examples/camera.cpp)。

## 旧版内嵌模型与来源对象

`Document::read_scene()` 支持内部线流版本 0～27 的已支持类别内嵌模型，以及 25～27 的独立几何表引用；NWF 引用加载复用同一入口。这些编号是文件内部版本，不是 Navisworks 产品年份。目前旧布局主要通过构造数据验证，真实旧文件与不同导出器的覆盖仍有限，不能据此推断所有对应版本文件均可读取。

已实现的旧共享数据包括三角带、折线、点、文字、圆、圆柱、外部几何描述、坐标及法线/颜色/UV 数组、几何引用、变换、材质和外观。不同形状复用同一个来源几何时，`Model::geometries` 只保存一份；不同来源对象即使数值相同也保留独立身份。不同几何共享坐标数组时，兼容的 `Geometry` 表仍可能各保存一份数值数组；原始共享关系保留在 `ObjectGraph::geometry_arena`。

`Model::inline_projection` 将分区图内的 `owner` 映射到对应 Model 表的 `value`。每个成功绑定的形状对象生成一个实例，重复空间引用不扩增实例；未绑定形状的映射值是 `nwd::none`。`inline_partition` 保存逻辑路径候选、形状绑定状态和独立的空间出现树。逻辑路径编号、空间出现编号和来源对象编号属于不同的索引空间。访问结构树请使用 `path_reference()`、`path_object()` 和 `ModelIndex`，不要固定假设所有节点都位于 `graphs[1]`。

`graphs[0].geometry_arena` 保留原坐标槽、未使用索引尾和旧材质/外观的附加字段；稀疏表按 `owner` 排序。兼容 Geometry 表只包含实际使用的顶点与带索引前缀。旧外观中尚未确认含义的第二材质槽、布尔字和枚举保留原值，通过 `inline_projection->legacy_appearance_fields` 及场景警告提示未映射部分。

旧模型的逻辑与几何数据交织保存，`Options::metadata=false` 仍会读取解析几何所需的分区图；旧模型的 `geometry_ms` 包含这部分读取和投影时间。共享几何投影可按 `Options::threads` 并行执行。使用示例见 [examples/inline_model.cpp](examples/inline_model.cpp)。

尚未支持的旧对象类别、共享几何类别或编码明确报错；未绑定形状和未消费块保持可检查。整文件兼容性以实际返回状态为准。静态库与调用方需使用同一版本头文件并重新编译。

## 文字、解析几何与外部描述

文字的 `Geometry::text_fields` 保留参数数量、字符串是否为 NULL，以及样式采用共享表索引还是图内对象引用。通过 `resolve_text_style(model, geometry)` 获取样式与状态：`resolved` 表示可用的文字样式，`null_style` 表示空样式，`unavailable` 表示表未加载或索引不可用，`wrong_type` 表示原始引用指向其他类别。返回的图视图依赖所属 Model/Geometry 的有效期；原始引用仍可检查，不能把不同索引空间混用。

内部版本 103 前文字保存 8 个参数，之后为 12 个，均按原始顺序存入 `parameters`。圆的前 9 个参数分别是原点、X 轴和 Y 轴，最后一个是半径；圆柱保存两个原点、两组附加向量和半径。库返回参数，不生成字形或离散曲面；负值、负零及非有限值保留，调用方自行决定处理策略。

`Geometry::external` 保存 loader、format、原始路径、Schema 属性、标志和边界。`null_strings` 区分三个源字符串的 NULL 状态。`payload_decoded` 仅表示描述字段已解码；`unparsed_payload` 保留原始载荷，即使字段已解码也不丢弃。共享描述及其属性按源对象复用。

`RcsReader` 以只读内存映射读取 Autodesk RCS 点云。`index()` 提供 chunk 和 voxel 描述；`raw_points(voxel)` 返回借用的 16 字节点记录视图，`raw_auxiliary_tables(voxel)` 一次返回24张辅助表的零拷贝字节视图（每条记录为小端 u32，条目语义保留为不透明数据），`raw_umbrella_octree_levels()` 返回 chunk 3 的32层级、每条72字节的零拷贝记录视图，`raw_timestamps(voxel)` 返回 chunk 13 中与该 voxel 点序对齐的原始 double 字节，`timestamp(voxel, point)` 按需解码单个值；首个计数字段保留在 `index()`，是否匹配点总数也显式提供，时间单位和基准未推定。`point(voxel, index)` 解出坐标、四通道原始字节、分类及保留的原始属性字。`rgba` 保留文件字节顺序；`color_rgba()` 按 Freedom 原生输出顺序交换中间两字节。`rgba[3]` 同时是原生 intensity 输出启用时使用的原始强度字节，可通过 `raw_intensity()` 读取。`decode_rcs_normal(point.normal)` 按需把14位法线索引解为单位方向。坐标通过 `primary_bounds` 和保存的量化值返回，不生成渲染几何。当前实样验证的是 RCS 3.2；版本门控也接受 RCS 3.1 以上 3.x 和 4.0，但其他版本尚未由真实文件验证。RCP 容器及其自动关联仍未实现。使用示例见 [examples/rcs_reader.cpp](examples/rcs_reader.cpp)。

`ExternalReferenceIndex(table)` 一次建立可复用的路径索引。`resolve()` 检查精确保存字节并保留重复项歧义；`resolve_read_path()` 执行 UTF-8 流的读取规则，返回拥有的 UTF-16 路径、末个同键表项及全部匹配编号。`query_path()` 查询已经转换的 UTF-16 路径。NULL、空路径和无效二进制编码分别保留状态，不做大小写或目录前缀归一化。使用借用视图期间，索引、源表和描述必须保持有效且不变。

`external_reference_scope(scene)` 定位文件根部引用表，返回块编号及可用、缺失、未完整加载或重复根表歧义状态；需要 `Options.products=true`。也可对 `ProductData` 或块目录查询作用域，目录查询不自动读取表。模型前缀的同名块不会覆盖文件根表。

`parse_embedded_reference()` 将 `nwd:///文件#资源` 拆成文件名和 `nwd:` 资源块名，保留 NUL、UTF-16 代理码点和多个 `#` 的读取规则。`expand_embedded_reference_path()` 用明确的当前文件名展开本地占位符。`EmbeddedResourceIndex(document.chunks())` 在明确选定的容器内返回首个匹配块和全部重复块号，再调用 `read_resource()` 获取原始字节，不按同名资源跨文件合并。资源索引借用目录名字，目录需保持有效且不变；匹配编号视图则依赖索引的有效期。

`load_project()` 自动关联 JSON 材质文件项和外部几何描述。`TextureFile.reference` 和 `Project.external_geometry_files` 提供原始路径、读取路径、引用表条目、容器文件、资源块身份及失败诊断。相同源的查询结果与资源缓冲会共享；空映射目标不会自动退回原路径。调用方显式 `remaps` 仍可覆盖文件位置，保存映射的身份保持可查。对已解析到的 `.rcs` 文件，项目加载器默认共享只读 RCS 映射，并用外部几何 Schema 的唯一 `int32 index` 字段关联 voxel 描述；`ExternalGeometryFile::rcs_voxel` 是 RCS 描述表索引，`rcs` 与点数据视图共享映射生命周期。`load_external_rcs=false` 可关闭该步骤，`max_external_rcs_voxels` 限制索引规模。加载状态保存在 `content_status`，未解析引用、索引问题和未支持的外部格式会加入 `warnings` 并使 `Project.complete` 为 false。关联只保留点云数据与外部几何记录身份；调用方仍需依据模型实例和项目上下文处理坐标变换。RCP 及其他插件格式尚未解码。

文件搜索采用调用方配置和库的目录查找策略，尚未完整复现产品的交互搜索、下载、取消及版本相关后处理。ACP 编码模式、异常 JSON 边界、插件协议和旧 Protein 的完整材质语义仍有兼容限制。

旧节点的修改对象记录保存在 `ObjectGraph::legacy_nodes`；旧分区附加字符串及字段存在性保存在 `legacy_partitions`。接口保留源数据，不隐式执行旧修改操作。裸 `decode_geometry()` 沿用现代独立记录约定；读取跨版本文件请使用 `Document`。示例见 [examples/geometry_descriptors.cpp](examples/geometry_descriptors.cpp)。

## 构建

需要 CMake 3.20 及以上、支持 C++20 的编译器和 64 位小端平台。依赖源码已随库提供，构建过程无需联网。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./install
cmake --build build --config Release --parallel
cmake --install build --config Release
```

输出静态库和头文件，并安装 CMake 配置。Windows 可使用 MinGW-w64；源码也包含 MSVC 和 POSIX 平台分支，但暂不保证这些分支的兼容性。静态库与调用方应使用兼容的编译器及 ABI。

## 接入项目

安装后，在调用方的 CMake 项目中使用：

```cmake
find_package(nwd CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE nwd::reader)
```

配置调用方项目时，将安装目录加入 `CMAKE_PREFIX_PATH`。也可以直接将源码作为子目录引入：

```cmake
add_subdirectory(external/nwd-reader)
target_link_libraries(your_target PRIVATE nwd::reader)
```

## 快速开始

`load_project()` 是统一导入入口，接受 NWD、NWC 或 NWF 路径。

```cpp
#include <nwd/reader.hpp>
#include <iostream>

int main() {
    nwd::ProjectOptions options;
    options.reader.threads = 8; // 0：自动选择线程数
    options.search_paths.emplace_back("models");
    options.search_paths.emplace_back("textures");

    auto project = nwd::load_project("model.nwf", options);
    for (const auto& warning : project.warnings)
        std::cerr << warning << '\n';

    for (nwd::Id id = 0; id < project.nodes.size(); ++id) {
        const auto& node = project.nodes[id];
        if (node.model == nwd::none)
            continue;

        const auto& model =
            project.sources.at(node.source).scene->models.at(node.model);
        nwd::ModelIndex index(model);

        for (nwd::Id child : index.children(0)) {
            auto ref = nwd::path_reference(model, child);
            const auto& object = nwd::resolve_object(model, ref);
            auto name = nwd::resolve_string(model.graphs.at(ref.graph), object.name);
            std::cout << name << '\n';
        }
    }
    return project.complete ? 0 : 1;
}
```

完整示例位于 [examples/main.cpp](examples/main.cpp) 和 [examples/project.cpp](examples/project.cpp)，分别展示单文件读取和联合项目导入。

```sh
cmake -S examples -B build-examples -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/install
cmake --build build-examples --config Release --parallel
```

运行 `nwd_example model.nwd` 或 `nwd_project_example model.nwf`。联合导入示例退出码 `3` 表示返回了部分数据及诊断。

## 数据接口

公共接口集中在 [include/nwd/reader.hpp](include/nwd/reader.hpp)。

| 接口或结构 | 用途 |
|---|---|
| `Document::read_scene()` | 读取单个 NWD/NWC 的嵌入模型 |
| `Document::read_nwf()` | 读取 NWF 引用、路径表和外观覆盖信息 |
| `Document::read_products()` | 独立读取视点、选择集、动画、TimeLiner和显示配置，适用于NWD/NWC/NWF |
| `ProductData` / `ProductBlock` | 块目录、共享schema定义、类型化数据、读取状态和未消费尾部 |
| `SavedItems` / `SavedItem` | 保存项层级、注释、GUID、视点、选择、关键帧、任务、图纸、现代灯光和材质资产 |
| `FileDatabase` | SQLite 普通表、列、行、动态单元格类型、DDL 与外键 |
| `SpatialHierarchy` | 紧凑空间树及源分片槽引用；`read_scene()` 可校验模型关联 |
| `PresenterData` / `PresenterLights` | 旧式背景、材质、灯光与纹理映射；归档内对象树和引用 |
| `LightWorksArchive` | 引擎版本、类型定义、字段语义及保留身份的实体记录 |
| `PublishInformation` / `CacheMetadata` | 发布属性与源文件缓存配置 |
| `HyperlinkOverrides` / `NodeOverrides` | 保存的超链接、位置及路径/节点级覆盖记录 |
| `load_project()` | 加载文件及其引用，返回项目节点、共享源和纹理 |
| `Project.sources` / `Project.nodes` | 共享源文件与独立引用出现构成的文件树 |
| `Model` / `ModelIndex` | 模型几何、实例、属性、结构路径及查询索引 |
| `path_reference()` / `resolve_object()` | 从结构路径定位对象，再沿属性引用访问数据 |
| `triangle_indices()` / `attribute_value()` | 读取三角槽索引以及槽对应的法线、颜色或 UV |
| `world_matrix()` / `project_world_matrix()` | 获取源单位矩阵或项目米制矩阵 |
| `appearance_stack()` / `appearance_arena()` | 获取实例基础外观、内到外排列的原始 NWF 覆盖层及各自材质存储区 |
| `effective_appearance()` | 获取最外层原始覆盖引用；不将跨文件的部分字段伪造成合成对象 |
| `describe_asset()` | 获取材质资产节点、参数、连接和纹理 URI |
| `Scene.schemas` / `Model.schema_references` | 共享字段定义及分区引用 |
| `NwfData.option_values` / `NwfReference.cache_plugins` | 缓存插件配置、嵌套选项及枚举对象 |
| `Project.textures` | 获取纹理归属、路径、状态和共享原始字节 |

`PublishInformation::has_option(PublishOption)` 可查询密码标志、打开时显示、到期日期标志、已重存、允许重存、密码提示时显示和嵌入数据库属性等保存配置。`flags` 保留原始位，`unknown_flags()` 返回尚未命名的位；`published` 和 `expires` 保留有符号时间值。密码标志不等于口令内容，此发布信息块不包含可返回的口令字段。

### 脚本与事件对象

脚本位于 `SavedItems.items[i].script`。每个条件的 `event` 和动作列表 `actions` 引用该块的 `SavedItems.objects`，同一个对象的多次引用保持相同 ID。`ObjectGraph.animation_objects` 按 `owner` 保存有类型的事件、动作和热点记录；可用 `animation_object(graph, id)` 查询，借用期间保持图不变。调用前检查 `id != nwd::none`；越界 ID 抛出 `nwd::Error`，普通对象返回空指针。

```cpp
for (const auto& item : saved.items) { // saved 是 nwd::SavedItems
    if (!item.script) continue;
    for (auto id : item.script->actions) {
        if (id == nwd::none) continue;
        const auto* action = nwd::animation_object(saved.objects, id);
        if (!action) continue;
        if (const auto* play = std::get_if<nwd::AnimationPlayRecord>(&action->value))
            std::cout << play->animation_path.second << " " << play->start_time << '\n';
    }
}
```

条件保存 `negated`、`open_bracket`、`close_bracket`、`has_operator` 和 `operator_code`（0=AND、1=OR）。未知模式/操作代码保持原值；有类型的 `Value` 中字符串和对象引用继续使用所属图的编号。`AnimationPlayRecord` 返回结束时暂停开关、起止模式和起止时间（秒）；`AnimationCollisionRecord.gravity` 返回保存的重力选项。解析器返回保存记录，调用方决定如何使用或执行。

`animation_enum_name(AnimationEnum, value)` 查询播放起止、停止、变量、定时器和事件触发模式的稳定名称；未知值返回空视图。例如播放起点值2表示 `current_position`，终点值2表示 `specified_time`，须选择对应枚举域。原始数值保持不变。含非空脚本的原生文件验证仍不足，应检查每块状态。

### 节点 GUID

开启 `Options.products` 后，`NodeGuidIndex` 可按模型编号和路径编号查询节点的 GUID：

```cpp
nwd::Options options;
options.products = true;
auto scene = nwd::Document("model.nwd", options).read_scene();
nwd::NodeGuidIndex identities(scene);
auto identity = identities.resolve(model_id, path_id);
if (identity.status == nwd::NodeGuidStatus::resolved) {
    const auto& bytes = *identity.guid; // 原始16字节GUID
    // identity.index 是源文件中的1起始索引；identity.block 指向产品仓库块。
}
```

索引按来源模型的完整块命名空间绑定仓库，包含根节点和模型内部的分区。建立后可并行只读查询，单次查询不分配内存、不复制GUID。索引与返回指针借用 `scene` 中的数据，使用期间应保持场景及其模型、产品数据存活且不变。

状态区分 `no_index`（源值为0）、`missing_store`（缺少仓库或未启用产品读取）、`unavailable_store`（仓库未完整解码）、`ambiguous_store`（重复命名空间或仓库块）和 `index_out_of_range`。全零GUID仍是有效保存记录。相同GUID不代表同一个路径出现，不能据此合并实例。

`node_guid_index(object)` 返回原始索引；`guid_attribute(object)` 返回type180属性中的GUID，其他类型返回空optional。GUID保留小端源布局：前4、2、2字节为三个整数域，最后8字节保持顺序。接口不执行应用运行时的模型重挂接、GUID重生成或独立对象属性回退；跨模块业务身份仍须分别核对。

### 保存对象之间的名称引用

`SavedItemPathIndex` 可解析动画、脚本、视点和TimeLiner等记录中的元素名称路径。索引属于一个 `ProductData`，查询使用拥有该引用的完整块名确定命名空间，结果给出目标块和 `SavedItem` 编号。

```cpp
nwd::SavedItemPathIndex paths(products); // products 是 nwd::ProductData
auto target = paths.resolve(owner_chunk, reference.first, reference.second);
if (target.status == nwd::SavedItemPathStatus::resolved) {
    const auto& items = std::get<nwd::SavedItems>(products.blocks[target.block].value);
    std::cout << items.items[target.item].name << '\n';
}
```

名称路径以换行符分隔层级，斜杠属于名称；`split_saved_item_path()`可拆分并保留空分量。`SavedItem.name_is_null`区分原始空指针名称与空字符串。同名兄弟遵循保存顺序选择第一个，同时返回 `duplicate_names`；同名元素块则返回 `ambiguous_element`。完全空路径返回 `element_root`，此时没有SavedItem编号。

含空分量的路径在不同原生实现中存在语义差别，默认返回 `context_required`。调用方可通过第四参数选择 `SavedItemPathSemantics::modern`（查找空名称子项）或 `legacy_stop_at_empty`（停在当前对象）。未解码目标返回 `unavailable`，不存在的目标返回 `missing`。索引查询不跨来源文件猜测身份，建立后可并行只读查询。

名称引用查找与几何PathLink验证、动态搜索和GUID关联是独立接口。它不会修改块的解析状态或执行脚本。

### 几何、属性与对象身份

`ModelIndex::children(path)` 返回直接子节点，`instances(path)` 返回直接关联的实例；遍历整棵树时继续访问子节点。实例通过 ID 引用共享几何和外观，应保留这种关联以降低内存占用。

`triangle_indices()` 返回的索引指向顶点槽。位置通过 `geometry.coordinate_indices[slot]` 索引 `geometry.coordinates`，顶点属性通过 `attribute_value(attribute, slot)` 获取，以保留 UV 和法线接缝。

对象属性使用 `(graph, object)` 引用，字符串 ID 只在所属图中有效。`Property.name` 指向同图的名称对象；`Value` 保留字符串、整数、数值、向量或对象引用等源类型。不要跨图直接比较对象 ID。

`Value` 的整数类型还包括 tag14 无符号32位、tag15 有符号64位、tag16 无符号64位。有符号值读取 `integer`，无符号值读取 `unsigned_integer()`；后者复用原有存储位，不增加每个 Value 的内存大小，也不经过浮点转换。

对象引用和字符串视图依赖所属模型的生命周期；查询索引依赖原模型，模型修改后需要重建索引。

`Scene.schemas` 保存字段名、显示名、类型、默认值和嵌套结构。`ExternalGeometry.schema` 与 `Model.schema_references` 使用这个表的零基 ID，`none` 表示空引用。外部描述的 `properties.children` 与定义中的字段顺序对应；`payload_decoded` 表示描述载荷已经读取，外部点坐标是否可用需要另外判断。GUID 保留原始16字节。

NWF 缓存插件及选项保留在引用记录中。`CacheOption.value` 的字符串 ID 和对象引用属于 `NwfData.option_values`，枚举对象继续共享；这些设置仅供调用方读取。

### 旧式 Protein 资产

`Asset.protein` 存在时表示原始 Protein 对象图，此时不要调用只接受 JSON 资产的 `describe_asset()`。`record_graph` 存在时拥有共享图；否则 `graph` 索引所属 `Model.graphs`。`object` 标明资产根，不同根可以共享子资产或形成循环。

`ProteinGraphIndex(graph, graph_id)` 可复用于同图资产：`connections(owner)` 保留属性位置、保存连接 ordinal、目标及启用标记，`native_ordinal` 区分跳过 NULL 后的位置；`uris(owner)` 返回属性内的字符串和嵌入文件身份。`reachable(root)` 每个可到达对象只返回一次，`enabled_path` 表示至少存在一条全部连接启用的路径；禁用路径的记录仍保留。它不执行着色器求值。

`protein_asset_fields(graph, owner, wire_version, graph_id)` 返回库 ID、定义 ID、可选 Schema/GUID、缩略图 URI、分类、关键词、界面名称、默认预览 ID和描述。`protein_schema_fields()` 返回 Schema 标识、资产类型、原始 data 和父引用。列表和 Schema 数据借用原图，调用方应保持图存活且不变。`optional` 区分未保存与保存为空，`Id == none` 保留保存的 NULL；不生成文件中缺失的 GUID，内部 XML 可以通过 `ProteinSchemaDocument` 按需读取。

`ProteinSchemaDocument(data, options)` 读取 Schema XML，拥有不可变的 UTF-8 字符串和有序节点。`entries()` 返回顶层声明，`entry_fields(id)` 返回名称、标签、单位、原始默认值、别名和声明标志；`nodes()` 保留未知属性、重复 ID、子节点及原始字节偏移。复制 document 共享存储，可并发查询；返回的字符串视图须在 document 存活时使用。

`defaults(id)` 解释转义的多值分隔符、布尔值、Integer/Float/Distance 默认值、Color/Vector3 分量以及 Reference 的 texture 标记。颜色按最长分量数组补齐 RGB=0、A=1，向量每轴保留首值并补零；UI 和 implementation 路径不拆分。数值同时返回原始 token、消费长度和转换状态：`defaulted` 表示缺省补值，`exact` 表示完整有限十进制值，`prefix` 表示仅转换了数字前缀，`empty/invalid/out_of_range` 不提供数值。它不复现所有原产品数值转换失败、溢出或非有限数值行为。

`protein_schema_value_type(name)` 返回精确大小写的声明类型编号，未知名称返回空值。`protein_schema_flag_conflicts(type, flags)` 检查原始声明标志的组合，包括多值、连接模式、只读及角色类型冲突；返回位掩码，零表示没有此类冲突。

`protein_schema_prepare_fields(raw, effective_id)` 返回标签和预处理后的声明标志。它按原始 public 标志补齐空标签，再处理 TextureURI、ui、implementation、Distance、Vector3、assettype 等角色及原生长度单位名称。单位匹配区分大小写，不换算数值。该函数无内存分配；返回的标签视图借用输入字段或有效 ID。应先检查原始声明标志，再进行预处理；Base、UID、PropertyAlias 的特殊处理、ID 接受、工厂创建和继承合并是独立步骤。

`constraint_nodes(id)` 按顺序列出进入数值约束读取分支的嵌套节点；`constraint_fields(node)` 返回可选最小/最大值、步长、开闭区间和百分比标记。所有候选节点及其原始文本都保留。

`ProteinSchemaChoiceIndex(document, entry)` 为一个 Choice 声明建立可复用的枚举表。`entries()` 保留所有 ChoiceValue 来源节点、ID、标签、原始数值和接受/拒绝/未决状态；非空 ID 与非负 int32 数值均要求唯一，先接受的条目保留。重复项的 `conflict` 指向较早条目的数组索引。缺少必要属性或数值解释未决时，`complete()` 返回 false，受其影响的后续条目保持未决。

`find_id(id)` 返回 found、missing 或 unresolved；found 的 `entry` 是 `entries()` 下标，不是 XML 节点 ID。此前已确认的条目仍可查询，但不把未决条目当作不存在。索引持有文档的共享副本，构建复杂度为 O(n log n)，查询为 O(log n)，独立查询可并发复用。`ProteinSchemaValueOptions::max_values` 在此限制候选条目数。该索引解释枚举表，不表示属性工厂已经接受声明或默认值。

`ProteinSchemaChoiceIndex::defaults(options)` 将该声明的默认值 token 映射到枚举整数。单值只使用首 token；多值按原顺序保留匹配项和重复项，跳过后续未匹配 ID。返回的每个 token 保留文本、匹配状态和枚举条目下标。`ready` 时 `values` 才是完整结果；首 ID 缺失返回 `missing_first`，枚举表或映射不能确定时返回 `unresolved`，两者均不生成替代默认值。`max_values` 限制全部 token 数，包含单值模式下未使用的后续 token。该方法复用索引，可并发调用；结果仅说明 XML 默认值转换，不代替声明标志/ID 验证、继承或属性注册。

`text_defaults(entry, options)` 返回 String、DateTime、Uuid、URL、TextureURI、ui、implementation、type、localtype、localname 的 XML 工厂文本赋值。`status` 区分 ready、rejected、unsupported；`values` 拥有返回的字符串，数组保留顺序、重复和空 token。`flags` 为 XML 预处理后的标志，`value_type` 为实际值工厂类型（ui/implementation 使用 URL 类型 4）。

String/URL 根据预处理后的 multiple 取全部值或首值；TextureURI 强制数组，String 的 assettype 特例为标量。DateTime 拒绝 multiple，Uuid 工厂只取首值。ui/implementation 将整个 val 保留为一个字符串。`uses_type_default` 表示标量没有 val token，工厂保留类型构造默认值；它与明确赋一个空字符串不同，接口不推断后续类型构造和继承的最终值。该阶段不转换日期/UUID、不规范化 URL；ready 也不代替原始声明标志、ID 或注册验证。所有输入 token（包括被忽略的尾项）都受 max_values 限制。

type/localtype/localname 要求原始 val 非空，然后只取首 token；type 的有效 ID 为 assettype，localtype/localname 使用各自的声明名。缺失 val 和显式空字符串拒绝；非空原文分词后得到空 token 时仍可赋空字符串。三个声明都使用标量字符串工厂，localtype/localname 保留原始 multiple 标志以供调用方另行验证。

`constructed_defaults(entry, options)` 返回 Binary 与 Reference 在 XML 工厂阶段保留的类型构造默认值。`kind` 区分 `empty_binary`、`empty_reference_array`、`empty_texture_link` 和 `unsupported`；`flags` 返回预处理后的声明标志。普通 Reference 是空的 AssetSymbolLink 连接数组，精确指定 type="texture" 的 Reference 是单个空链接。Binary 是空字节缓冲。这些工厂没有从 val 赋入二进制内容或引用目标，因此接口不会将 val 解码为 Base64 或伪造目标身份。忽略的 val 仍按转义分隔规则计入 max_values，但不分配 token 字符串。结果仅描述局部工厂默认值，完整声明验证、继承和后续连接解析须分别处理。

`identity_assignment(entry, current_value, options)` 读取 UID/Base 的局部赋值规则。`kind` 区分 uid、base_schema、unsupported；`status` 区分 ready、invalid_value、redefined、unsupported。两个声明都要求恰好一个 token：UID 要求原始 val 非空，Base 要求解转义后的 token 非空。检查值合法性后，再检查调用方提供的当前值；当前值非空时返回 redefined。返回字符串由结果持有，ready 的 UID 可能为空字符串。ready 的 Base 对应 BaseSchema 字符串属性，flags 为 8；UID 设置定义元数据，flags 为 0。

调用方按自己的定义作用域和接受顺序提供当前 UID 或 BaseSchema 值。该接口不修改文档或调用方状态；它返回本次局部赋值结果，完整定义接受、别名/继承、Base 的 file 加载和最终注册仍需另外处理。输入 token 都计入 max_values，只保留首项文本，拒绝多值时也检查尾项预算。

`ProteinSchemaAliasIndex(document, visible_parent, options, parent_changes)` 建立不可变的 PropertyAlias 索引，并共享文档存储。`entries()` 保留本地声明的源节点、alias/id 原文、接受或拒绝原因及冲突来源；`find(name)` 返回一个目标属性 ID 和对应源文档/节点，按精确名称查询，查询不分配内存。别名名称在当前可见的父级中查重，目标 ID 仅在本地已接受别名中查重；目标属性可以尚未存在。返回视图和文档指针的寿命由索引及其副本维持。

`with_parent_aliases(parent, options)` 返回绑定新父级后的快照，保留已有声明的接受结果和原始冲突来源；本地已接受名称遮蔽后绑定父级的同名别名。构造时就可见的父级参与登记查重，因此这两种加载时序有不同结果。可选的 `parent_changes` 按直接子节点的源顺序指定 `before_entry` 和父级指针，在该节点前切换可见父级；空指针表示解绑，支持 XML 中途加载 Base 的时序。`max_parent_changes` 限制该事件表长度。`max_aliases` 限制本地声明尝试数，`max_visible_aliases` 限制可见名称数，`max_scope_depth` 限制保留的父级和历史依赖深度。每次绑定复制父级可见表并加入本地已接受名称，查询为 O(log n)。

别名查询只展开一层；完整属性查找应先查实际属性，再用别名目标做一次直接属性查询。该索引负责别名登记和父级可见性，普通属性接受、属性值合并、父定义选择、文件加载及最终 Schema 注册仍需分别处理。

`ProteinSchemaDefinitionIndex(document, visible_alias_parent, options, parent_changes)` 将声明接受、UID/Base 局部赋值、别名和值工厂组合为不可变定义快照。`declarations()` 按 XML 顺序返回每条声明的门槛、标志冲突、动作、属性历史索引和候选值；`uid()` 与 `uid_source()` 返回成功赋值的 UID 及其来源。父级别名的可见时点由显式参数控制，后面的别名不会提前影响前面的声明。

`properties()` 包含属性的值来源文档/节点、ID、标签、工厂值类型、标志、容器及覆盖历史。仅 `active` 条目属于当前实际属性表；`container(group)` 返回普通、定义迭代、只读容器内的有效属性索引。`find(name)` 先按这三个容器的顺序查实际属性，再对别名目标做一次直接查询。值以 `shared_ptr<const ProteinSchemaDefinitionValue>` 缓存，调用方可用 `std::visit` 读取布尔、文本、数值、构造默认值或带枚举表的 Choice 投影。索引和副本维持所有返回视图及来源文档的寿命，独立读取可并发进行。

`child.with_inherited_parent(parent, options)` 显式合并父属性并返回新快照。缺失属性共享父值；同名属性保留子值，标志合并为 `parent.flags | (child.flags & 0x180)`，必要时移动容器。继承不会依据新标志重新塑造原值；跨容器同名属性、移动时覆盖的目标，以及旧状态都保留历史索引。后绑定父别名不会重新登记子别名。

`status()` 区分已知声明接受、未知工厂和未决接受。已知拒绝允许继续读取后面的声明；首次值约束是否通过仍未知、Choice 首项缺失或不安全的 BaseSchema 类型会停止后续语义推断，并将后面的声明标为 `unprocessed`。此时已保留前缀仍可检查，未命中的查询返回 `unresolved`。`ready` 仅描述声明接受过程，数值候选仍可能带有独立的转换未决状态。资源限制覆盖声明数、属性历史、缓存值、继承深度、单声明值和别名作用域。

这个索引读取局部定义和显式继承；Base 文件加载、命名父依赖求解、加载后 SchemaVersion 补值及全局 Schema 注册仍需单独处理。

```cpp
nwd::ProteinSchemaDefinitionIndex definition(document);
for (const auto& property : definition.properties()) {
    if (!property.active) continue;
    // property.id / label / flags / document / source 保留属性及其来源。
    std::visit([](const auto& value) { /* 读取具体投影类型 */ }, *property.value);
}
auto found = definition.find("diffuse");
if (found.status == nwd::ProteinSchemaDefinitionLookupStatus::found) {
    const auto& property = definition.properties()[found.property];
    // 使用缓存值；无需重新解析 XML。
}
```

String、DateTime、Uuid、URL 的单值省略 `val` 时，底层内部字符串构造为空。投影保留 `uses_type_default=true` 和空 `values`；显式 `val=""` 则产生一个空字符串赋值 token。两者在兼容比较中按实际空值处理，调用方仍可区分赋值来源。多值 String/URL/TextureURI 省略 `val` 为空数组，显式空值为含一个空字符串的数组。此规则不生成时间戳或16字节 UUID。

`ProteinSchemaPropertyState(index, property)` 提供可独立保留的属性状态，适合在导入或定义处理时组合数据。`target()` 保留初始目标属性的 ID、具体类和定义来源；`flags()`、`label()`、`constraint()`、`choice_table()` 返回当前元数据。`data()` 共享数据的原始不可变缓存投影；URI 更新后的文本通过 `text_value()` 读取，POD 修改后的原始字节通过 `pod_bits()` 读取；不能仅凭 `data()` 指针相同判断值相同。`binding()` 与 `data_binding()` 分别描述目标类和当前数据类型。数组复制携带源元素类型，例如 IntegerList 目标可以保存从 StringList 复制的数据，调用方应按 `data_binding()` 读取。

```cpp
nwd::ProteinSchemaPropertyState target(targetDefinition, targetProperty);
nwd::ProteinSchemaPropertyState source(sourceDefinition, sourceProperty);
auto copied = target.copied_from(source);
if (copied.state) {
    auto state = *copied.state;
    auto payload = state.data();  // shared immutable factory projection
    auto actualType = state.data_binding();
    auto label = state.label();
    auto origin = state.supplier(nwd::ProteinSchemaPropertyField::data);
}
```

复制覆盖数据、标志和标签；源约束或选项表不存在时，目标原有引用保留。各字段通过 `supplier(field)` 保留独立的索引和历史行来源，包含继承标志的原始供应信息，不按同名或相同值合并身份。状态持有文档和值的寿命，连续复制不保留旧状态链，可并发只读使用。`data()` 内的类、默认赋值标记和诊断属于其供应方；当前元数据应读取状态接口。

无法可靠解释的跨底层布局复制返回 `raw_layout_required`，约束是否存在无法确定时返回 `unknown_constraint`，并保留输入。它们是未决情况，不表示原生操作返回拒绝。状态复制不改写原索引，不执行命名空间登记，也不模拟产品分配器或异常展开；完整全局登记仍未实现。

`protein_schema_compare_property_states(&left, &right, skip_type, options)` 比较复制后的当前状态：先检查目标声明类型与跳过类型，再比较当前约束或选项表、原始控制字节和固定目标具体类，最后比较实际数据。应使用这个接口处理 `ProteinSchemaPropertyState`；直接比较其 `data()` 投影会混淆数据供应方的原始类或元数据。

```cpp
auto relation = nwd::protein_schema_compare_property_states(&left, &right);
if (relation.status == nwd::ProteinSchemaComparisonStatus::unresolved) {
    auto reason = relation.issue;
    auto element = relation.ordinal;
}
```

数组比较采用左侧实际元素比较器和右侧实际元素步长，保留有方向性的原生结果。int32、double、Color4 的稳定 POD 布局按需读取，不展开或复制数组；RGB 保留原值，不做 255→1 转换。超出已知数据范围的读取返回 `unsafe_raw_access`；依赖未确定字符串 ABI 的混合读取返回 `raw_layout_required`。这些结果不表示普通的“值不同”。空公共前缀可直接按元素数比较，不猜测未知存储。相等不是对象身份或可用于去重的等价关系。

该接口保留方向性 Choice 前缀、约束空引用、未知数字/单位和资源预算；忽略普通属性比较不使用的 flags、label 和 ID。URI 更新与版本处理分别提供独立接口，由调用方明确组合；容器移动和全局登记仍需另行处理。`control_byte()` 返回独立的原始控制字节，`with_control_byte(value)` 返回修改后的不可变状态。已知的 21 个工厂类初始值均为 1，包括列表；这个字节不是数组开关，其完整字段语义仍未确定。修改保留具体类、数据、flags、标签和约束，普通 `copied_from` 保留目标端控制字节。普通状态比较在值比较前检查字节一致性，Reference/Texture 也经过此检查；类型跳过和约束/Choice 检查仍先执行。

```cpp
auto raw = left.control_byte(); // optional<uint8_t>
auto adjusted = left.with_control_byte(0);
auto origin = adjusted.supplier(nwd::ProteinSchemaPropertyField::control);
// Explicit changes have no indexed-row supplier (origin.definition == nullptr).
```

这个接口保留有证据支持的原始状态行为，不转换标量或数组，也不代表完整版本登记已自动实现。非规范字节 2..255 可按原值保留，但不表示它们是有效的产品 C++ bool 参数。

`protein_schema_apply_ordinary_version(source, target, source_version, target_version, options)` 处理已定位的普通属性对，版本按 `uint32_t` 比较。调用前应完成父定义覆盖判断及旧上下文的元数据修复；它不执行完整定义登记。SchemaVersion、BaseSchema 和 URI 目标返回 `special_branch_required`，需要各自的特殊分支。

版本相等时只比较当前状态；版本不同先要求声明类型、完整 flags 和 type7 的原始单位代码相同。该单位检查不是分量数量检查：ScalarValue、ScalarList 和 Vector3Value 返回单位代码 0，GeneralUnitsValue 使用当前数据的代码，未知单位仍未决。源版本较旧时保留目标，不读取或比较值；较新时先同步控制字节，再比较，值不同时按既有共享复制规则更新。

```cpp
auto applied = nwd::protein_schema_apply_ordinary_version(
    source, target, source_version, target_version);
auto current = applied.target;
if (applied.status == nwd::ProteinSchemaPropertyVersionStatus::unresolved) {
    auto stage = applied.issue;
    auto copy_issue = applied.copy_issue;
    // current retains the known prefix, including an earlier control write.
}
```

结果提供比较诊断、控制字节修改及复制尝试/完成标记；输入和原定义索引保持不可变。如果比较或原始布局复制无法可靠完成，不猜测后续分支，也不把未决当作不兼容。较新分支不会把原生复制回调的返回值额外当成兼容门槛；库无法可靠投影的复制仍明确报告 `copy_unresolved`。此接口不查找属性、不升级 URI、不创建或传播属性，也不模拟产品分配器或异常展开。比较预算耗尽仍抛出 `Error`。

`protein_schema_compare_base_properties(&target, &source, options)` 提供 BaseSchema 的目标优先比较：检查声明类型、控制字节及屏蔽位 8 后的 flags，再按具体类比较 const 值。它跳过属性范围约束，但 Choice 自身的表仍参与比较，且保留有方向性的前缀规则。不能用普通属性比较或只比较选中值替代它。

`protein_schema_reconcile_existing_property(source, target, context, options)` 组合已有属性的处理顺序。SchemaVersion 留给最终版本写入；随后先比较目标与源父属性，相等时直接复制当前源属性；否则再处理 BaseSchema、URI 或普通版本分支。父级覆盖先于这些分支，因此它也可能更新 BaseSchema 或 URI。URI 分支按上下文、控制字节、数量和逐项路径规则更新；全定义登记仍未实现。

```cpp
nwd::ProteinSchemaExistingPropertyContext context;
context.source_parent_property = parent_property; // nullptr means known absent
context.source_version = source_version;
context.target_version = target_version;
auto reconciled = nwd::protein_schema_reconcile_existing_property(
    source, target, context);
auto current = reconciled.target;
auto route = reconciled.route;
```

父级查找不能确定时应设置 `parent_lookup_complete = false`；查找或父比较未决会停止推断。版本为可选值，只在普通分支需要时读取，缺失不会自动当成 1。结果保留父比较、BaseSchema 比较、URI/普通版本结果、复制诊断和已知目标状态；父比较失败时可继续分派，未决时不能冒充“不相等”。比较预算累计覆盖父比较和后续分支，耗尽抛 `Error`。输入保持不可变，成功复制继续共享缓存数据和扁平字段来源。

该入口要求调用方已经完成实际名称查找；源修复可预先完成，也可通过 `context.source_context` 显式提供上下文来组合执行。它不解析父级作用域、不查别名、不创建或传播属性，也不执行最终 SchemaVersion 写入、命名空间接管或产品分配器/异常处理。

`protein_schema_apply_uri(source, target, context, options)` 处理已有 URI 属性的逐项更新。两边声明类型必须为 URI，原始控制字节及值数量相同；不同 URI 具体类可以匹配，例如单值 URL 与含一个元素的 TextureURI。只有目标不是相对路径、源是相对路径时，才写入对应项；其它不相等组合仍可兼容。空文本不是相对路径，首字符 `/` 或 `\`、或含冒号的文本也不是相对路径。该接口不规范化路径、不拼接目录、不检查文件存在。

```cpp
nwd::ProteinSchemaUriContext uriContext;
// Default means the source context is known absent. Set complete=false if
// unresolved; source_context_word retains the actual raw context value.
auto updated = nwd::protein_schema_apply_uri(source, target, uriContext);
if (updated.status == nwd::ProteinSchemaUriStatus::compatible) {
    if (auto count = updated.target.text_value_count()) {
        for (size_t i = 0; i < *count; ++i) {
            auto item = *updated.target.text_value(i);
            auto text = item.value;          // current borrowed bytes
            auto origin = item.supplier;    // retained indexed row
            auto originalOrdinal = item.ordinal;
        }
    }
}
```

`ProteinSchemaUriContext` 保存源定义原始上下文的 `+d0` 单元，不是文件年份或 SchemaVersion。已知上下文缺失时保留空值；信息不足应设 `source_context_complete=false`。上下文值精确为 3、且任一属性 flags 含 0x200 时，额外要求屏蔽 0x204 后的 flags 相同；其它已知上下文不附加这项检查。现有属性组合入口通过 `context.uri_context` 接受相同信息，父级覆盖仍优先于 URI 分支。

URI getter/比较/写入使用首次 NUL 前的 C-string 字节。可见前缀相同时保留目标完整原值；实际写入时仅存源可见前缀。`replaced_ordinals` 返回发生写入的位置，目标类、数据布局、flags、标签和控制字节保持原状。非空的外来 Array 若不能安全按字符串解释，返回 `raw_layout_required`；空 Array 可以在数量检查后完成，不读取元素。

`text_value_count()` 区分不可用/非文本数据与空数组；`text_value(i)` 返回当前值及其原始供应行、ordinal 和可选前缀长度。值和来源视图借用状态寿命。URI 修改以稀疏记录共享原始字符串，批量合并覆盖，不保留旧状态链。`has_text_overrides()` 为真时，`supplier(data)` 没有单一来源，应逐项查询；`data()` 仍是底层原始缓存投影。后续状态比较和 `copied_from()` 均使用或携带有效文本。输入及索引不可变，可并发只读使用。

值/文本预算累计覆盖父比较和 URI 处理，耗尽抛出 `Error`。该逻辑不执行产品所有者通知，也不补齐全定义创建、派生传播和最终 SchemaVersion 写入。

`with_flags(value)` 返回显式写入原始 PropertyBase flags 的不可变状态。它保留类、数据、控制字节和已有 URI/POD 覆盖，不按 flags 重塑数组；显式 flags 没有原 XML 行供应者，普通复制继续携带其当前值。原始声明及文档保持不变。

`protein_schema_repair_source_flags(source, rawContext)` 处理普通源属性的旧上下文修复。`ProteinSchemaSourceContext` 用 absent/present/unresolved 区分上下文存在性，`word_d0`、`word_d8` 分别保留两个原始 64 位单元；它们不是文件年份或 SchemaVersion。仅在精确等于 `(0x100000003, 3)` 时清除当前 flags 的 0x800 位。没有该位或上下文已知不匹配时不改值；确实需要但缺少信息时返回未决，不猜测。

已有属性入口的 `context.source_context` 为可选值：省略时沿用调用方已完成修复的契约；提供时，在 SchemaVersion 提前跳过之后、父级/Base/URI/普通版本比较之前修复 source，并用同一上下文的 d0 为 URI 提供信息。结果中的 `source`、`source_repair` 保留当前源状态和修改诊断。此时独立的 `uri_context` 由该原始上下文取代。

`protein_schema_get_integer(state, ordinal, options)` 和 `protein_schema_set_integer(state, bits, ordinal, options)` 用于 IntegerValue、IntegerList、ChoiceValue、ChoiceListValue 的整数接口。`bits` 保留 int32 的全部位，包括负数位型；接口类别按固定目标类检查，数据按实际布局读取。Array 携带 double/color4 元素时，整数接口读写其前四字节，其余字节、动态类型及数量保留，不能作数值转换。非 POD 字符串 ABI 返回未决。越界读取返回带诊断的 0，越界写入拒绝且不扩容；Choice 写入还要求当前表中存在该数值。

```cpp
auto write = nwd::protein_schema_set_integer(property, 3u, 0);
if (write.written) {
    auto current = write.target;
    auto lowWord = current.pod_bits(0, 4);
    auto patches = current.pod_patches();
    auto original = current.backing_supplier();
}
```

`pod_bits(offset, bytes)` 按小端读取当前 int32/double/color4 存储，每次 1..8 字节，返回位型或明确的未知/越界原因。offset 是逻辑值存储内偏移，不是文件偏移。`pod_patches()` 枚举显式四字节修改，`backing_supplier()` 指向原始缓存来源；修改字节不冒充源 XML 数据。POD 覆盖按偏移扁平保存、共享复制，不保留历史状态链，不复制整个数组。状态比较读取当前字节，完整复制能携带或清除覆盖；原缓存中的转换诊断仍保留。

`protein_schema_read_version(property, lookup_complete, options)` 接收 SchemaVersion 接口查询的结果：先直接名称查询，再扩展一次别名，实际属性 ID 可以不同。已知不存在时默认 1，查找不确定或错误/未知整数接口保持未决；空整数数组返回带诊断的 0，不能误当成默认 1。指针入口要求调用方完成查询，不自行检查属性名称。

`protein_schema_finalize_version(property, source_version, target_version, compatible, lookup_complete, options)` 只在此前兼容且源版本按 uint32 大于目标版本时执行最终步骤。已有属性请求 ordinal0 写入；结果区分 `write_requested` 与 `written`。Choice 不匹配或空 Array 拒写时，局部步骤可为 `finished` 且 `written=false`，不会额外撤销已有兼容判断。`finished` 只描述这个步骤，不代表完整定义登记成功。

低级指针入口在目标缺失时返回 `creation_required`，携带精确的 SchemaVersion 名称、flags2、空标签及原始32位值；该入口本身没有创建属性。局部创建可使用下面的作用域 API；全局接管和派生传播仍需单独实现。查找、整数值或 Choice 表未知时返回明确未决；资源预算耗尽抛 `Error`。这些逻辑不执行产品所有者通知、分配器或异常策略。

`ProteinSchemaDefinitionState` 为已解析定义保留不可变局部运行状态。`definition()` 提供元数据索引，`property(row)` 提供当前字段，`with_property(row, state)` 保存同一目标身份的当前状态；同名的外来属性会被拒绝。原始文档、属性值、POD/URI 覆盖共享，当前 flags 写入不会改变登记时的容器。元数据索引中的已有行仍是原始字段，不能替代当前状态访问。

`find_actual(name)` 只查实际属性；`find(name)` 先查实际名称，再扩展一次别名。两者按普通、iterator、readonly 容器顺序查询，保留跨容器同名身份。别名链不递归，实际属性优先。`ProteinSchemaDefinitionIndex` 也提供这两个查询；接受状态未知时，未知缺失返回 unresolved。

```cpp
nwd::ProteinSchemaDefinitionState current(definition);
const auto target_version = current.read_version();
if (target_version.value) {
    auto result = current.finalize_version(source_version,
                                          *target_version.value,
                                          previously_compatible);
    current = result.state;
    // result.created / result.step.written describe actual local changes.
}
```

`read_version()` 使用作用域内的 SchemaVersion 接口查询。`finalize_version()` 只在此前兼容且源版本按 uint32 更大时工作：命中实际属性或别名目标就复用其整数/Choice setter；确认缺失才生成独立的局部 Integer 属性。生成属性为 Type6、flags2、空标签、readonly 容器和原始 int32 位型，无约束/Choice；`origin=registration_create`、`source=none`，不会借用已有 XML 行作为新身份。重复写入复用生成身份。输入文档不变，也不通过合成 XML 创建属性。

作用域创建成功时，`created=true`，`step.status=finished`、`step.written=true`，`step.target` 为新属性，`step.creation` 保留初始化请求，`step.integer` 为空。已有属性拒写时仍区分 finished 与 written=false；查找或值未知时不创建。参数中的比较预算及定义历史/缓存上限耗尽抛出 `Error`。

已有属性更新只复制稀疏修改表，数组和定义缓存继续共享。单次缺失版本创建复制元数据容器/历史表，成本随属性数增长，值不复制。版本操作不执行普通属性批量创建、全定义兼容判断、产品命名空间接管或派生传播，也不替代全局 Schema 登记。

`current.create_reflected_properties(sources, options)` 按输入顺序执行局部普通反射创建。它保留 source 的固定属性类，分享当前数据、POD/URI 覆盖及独立字段供应者；新属性拥有自有名称、标签和 `registration_create` 身份，`source=none`，上下文属于目标定义，控制字节初始为 1。

```cpp
std::vector<nwd::ProteinSchemaPropertyState> sources;
sources.push_back(source_scope.property(source_row));
auto created = current.create_reflected_properties(sources);
current = created.state;
for (const auto &entry : created.properties) {
    auto property = current.property(entry.property);
    // entry.source_ordinal, entry.container, entry.replaced retain the mapping.
}
```

该入口直接按当前 flags 选择 ordinary/iterator/readonly 容器，0x2 优先于 0x8，不重新执行 XML 声明门槛，也不根据 flags 重塑固定类。它可以替换被选容器内的同名属性；其它容器的同名属性和所有历史行保留。别名不会阻止显式插入。已有身份不变，新行身份独立，不能按名称或数据缓存指针合并。

反射创建的元数据行使用 `constructed_binding` 保存固定类，`value` 为空，明确表示没有投影未观察到的构造初值。通过 `current.property(row).binding()` 读取固定类，通过 `data_binding()`、`data()`、`text_value()`、`pod_bits()` 读取当前数据。直接从元数据行重新构造 PropertyState 只得到带固定类的未赋值投影，不会自动恢复作用域中保存的当前值。

每批只构建一次元数据索引，所有新身份共享同一完成快照，数据和扁平覆盖表继续共享。`properties` 返回已完成行及同容器被替换行，`next_source` 是首个未应用输入或输入数量。复制布局或约束存在性未知时，`issue` 保留原因并返回已完成前缀；这表示本库没有应用该项，不表示产品拒绝创建。

`ProteinSchemaReflectedCreationOptions::definition` 限制整批来源数、总历史及保留缓存值，独立 Choice 表也计入预算；`max_generated_text_bytes` 限制自有生成 ID/标签的累计历史字节。资源耗尽抛出 `Error`，原始输入不变。这些上限是本库资源策略，不模拟产品异常展开或回滚。

调用方仍须完成 SchemaVersion 源跳过、旧上下文修复和接收定义的选择；这个方法不自动执行派生传播、完整定义兼容判断或全局接管/释放。它不依赖 Autodesk DLL，也不通过合成 XML 创建目标。

`ProteinSchemaDefinitionGraph` 保存一组有独立身份的定义状态及显式父关系。每个图行代表一个作用域，名称、UID、XML 或值缓存相同不会合并。对不同定义应分别构造 `ProteinSchemaDefinitionState`；复制一个状态表示同一作用域身份，图构造器拒绝把它重复放入两行。namespace 中重复出现同一对象时，重复使用该图行 ID。

```cpp
using Node = nwd::ProteinSchemaDefinitionGraphNode;
std::vector<Node> nodes{
    {nwd::ProteinSchemaDefinitionState(root_definition), {nwd::none, true, 90}},
    {nwd::ProteinSchemaDefinitionState(child_definition), {0, true, 10}}
};
nwd::ProteinSchemaDefinitionGraph graph(nodes);
std::array<nwd::Id, 1> namespace_entries{1};
auto propagated = graph.create_reflected_properties(0, namespace_entries, sources);
graph = propagated.graph;
```

父关系的 `parent_known=false` 区分未解析父级与已知无父级。`select_receivers(target, namespace_entries, options)` 包含 target，以及能够沿父链到达 target 的枚举对象和中间祖先；中间祖先无需直接出现在 namespace 枚举中。重复枚举按图身份去重。查询复用不可变父链结果，避免物化重复祖先链；被访问的循环、缺失节点或父关系返回明确未决，未访问的循环不触发推断。

`namespace_complete=false` 表示仅提供了枚举前缀。未决结果中的 `receivers` 是已知正向集合，按图行顺序列出，不是完整原生事件前缀。传播在选择未决时不修改任何定义。`max_namespace_entries` 和 `max_parent_edges` 限制枚举及优化后的独立父边读取，后者不是产品回调次数。

默认 `unsigned_keys` 按调用方提供的独立 uint64 `order_key` 排序；多接收者缺键或键冲突会保持未决，不把图行号或保存文件 ID 猜成指针。示例中的键仅演示排序。可显式选择 `ProteinSchemaReceiverOrder::graph_row`，按图行顺序执行确定性的库策略。单个接收者不需要比较键。`local_target` profile 只处理目标，不读取 namespace；它对应已观察的局部 helper，不断言旧版全局登记与新版派生行为相同，也不是文件年份检测。

图上的 `create_reflected_properties` 按接收定义分组调用局部批量创建，每个接收者只构建一次元数据索引，再按“源属性→接收者顺序”返回事件。数据、覆盖及字段供应者共享，新属性身份独立。结果区分选择未决、复制未决和已完成源前缀；来源复制未决时，只把此前可确定的源属性传播给全部已选定义。

传播的新增属性数、缓存成本和生成文本使用跨接收者的累计预算；`max_receiver_history` 另限制为接收者构建快照时复制的旧行与新增行总数，避免少量新属性导致大量旧元数据复制。各 scope 的创建预算也继续生效。资源耗尽抛 `Error`，输入图保持不变，不模拟产品异常展开或回滚。

`definition(row)` 读取当前定义，`with_definition(row, state)` 只接受同一作用域身份的不可变更新。图副本和更新保留图身份及拓扑，`same_identity()` 可检查该关系。图行 ID 只在该图的身份范围内有意义；事件的 property 是返回 scope 快照内的历史行，跨分叉快照的新行不能仅凭相同行号合并。

调用方仍须提供真实的定义/namespace/父关系映射、普通缺失根属性的判断、源 SchemaVersion 跳过和上下文修复；该入口不执行完整源迭代、已有属性兼容、全局 UID/CommonSchema/assettype 门槛、别名传播或命名空间接管/释放。

`protein_schema_finish_merge(source, target, source_version, target_version, compatible, options)` 执行普通属性处理之后的局部收尾：先按原始 uint32 版本和兼容结果写回或创建目标 SchemaVersion，再检查目标独有名称，最后把源别名逐条登记到目标。返回两个不可变状态；使用返回的 `source` 和 `target` 继续处理。

```cpp
auto tail = nwd::protein_schema_finish_merge(
    source_state, target_state, source_version, target_version, compatible);
source_state = tail.source;
target_state = tail.target;
// 检查 tail.issue；未决时仅此前已确定的步骤已建立。
```

默认 `reader_2026` 在源中直接查目标属性名；显式 `reader_2017` 允许先查实际名称、再展开一层源别名。两者是已确认的算法 profile，调用方选择；不由文件年份推测。检查包括全部活动容器和刚生成的版本属性，在首个缺失名称停止。它将源定义的原始 `merge_marker_byte()` 设为1；目标标记保持原值。该字节的其他产品语义不作推断；仅有 XML 时为未知，可用 `with_merge_marker_byte(byte)` 提供已确认原值。

版本 setter 或别名登记拒绝不撤销此前的兼容结果；未确认的版本接口、属性查找、目标枚举或别名输入返回不同的 `issue`。`compatible=false` 跳过全部收尾。相同 scope 的同一快照按同一对象处理；同一身份的不同分叉快照被显式拒绝，避免静默拼接两份当前状态。资源限制抛 `Error`，输入状态保持不变。

`ProteinSchemaAliasIndex::enumerate()` 与定义的 `enumerate_aliases()` 按“本地接受顺序→父级”枚举，包含被本地名称遮蔽的父级条目；它不同于可见名称查询。`copy_aliases_from(source, options)` 对该序列做一次快照，批量登记到目标本地，保留名称冲突、目标冲突及原文档来源。索引和定义状态均支持该操作；定义状态的 `alias_attempts()` 可检查本地 XML 和运行时登记的全部尝试。`ProteinSchemaDefinitionIndex::aliases()` 继续表示本地 XML 声明。属性查找始终先查实际属性，再展开一个别名目标。

批量别名传播共享来源文档、名称和属性值，仅构建一次目标索引快照。`max_aliases` 包括被拒绝的本地尝试，`max_enumerated_aliases` 包括被遮蔽的父级条目，另有可见名称数和保留作用域深度限制。无源别名时不建立新快照。这个收尾入口仍需要调用方完成普通源属性迭代、修复及兼容检查，不代替完整全局注册、namespace 接管或文件记录到 scope 的身份映射。

`ProteinSchemaDefinitionGraph::reconcile_definition(source_row, target_row, namespace_rows, options)` 将局部定义合并接成有序的源属性循环。调用方提供同一图内的源/目标 scope、父关系和 namespace 枚举；UID 相等只允许进入兼容检查，不合并两个 scope 的身份。

```cpp
nwd::ProteinSchemaReconciliationOptions options;
options.profile = nwd::ProteinSchemaIterationProfile::reader_2026;
auto merged = graph.reconcile_definition(source_row, target_row,
                                         namespace_rows, options);
graph = merged.graph;
// 检查 merged.status、merged.issue、events 与 tail。
```

入口先核对 UID 并按目标→源读取初始版本，然后按全部活动容器的原生 profile 顺序逐条处理源属性：跳过实际名 SchemaVersion、按显式上下文修复源 flags、查询当前目标、执行父级覆盖/Base/URI/普通版本分派，或创建缺失属性。普通版本分支每次重读当前源/目标版本；初始版本只用于末尾写回。任一步已确认不兼容就停止下一条源属性，保留此前完成的修改，跳过整个收尾。未决另行报告，不能作为兼容接受。

`reader_2017` 的目标和源父级查询允许一层别名，缺失创建只处理当前目标；`reader_2026` 直接查询属性名，缺失创建使用已提供 namespace 中的派生接收者。选择结果按固定拓扑复用，源属性始终按顺序执行：一次创建对下一次查找立即可见，不将被已有属性操作隔开的缺失项任意分组。全部源属性兼容后执行版本写回、源端目标独有名称标记及源→目标别名收尾。

内部为受影响 scope 使用私有可变索引/稀疏覆盖，结束时冻结；每个 scope 最多复制一次已有元数据和更新表。生成行共享同一最终索引与源数据，已有属性/scope/图身份保留。`comparison` 是整次调用的累计读取预算，覆盖 UID、初始/当前版本、父级和属性比较及最终整数写入；`max_metadata_history`、`max_state_history` 分别限制所有 scope 复制和新增的元数据/覆盖行。传播预算也累计使用，最终新建版本计入新增属性/值总量；事件、源/目标枚举及别名另有限制。

结果的 `events` 保留已访问源行及已有分派诊断，`created` 保留按源顺序的接收事件，`next_source` 表示原始源枚举中已经完成的前缀。`graph` 保留已确定的本地状态，包括未决前完成的源 flags 修复、控制字节或 URI 修改。运行时别名继承使用下述图级查询；抽出的定义索引保留自己的快照视图。预算超限抛 `Error`，输入图保持不变；这不模拟产品异常回滚。缺失创建若会反射回正在枚举的源 scope，当前返回 `source_receiver_overlap` 未决，避免猜测重入迭代及对象释放行为；同一行的源=目标普通合并可直接处理。

这个入口处理显式 runtime scope 上的局部合并。CommonSchema/全局注册门槛、namespace 接管/释放及持久文件记录到这些 scope 的映射仍需要上层解析流程提供，不能仅凭局部合并兼容就断言整个 NWD/NWC/NWF 已完整解析。

反射传播按已确定的接收者顺序执行。如果接收者中包含正在迭代的源作用域，合并器在轮到该作用域时返回 `source_receiver_overlap`，并以 `blocked_receiver` 标明阻断作用域。`created` 和返回图保留此前已经完成的接收者写入；未完成的当前源事件仍保存在 `events`，`next_source` 只计算已全部完成的源属性。阻断接收者及之后的作用域不生成属性，版本与别名收尾不执行。接收者之前发生的源 flags 修复或已有属性更新也保留。

这一诊断表示源对象替换和后续迭代的所有权契约尚未确定，不能据此把旧源快照继续处理成完整合并，也不能当作可恢复产品事务。资源预算只消耗实际执行的前缀；超限仍抛 `Error`，输入图保持不变。

图中的 `parent` 与定义索引在加载时保存的别名父级快照是两种视图。仅在已确认定义作用域父级映射时启用动态绑定，不能直接使用几何结构树的父节点。默认图查询仍使用索引快照。调用 `with_alias_parents()` 可明确将图关系绑定为运行时别名父级；此后使用图的 `find_alias(scope, name)`、`find(scope, name)` 和 `enumerate_aliases(scope)` 读取当前继承结果。

```cpp
graph = graph.with_alias_parents();
auto copied = graph.copy_aliases(source_scope, parent_scope);
graph = copied.graph;
auto property = graph.find(child_scope, "new_alias");
// 检查 property.property.status 和 property.alias.issue。
```

父作用域的别名表变化后，新图中的子节点查询立即读取当前父表；旧图继续返回旧结果。绑定和查询不复制子节点属性值，不按 UID/名称合并 scope，也不重放本地接受/拒绝结果。`definition(scope)`、抽出的 `ProteinSchemaDefinitionState` 和索引仍是本地/索引快照，不会自动携带整个图的动态继承关系。可用 `with_alias_parents(false)` 回到各索引保存的视图；本地运行时登记历史保留。

动态 `find` 先查本作用域实际属性，缺失时才查询本地别名及当前父链，再用所得目标名对本作用域做一次直接属性查找。不展开别名链，也不从父级借用实际属性值。枚举保留本地接受顺序及被遮蔽的父级条目。`scope` 字段表示提供该本地接受条目的运行时作用域；binding 的 document/source 则表示原始文档来源，两者可能不同。返回视图借用所属图的生命周期。

`copy_aliases(source, target, options)` 先确认源初始总数与父链，再按这个总数逐条读取当前序号。目标是源的祖先时，本地表追加会改变后续源枚举；不会盲目重放旧快照。结果的 source 是初始清单，inputs 是实际完成的序号及其供应作用域，blocked_input 是尚未登记的未决输入。名称冲突检查使用当前图父级，目标反查只检查目标本地。后续父级未知时，已完成的接受/拒绝前缀保留，未确认条目不生成历史行；源计数尚未完成时不复制其已知条目。尝试和冲突所引用的源/祖先文档被持有，抽出目标状态后仍能检查历史来源。

未知父级、缺失节点、未完整作用域及被访问的父环有不同诊断；实际属性或本地别名命中可绕过无关的未知/循环父级。未决枚举只表示已知条目，不能当作原生计数已成功。`ProteinSchemaGraphAliasOptions` 限制逻辑父边读取、枚举条目和单次组合操作的查询缓存；资源超限抛 `Error`。登记同时遵守别名历史、索引可见表存储、来源持有深度及目标元数据历史预算。

绑定模式下 `reconcile_definition` 的初始/当前/最终版本、旧 profile 的别名查找和末尾别名登记均使用这套动态视图。`graph_aliases` 预算贯穿整次合并；缓存仅保存别名名称绑定，属性值每次重新读取，因此已有字段更新仍立即可见。缓存没有全局可变状态，不跨图快照保存。

`protein_schema_compare_values(left, right, options)` 对缓存工厂值执行原生兼容比较；`protein_schema_compare_choice_tables(left, right, options)` 单独比较 XML 枚举表。`protein_schema_compare_properties(left, right, skip_type, options)` 依次检查属性存在和类型、跳过类型、数值约束或 Choice 表及具体值。它们返回 `equal`、`different` 或 `unresolved`，并提供未决原因与条目位置；不会复制数据或执行全局登记。属性 flags、label、ID、别名、版本升级和 URI 更新由登记流程另行处理。

比较保留原生规则：普通浮点无序比较可返回 equal，颜色与向量采用各自的分量规则；Reference/Texture 的该兼容判断只检查具体类。Choice 以左表为准，右表额外条目可被忽略，右表短前缀返回 unresolved；数值约束的单侧空对象也保留方向差异。已确定的早期不匹配先返回，未知数字和单位提供器结果会保留未决状态。它们不定义模型对象身份或资源目标相同。

比较只访问不可变缓存，不重新解释 XML，不分配比较工作区，可由多个线程并发调用。`ProteinSchemaComparisonOptions` 限制访问的值/条目数与文本字节数，超过限额抛出 `Error`。`ProteinSchemaResolvedConstraint.fields` 缓存最后一个约束候选的原始转换结果，便于检查未决数值和保留未启用端点的字段。

`protein_schema_runtime_binding(*property.value)` 返回缓存值工厂的具体注册类型名和数据类型名。例如 IntegerValue/int、StringList/run.string_array、GeneralUnitsValue/units_value；未知布局返回空。此查询不分配内存，不依赖产品 DLL，并以缓存值的实际形态为依据，继承后属性 flags 的变化不会重塑该值。它不执行全局登记，也不替代候选值的接受/转换状态检查。

Float/Distance 单值的非空 `unit` 保留在 `ProteinSchemaNumericDefaults.unit` 中；`unit_code` 返回已知长度名称的原始单位代码，不换算数值。37 个已知名称按大小写精确匹配。其它名称保留原文，代码为空，仍需单位提供器解析。多值 Float/Distance、Integer、Color、Vector3 工厂不将声明单位写入值。无单位的 Distance 仍使用 ScalarValue；不能仅凭距离标志判断具体值类型。

`definition.iteration(options)` 返回实际属性的历史索引序列，保留跨容器同名项，排除已替换历史和别名。`ProteinSchemaIterationOptions` 可选择正向/反向、仅 public 属性及 `reader_2017` / `reader_2026` 算法。默认使用后者；算法选择不等于自动判断文件保存年份。

三容器按名称合并，同名时保留当前游标所在容器，public 筛选在推进原始合并序列后执行。反向遍历有独立的同名顺序，不能直接反转正向结果。旧算法按有符号字节比较 UTF-8 名称，新算法按无符号字节比较，因此非 ASCII 名称可能有不同次序。迭代复用已缓存属性和值，返回值只包含索引；原有 `container()` 仍提供各容器的固定字典序视图。

```cpp
nwd::ProteinSchemaIterationOptions order;
order.profile = nwd::ProteinSchemaIterationProfile::reader_2026;
order.public_only = true;
for (auto id : definition.iteration(order)) {
    const auto& property = definition.properties()[id];
    // property.id / flags / value / document / source 保留各自身份。
}
```

如果定义状态不是 `ready`，序列仅覆盖当前已接受的属性前缀，调用方须检查 `definition.status()`。此接口不执行全局注册或更改已有定义。

`ProteinSchemaDefinitionLoadIndex(local_definitions, options)` 在一个明确的批次作用域内组合已缓存的局部定义，执行 XML 读取后的补值及命名父依赖解析。输入顺序决定登记时点，UID 精确匹配；已就绪或等待中的同名定义均保留首次登记身份。`records()` 返回每个输入的 UID、实际读取的 Base、规范对象、父级、等待关系、循环和接受状态；根定义、重复或未处理输入的 `base` 可以为空。`input(i)` 始终保留原始局部索引，`definition(i)` 返回加载结果，重复项跟随规范对象最终合并后的快照；尚未处理的输入返回空指针。

加载期只在缺失时补只读 Integer `SchemaVersion=1`，随后才进行命名父合并，因此缺少版本声明的子定义会保留自己的 1。CommonSchema 还会按需创建 AssetLibID、VersionGUID、localtype、localname，并按已知旧版本规则改写 category/keyword。`ProteinSchemaDefinitionProperty::origin` 区分 XML、加载期默认、加载期改写和局部登记创建；生成属性的 `source` 为 `none`，改写保留原 XML 上下文及历史值。继承复用不可变值，旧版文本清空保留标量/数组形态。

`events()` 保留补值、改写、登记、等待、重复、拒绝和未决事件的顺序。等待队列按 UID 顺序选择当前可就绪项，使用反向依赖避免重复扫描无关项。`pending()` 和 `cycles()` 分别返回剩余等待项和循环成员，记录的 `waiting_on` 还能区分指向已知等待项的边与当前缺失的父。

`status()` 描述批内依赖是否就绪、仍有等待项或存在未决语义。已知空 Base 拒绝允许处理后续输入，因此依赖就绪不代表所有输入都接受，调用方仍须检查每条记录。局部声明接受未决、CommonSchema 版本读取未决或不兼容值接口会停止后续登记推断，保留输入和已处理前缀；与加载门槛无关的未知候选数值仍按其投影状态保留。此时未命中的 UID 查询为 `unresolved`。`common_requirements()` 对已就绪的规范 CommonSchema 返回 17 项实际名称与 `flags & 0xe` 检查，别名不代替必需属性；这不等于完整全局注册或兼容性已经通过。

`protein_schema_check_common_registration(state)` 将同一组 17 项门槛应用于 `ProteinSchemaDefinitionState` 的**当前**属性及 flags。仅 UID 精确为 `CommonSchema` 时 `applicable` 为 true；缺失属性、flags 不匹配和定义未决分别保留在 `requirements` 中。`accepted` 只表示这 17 项检查全部通过，不表示递归父注册、目标 UID 查找、兼容合并或新对象创建成功。与 `common_requirements()` 的加载快照不同，后续 `with_property()` 写入的 flags 会影响此检查。

```cpp
auto gate = nwd::protein_schema_check_common_registration(current_state);
if (gate.applicable && gate.accepted) {
    // 17 项当前状态检查通过；仍需执行其余全局注册步骤。
}
```

`child.with_inherited_parent(parent)` 在此之后可用于局部状态继承：它复用索引的活动容器与父别名规则，同时将父级当前属性状态带到新行。父级独有属性保留当前文本/POD 覆盖、约束、Choice 和共享字段来源；子级同名属性保留子级当前值，并按 `parent.flags | (child.flags & 0x180)` 更新当前 flags。调用会保留子状态身份并服从 `ProteinSchemaDefinitionOptions` 的历史/深度限制。它只完成此作用域的继承，不执行 UID namespace 注册、递归父登记、对象创建或释放。

```cpp
auto child_with_parent = child_state.with_inherited_parent(parent_state);
auto inherited = child_with_parent.find_actual("ParentProperty");
if (inherited.status == nwd::ProteinSchemaDefinitionLookupStatus::found) {
    auto current = child_with_parent.property(inherited.property);
    // 使用 current.text_value(), pod_bits(), constraint() 或 choice_table()。
}
```

`ProteinSchemaGlobalRegistry` builds a bounded, immutable lookup index for one explicit manager and its Namespace. Namespace entries and pending creators remain separate tables; lookup checks Namespace first, and a matching null-Core row prevents pending fallback. An incomplete Namespace or pending table, duplicate key, unknown Core, or unresolved creator returns `unresolved` instead of guessing. The registry copies UID bytes (including embedded NULs), supports the two reader byte-comparison profiles, and returns the caller's manager, Namespace, object, creator and definition-scope IDs separately. `adopt_pending()` returns a new registry only when the pending creator, object and definition IDs match and the UID is still absent from the complete Namespace; the new snapshot contains the Namespace object and no longer contains that creator. `dispose_pending()` removes only the explicitly named creator, even if a same-UID Namespace object shadows lookup. Both operations report ownership changes separately; the input snapshot remains unchanged.

```cpp
nwd::ProteinSchemaGlobalRegistryInput input;
input.manager = manager_id;
input.name_space = namespace_id;
input.profile = nwd::ProteinSchemaIterationProfile::reader_2026;
input.namespace_entries = namespace_entries;
input.pending_creators = pending_creators;
nwd::ProteinSchemaGlobalRegistry registry(input);
auto found = registry.lookup(uid);
if (found.status == nwd::ProteinSchemaGlobalLookupStatus::namespace_object) {
    // found.object and found.definition retain caller-supplied identities.
}
auto adopted = registry.adopt_pending(uid, creator_id, object_id, definition_id);
if (adopted.status == nwd::ProteinSchemaGlobalRegistryUpdateStatus::adopted &&
    adopted.registry) {
    registry = *adopted.registry;
}
```

These per-manager transitions provide the ownership bookkeeping for the global registrar. Parent recursion, assettype factory calls, actual wrapper/Core allocation and destruction, compatibility merge execution and wire-record-to-manager mapping remain separate parsing stages.

`ProteinSchemaDefinitionLoadOptions` 限制输入数、事件数、累计声明、保留的属性历史和缓存值，并提供单定义与继承深度上限。输入必须是尚未执行显式父属性合并或加载期改写的局部索引，已有 XML 值不会重新解析。索引及副本维持所有文档/值的寿命，结果可并发只读查询。

每个加载索引是独立作用域；不同文件、库或模型图中的同名 UID 不会自动合并。Base 文件与归档加载、自动 NWD 库/图作用域构造、完整全局注册兼容性仍需单独接入。

```cpp
std::vector<nwd::ProteinSchemaDefinitionIndex> local;
for (const auto& document : documents) local.emplace_back(document);
nwd::ProteinSchemaDefinitionLoadIndex batch(local);
for (nwd::Id i = 0; i < batch.records().size(); ++i) {
    const auto& record = batch.records()[i]; // 检查接受/等待/拒绝/未决状态。
    if (const auto* definition = batch.definition(i)) {
        auto property = definition->find("SchemaVersion");
        // 返回缓存投影；重复项指向首次登记对象的最终快照。
    }
}
```

`numeric_defaults(entry, options)` 返回 Integer、Float、Distance、Color、Vector3 的 XML 数值工厂转换结果。`status` 区分 ready、rejected、unresolved、unsupported；只有 ready 时，`candidates` 才是完整且通过局部数值检查的结果。候选项保留原始数值文本和转换状态，按值排列，`components` 为每个值的分量数（标量 1、XYZ 3、RGBA 4）。

数值数组只用首值检查范围，整数还检查上下限是否为整数；后续数组值保留原值。Color 首项检查 RGB `[0,255]`、alpha `[0,1]`，数组后续项不重复检查。Vector3 只保留各分量首值；Color/Vector3 不使用数值约束节点。空数组、标量缺省值和未决数字分别保留，未知输入不自动变成 0。`rejections` 给出范围、整数边界或首颜色拒绝原因；`constraint` 保留所检查约束的来源和状态，只有步长未决不影响值的范围检查。该接口不换算单位，也不代替声明 ID/标志、继承或注册验证。

`resolved_constraint(id)` 返回声明中最后一个数值约束候选的处理结果和来源节点。`absent` 表示没有候选，`ready` 提供可用区间，`discarded` 表示无效区间清除了前一个约束，`unresolved` 表示数值转换尚不能确定。最小值大于最大值，或相等且任一端不包含端点时，区间被清除；百分比约束强制使用闭区间 `[0,1]`、步长 `1`。该接口按声明顺序遍历，使用常量额外遍历空间，仅解释最终候选；它不表示属性工厂已接受默认值，也不执行继承合并。

```cpp
auto schema = nwd::protein_schema_fields(graph, schema_id, graph_id);
nwd::ProteinSchemaDocument document(schema.data);
for (nwd::Id id : document.entries()) {
    auto raw = document.entry_fields(id);
    auto defaults = document.defaults(id);
    auto type = nwd::protein_schema_value_type(raw.kind);
    if (type) {
        auto conflicts = nwd::protein_schema_flag_conflicts(*type, raw.declaration_flags);
        auto prepared = nwd::protein_schema_prepare_fields(raw, defaults.effective_id);
        // conflicts 检查原始标志；prepared 提供预处理字段，不表示工厂已接受声明。
    }
    // raw 保留保存文本；defaults 的数值须连同 status 检查。
    auto constraint = document.resolved_constraint(id);
    // 仅 ready 状态提供 range；source 可回查原始 XML 节点。
    for (nwd::Id child : document.constraint_nodes(id)) {
        auto constraint = document.constraint_fields(child);
        // document.nodes()[child] 保留约束来源与全部原始属性。
    }
}
```

XML 读取限制输入长度、节点数、属性数、深度及解码文本总量；默认值解释另有限制值数量的 `ProteinSchemaValueOptions`。支持 UTF-8/UTF-16 和普通 XML 实体，DTD 明确不支持。Schema 的 Base/父引用、库定义查找、继承覆盖、UUID/URL/Choice 的完整业务转换和资源角色仍有未实现部分；不会自动把所有 URL 解释成纹理，也不会生成运行时材质。


`SavedItem.material_asset` 指向所属 `SavedItems.objects`，保存版本 443 前为旧资产 182，443 起为 JSON 资产 185。重复、NULL 和跨块的对象身份保持独立；保存材质的存在不等于它已应用到某个实例。

`ExternalReferenceIndex::resolve_protein_uri` 支持明确协议分类或 `FileProtocolContext` 注册快照，图索引重载适合批量 URI 读取。嵌入文件绕过 XRef；本地路径仅在满足来源格式的绝对路径规则时查询，远程协议查询。注册区分大小写，同名首项生效；未知协议和缺少非 ASCII 字符分类环境的状态明确返回。

`ProjectOptions.protein_protocols` 默认包含 file 注册和默认处理器，调用方可提供其他已知注册；`protein_url_generation` 选择分类行为，独立于文件保存版本。Project 读取旧资产 URI 并复用引用表、文件和资源缓冲。`TextureFile.protein_uri` 保留原始对象/属性/ordinal，`enabled_path` 是结构可达性，`active` 还要求根资产实际被已绑定外观使用。旧 URI 暂不推断 diffuse、bump 或 thumbnail 角色；缩略图列表可通过具名字段另行读取。

`ProteinProperty.embedded_files` 索引所属 `ObjectGraph.embedded_files`，可取得原始字节、前后缀及所属对象。嵌入 ordinal 只在所属属性内有效，不能按同名或跨属性 ordinal 合并。Project 返回的共享字节缓冲可独立维持来源数据生命周期；独立图查询返回的视图则借用原图。

### 单位、材质与纹理

矩阵使用列主序。`world_matrix()` 保持源单位，`model.meters_per_unit` 提供到米的比例。`project_world_matrix()` 将受支持的引用放置和单位换算组合为米制矩阵；遇到不支持的引用放置会抛出异常。

同单位 NWF 引用支持替换源模型的基准变换，包含旋转、平移、缩放和剪切；源几何仍共享。`model_base_matrix()` 返回源基准矩阵，`reference_placement_matrix()` 返回引用所需的米制修正矩阵。方向提示的变化单独保存在节点的 `orientation_changed` 中。

`NwfReference.load_flags` 的 bit0、bit1、bit2 分别表示采用保存的单位、基准变换和朝向向量；版本244之前的缺省值为7，之后保留文件中的显式值。引用放置仅在 bit1 启用时替换基准变换；未启用单位和变换覆盖时保留源米制坐标。单位覆盖、跨单位基准替换及嵌套引用的复杂放置仍会明确报告不支持。朝向向量保留原始记录，尚未提供加载后的有效向量查询。

`NwfReference` 和 `NwfData` 的 `up`、`north`、`front` 分别返回保存的上向、北向和前向向量，不自动归一化或推算缺失方向。来源记录在格式版本109之前没有前向字段，`front` 返回零向量。

直接、同单位引用且源模型没有已保存的对象覆盖时，`project_world_matrix()` 还会应用 NWF 对象变换。`nwf_transform_matrix()` 提供原始载荷到列主序矩阵的转换，`Project.transform_overrides` 保留每个实例的米制覆盖矩阵。

`Instance.auxiliary_transform` 引用 `Model.auxiliary_transforms` 中的辅助仿射、平移或平移＋旋转记录。这些数据保持源值，与当前实例矩阵分开返回。

材质通过 `Material` 的 `ambient()`、`diffuse()`、`specular()`、`emissive()`、`shininess()` 和 `transparency()` 访问。资产图保留原始参数类型、连接和 URI；JSON 资产的完整数据在 `Asset.json`，旧资产原图在 `Asset.protein`。

纹理保留编码后的原始字节，不做图片解码。通过 `TextureFile` 的源、模型、资产及别名关联材质；`active` 标明根资产的使用状态，`thumbnail` 用于已识别的 JSON 缩略图角色。外部文件可使用 `ProjectOptions.search_paths` 查找，或通过 `remaps` 指定原路径到本机文件的映射。

`NwfData.texture_spaces` 返回盒形、平面、圆柱、球形和显式 UV 映射。向量、旋转和方向枚举保持序列化顺序，`parameters_present` 指明文件是否保存了参数。`Project.texture_space_assignments` 将记录关联到模型路径；通过 `owner` 找到所属 NWF，再以 `record` 访问记录。`node_scope` 表示共享节点赋值，可用 `path_reference()` 获取 graph/object 身份。这是赋值记录接口，多层映射覆盖的最终优先级由调用方处理。

## 支持范围与错误处理

产品数据通过 `Document::read_products()` 单独读取，也可设置 `Options.products = true`，从 `Scene.products` 或 `ProjectSource.products` 获取。它默认关闭，便于只读取模型数据的调用方控制耗时与内存。

```cpp
nwd::Document file("model.nwd");
auto data = file.read_products();
for (std::size_t i = 0; i < data.blocks.size(); ++i) {
    const auto& block = data.blocks[i];
    if (const auto* saved = std::get_if<nwd::SavedItems>(&block.value)) {
        for (const auto& item : saved->items) {
            // parent 是 saved->items 内的索引；nwd::none 表示根项。
            if (item.keyframe)
                std::cout << item.name << ": " << item.keyframe->time << '\n';
            if (const auto* task = std::get_if<nwd::TimeLinerTask>(&item.timeliner))
                std::cout << task->display_id << ": " << task->progress_percent << '\n';
        }
    }
    if (block.status == nwd::ProductStatus::partial ||
        block.status == nwd::ProductStatus::failed)
        std::cerr << data.chunks[i].name << ": " << block.diagnostic << '\n';
}
```

`ProductStatus::decoded` 表示受支持的块布局已读取并通过结尾检查，不保证所有源枚举含义和对象关联都已解释。`not_handled` 表示产品模块未处理该块，其中也包括由核心读取器负责的几何、属性等块。`Scene.parsed_chunks` 合并本次核心与产品读取结果；NWF 使用 `NwfData.parsed_chunks` 与产品状态共同检查。设置 `ProjectOptions.reader.products = true` 后，未处理或失败的块会使 `Project.complete` 为false。

`SavedItems.objects` 保留共享名称和视点覆盖对象；选择记录的 `path_links` 是源PathLink标识，必须结合下述模型绑定或项目绑定接口使用，不能当成ObjectGraph或NWF路径索引。`SavedItem.complete` 表示该保存项布局及子项已读完，部分块中仍可能有成功读取的前序项。旋转、时间、枚举和未命名字段保留文件值，不自动执行搜索、动画或施工仿真。

碰撞数据通过 `SavedItem.clash` 访问：`ClashTest` 返回选择、容差、规则与运行配置；`ClashResult` 返回距离、两侧位置、包围盒、状态、审批信息和模拟事件；`ClashResultGroup` 保留结果分组。组的子项使用 `SavedItem.parent` 关联。`SavedItems.source_references` 提供保存的模型来源，缓存属性与规则参数共用 `SavedItems.objects`。`legacy_clash` 标识旧容器，旧状态枚举保持源值；结果中的 `test_name` 是测试类型或自定义标签，与保存项的 `name` 分开使用。PathLink 和备用 PathLink 保留原始编号，调用方不能直接把它们当作模型路径索引。

当前动画返回一个隐含类型为 1 的保存项根及其子项。`TimeLinerClock` 提供模拟时钟，`TimeLinerSimulation` 按序列化顺序返回时间、整数、布尔、字符串和动画路径。时间与枚举保留源值，模拟设置中的部分字段语义仍需解释。

`DatabaseLinks` 的名称索引属于自身的 `objects`，与 SQL、连接字符串和字段映射一起返回；`tagged_connection` 使用 UTF-8，保留宏表达式，不执行外部数据库查询。`Grids` 保留系统、轴线和楼层层级，段类型 `5` 是直线、`2` 是圆弧。`GuidStore` 保留源顺序和重复 GUID；`SceneStatistics.text` 保留统计报告原文。

`SavedItem.sheet_info` 返回图纸ID、初始文件/图纸、属性类别和来源GUID；文件根的 `file_info.default_sheet_matches` 按原始图纸ID列出所有直接子项匹配，不擅自消除重复。`light` 的 `object` 与 `material_asset` 索引属于同一 `SavedItems.objects`，现代灯光另提供 `position` 和 `target`。

`FileDatabase.tables` 保留数据库普通表的原始列与行；`DatabaseCell` 区分NULL、64位整数、浮点数、字符串和二进制字节。空BLOB与NULL、自动索引的NULL SQL与空字符串分别保留。`schema` 保留视图/索引/触发器定义，但读取器不会执行这些定义；应用表中BLOB内部格式和业务关联需另行解释。

`SpatialHierarchy.nodes` 使用 `parent/first_child/next_sibling` 表达邻接关系。类型1/4的 `fragment` 是模型分片槽，只有 `associations_verified` 为true时才能结合 `model` 访问 `Scene.models[model].instances[fragment]`。通过 `Options.products = true` 调用 `read_scene()` 才会校验此关联；单独 `read_products()` 保留未绑定状态。实例仍引用共享几何。

`HyperlinkOverrides.paths/nodes` 和 `NodeOverrides.paths/nodes` 分别保存路径级、节点级覆盖；节点记录的 `paths` 保留显式选择器或隐式候选列表。`TextureSpaceOverrides` 复用纹理空间记录。三种类型通过 `read_scene()` 读取时，共用与 Presenter 相同的模型命名空间和路径校验：只有 `associations_verified` 为true时，才能用 `model` 访问所属模型、将选择器视为该模型的路径索引。节点范围通过 `path_reference()` 取得共享节点身份；单独 `read_products()` 不绑定模型。

`NodeOverrides.mask_value_encoding` 为true时，`flags` 的低字节是覆盖掩码，高字节是覆盖值。以 `mask = flags & 0xff`、`values = (flags >> 8) & 0xff` 计算，基础状态的覆盖结果为 `(base & ~mask) | (values & mask)`。旧版位字保持原值。

`SavedItems.model` 和 `SavedItems.associations_verified` 为保存项中的显式 PathLink 提供分区上下文。通过 `read_scene()` 并开启 `Options.products` 后，仅当标志为 true 时，才能将选择集、动画选择轨道、TimeLiner 选择数组、碰撞测试/结果主路径及备用路径、视点节点/材质覆盖中的非空路径编号用于 `Scene.models[model].paths`。零是有效根，`nwd::none` 是空引用，重复路径保留。用 `path_reference()` 再取得共享对象身份。此标志不表示名称 locator、搜索条件、GUID 或 item-path 字符串已经求值；直接 `read_products()` 保持未绑定。

`SavedSelection.conditions` 保留搜索条件的类别/属性名称对象、比较操作、选项和带类型的值。`SearchCondition.condition` 是比较操作，`options` 是标志；磁盘中的顺序为先选项、后操作。`operation()` 返回已识别的 `SearchOperator`（存在、等值、次序、包含、通配符、日期范围等），未知编号返回空值；`has_option(SearchOption::...)` 检查名称模式、字符串处理、取反与分组标志，`unknown_options()` 返回尚未解释的位。原始编号和源值始终保留。这些接口描述保存的条件，不执行动态搜索。

`search_value_match()` 可以执行已有候选属性的值比较阶段，支持类型匹配、相等/不等、数值次序、字符串包含/通配符及时间窗口。`SearchValueView` 提供源 Value、字符串图和对象图 span；名称类型的值通过图上下文比较名称内容。返回 `SearchValueStatus`，缺少显示处理或数值比较规则时有独立状态。它不执行类别/属性名称匹配、取反、分组、选择范围或节点遍历。

```cpp
// property / model / graph 是候选属性及其来源；condition / saved 是保存条件。
if (auto op = condition.operation(); op && condition.unknown_options() == 0) {
    nwd::SearchValueOptions options;
    options.text.ignore_case = condition.has_option(nwd::SearchOption::ignore_value_case);
    options.text.ignore_accents = condition.has_option(nwd::SearchOption::ignore_value_accents);
    options.text.ignore_widths = condition.has_option(nwd::SearchOption::ignore_value_character_widths);
    auto status = nwd::search_value_match(
        *op,
        {property.value, graph, model.graphs},
        {condition.value, saved.objects, std::span(&saved.objects, 1)}, options);
    // 按 status 处理 match、no_match 或缺少比较上下文等状态。
}
```

比较保留源类型：浮点 tag1/6/7/10/11 属于同一存储类型族，整数、时间和其他类型分别处理；`search_storage_type_equal()` 可单独查询此规则。不同存储类型在“不等”操作中也返回不匹配。相等比较不把整数转为浮点；NaN 不等于自身，名称值比较内部名与显示名，空字符串与空引用分开。字符串操作以首个 NUL 为结束，通配符 `?` 匹配一个 UTF-16 单元，`*` 匹配任意长度；包含操作不匹配空参数。

`SearchValueOptions.exact_numeric_order=true` 显式选择精确数值排序。需要应用容差时提供 `compare_numeric(candidate, target)`，返回负数、零或正数；无法判断时返回 `nullopt`。回调优先于精确策略。默认不猜测新版应用的单位容差，数值次序操作返回 `numeric_context_required`。时间比较使用有符号整数，`within_day` / `within_week` 判断 `(target − 86400/604800, target]` 秒窗口，不按日历日或自然周分组。

`SearchValueOptions.text` 设置大小写、重音和字符宽度处理。启用任一标志时，`transform` 必须提供调用方应用的完整 UTF-16 规范化规则；不能确定则返回 `nullopt`。字符宽度处理后的通配符是全角 `＊` / `？`。`max_text_units` 和 `max_match_steps` 限制字符串及匹配工作量，超限抛出异常。未知值类型、无图上下文的名称值、非法 UTF-8 或非标准布尔编码明确返回未支持状态；源值不改写。类别/属性存在等操作需要节点上下文，不属于这个值比较接口。

NWF 中的保存项路径通过 `Project.saved_path_bindings` 关联到引用出现位置。开启 `ProjectOptions.reader.products` 后，用 `saved_path_binding(project, owner, block, source_path_id)` 查询：`resolved` 返回目标 `node/path`；`project_root=true` 表示整个 NWF 出现的子树；`empty` 是空引用，`unresolved` 表示无法唯一匹配。这里的源 0 和 `none` 都为空，源 1 是项目根，不能按 NWD/NWC 的零起始模型路径解释。重复源 ID 共享绑定记录，原字段顺序和模型几何仍保留。缺失引用或歧义会使 `Project.complete=false`。

`parse_selection_locator(selection.locator)` 读取名称路径及转义，`SelectionSetIndex(products)` 建立可复用的选择集索引。调用 `index.resolve(owner_chunk_name, locator)`，每个目标返回原 `ProductData.blocks` / `SavedItems.items` 身份或明确的缺失、歧义、不可用、未支持根状态。精确 `/` 返回 `select_all` 标志。索引按源块前缀区分选择树，支持 `lcop_selection_set_tree`；它定位保存项，不执行该项的动态搜索或把组展开为几何。

`PropertyLocatorIndex(model)` 为单个模型中节点直接挂接的序列化属性（类型 84/86）建立可复用索引，支持 `lcop_property_tree` 的类别、属性和值三级名称路径。`resolve(path)` 返回源 `PropertyReference`；`attribute` 是 graph/object 引用，`property` 是该对象的属性下标，类别记录使用 `none`。`owners(attribute)` 返回挂接该属性的模型路径，保留共享节点的各次出现。查询结果中的 span 在索引仍存活时有效；解引用源记录仍需保留原模型。索引拥有名称和身份数据，源模型变化后应重建。

```cpp
auto scene = nwd::Document("model.nwd").read_scene();
const auto& model = scene.models.at(0);
nwd::PropertyLocatorIndex index(model);
auto locator = nwd::parse_selection_locator("lcop_property_tree/类别/属性/值");
for (const auto& path : locator.paths) {
    auto found = index.resolve(path);
    if (found.status != nwd::PropertyLocatorStatus::resolved)
        continue; // 按状态处理缺失、歧义或未支持的显示规则。
    for (const auto& ref : found.records) {
        const auto& attribute = nwd::resolve_object(model, ref.attribute);
        if (ref.property != nwd::none) {
            const auto& value = attribute.properties.at(ref.property).value;
            std::cout << "value type: " << value.tag << '\n';
        }
        for (auto owner : index.owners(ref.attribute))
            std::cout << "model path: " << owner << '\n';
    }
}
```

索引默认忽略内部属性。`PropertyIndexOptions.include_internal` 可将其纳入；`max_entries` 限制记录及关联数量，`max_label_bytes` 限制索引持有的名称、显示文字和类型值键的字节数，超限抛出异常。格式化回调产生的临时分配不计入此字节限额。索引只在构建时扫描模型，同一共享源属性只格式化一次，查询不复制几何或展开子树。

默认值标签仅采用旧式字符串规则（CR/LF 换为空格），不能自动判断任意来源文件的原生显示方式。单位、小数位、本地化和较新属性树的计数后缀需要调用方通过 `format_value(model, ref)` 提供完整标签；无法确定时返回 `nullopt`，查询返回 `display_context_required`。不同内部名称具有相同显示名称、或不同源值显示为同一文字时返回 `ambiguous`。非有限数值的比较语义未支持，提供格式化文字后仍返回 `unsupported_value`。类别和属性级定位不依赖值的显示格式。

这里的 `resolved` / `missing` 仅描述索引中的直接序列化属性。应用程序及插件动态生成的属性类别、完整动态搜索、多个模型的联合属性树与显示计数不在此索引中自动合成；NWF 调用方应通过源文件和模型上下文分别查询并保留引用出现身份。`SelectionSetIndex` 仍只负责选择集名称定位。

`ExternalReferenceTable` 返回完整重映射表和原始JSON；联合引用加载仍使用 `load_project()`。

`PresenterData` 包含背景、材质槽、材质分配和纹理映射；`PresenterLights` 返回旧式灯光列表。实体的 `archive` 索引指向各自的 `archives`，同一个归档内用 `LightWorksObject.parent/first_child/next_sibling` 访问层级。`identity` 保留源编号，`reference` 指向同归档的已有对象，`null_reference` 区分显式空引用。不同归档不能按名称或数字ID合并。

`LightWorksField` 同时提供字段编号、源类型、名称和有类型的值。着色器返回类型名称和命名参数；图像返回编码字节、宽高、位深、行步长和codec字段；颜色、纹理坐标及灯光参数保留原值。未知字段名称为空，不能据此推定业务语义。旧式非LightWorks着色器通过 `LegacyShader` 返回参数。字符串数组使用 `vector<string>`，保留空字符串和内嵌 NUL；`LightWorksArchive.frame_count` 返回读取的分页数，跨页字段统一解码。`root_page/page_order` 返回起始页及逻辑读取顺序；`header_value` 保留尚未解释的头部值，`encoded_key` 保留旧式编码密钥。

对已解析、非引用的 `LtImage` 对象，可按需调用 `decode_lightworks_image(object, max_bytes)`，取得宽高、位深和源像素字节。支持原始数据和 zlib 两种内置 codec，默认输出上限为 256 MiB；不做颜色转换。压缩字节及其他图像字段仍保留在源对象中。未知插件 codec 抛出异常，不猜测其图片格式。

通过 `read_scene()` 读取并且 `PresenterData.associations_verified` 为true时，`model` 指向所属模型，NWD分配记录中的 `paths` 对应该模型的路径索引。`node_scope=true` 表示覆盖共享节点：用 `path_reference()` 取得节点身份，处理它的各次出现；路径范围则保持特定出现位置。NWF 通过 `load_project()` 并设置 `ProjectOptions.reader.products = true` 时，用 `Project.product_bindings` 返回 Presenter、TextureSpace、Hyperlink 和 NodeOverride 的候选关联。`owner` 是 NWF 的引用出现节点，`block/record` 指向该 source 的产品块和记录；`kind` 区分列表，`node_scope` 区分路径或节点范围。仅 `status == ProductBindingStatus::resolved` 时使用 `node/path` 访问联合模型目标。`empty` 表示空选择器，`unresolved` 表示缺少路径上下文或匹配不唯一，并使项目报告不完整。不同引用位置保持独立，源记录和共享几何不复制。单独 `read_products()` 不绑定模型。

`Document::read_scene()` 按模型范围读取几何压缩配置，分别选择坐标、法线、颜色、UV、带长度和带索引的编码；存在配置时按该配置单次解码。不同模型使用各自的配置，重复或损坏的配置会报错。`Options::normal_bits` 非零时显式覆盖法线精度；没有配置块时保留候选模式检查。

顶点属性保留各自的 `bits`、原始/打包数据和索引。短整型法线支持 1–16 位有效精度，字节颜色支持 1–8 位，浮点法线的打包载荷支持 0–32 位；`attribute_value()` 按属性类型还原数值。颜色先扩展到字节，低精度最大值不会强制变成 1。未压缩 UV 及压缩流内的浮点法线/UV 浮点值表均可读取。单独的 `decode_geometry()` 仍保留原有参数及记录封装约定。浮点法线超过 32 位的编码不支持。旧版共享对象的支持范围见下文。

浮点法线的 0 位值表分量返回零；1 位值表分量按保存编码解码，可能产生 NaN 或无穷值，`AttributeArray::finite` 为 false，文件读取路径给出顶点属性诊断。六个标准轴引用直接返回单位轴，不受值表精度影响；只含这些轴引用的数组保持有效。24–32 位分量按带符号整数转单精度浮点，保留该转换的舍入结果。原始浮点数组和压缩流内的浮点值表不套用打包精度。

有模型压缩配置时，`Options::normal_bits` 可用 1–32 显式覆盖精度，0 表示使用文件配置；属性实际表示以 `AttributeArray::bits` 为准，`Model::normal_bits` 仅作兼容提示。

量化坐标支持三个分量各自 0–32 位的编码，使用载荷内保存的精度和上下界还原坐标。`Geometry::coordinates` 保存解码后的共享值表，`coordinate_indices` 将顶点映射到值表；重复引用不会重复展开坐标。可选 `coordinate_quantization` 返回保存精度、六个上下界、四个位宽字节、值表数量及原始打包字节，未量化时为空。第四个位宽字节按原样保留，不参与 XYZ 解码。非有限输入或结果保留并令 `finite=false`，文件读取路径同时给出诊断；调用方应据此决定如何处理。库不会修正倒置的上下界或替换保存精度。

静态库和调用方应使用同一版本的头文件并一起重新编译，避免 `Geometry` 等公开类型的布局不一致。

`GeometryCompression` 返回原始压缩标志及法线、颜色、纹理坐标和坐标精度。内部版本 26 前只保存标志，未保存的精度字段为 0；调用方应结合来源版本区分缺失与保存的零值。此字段布局范围不表示同版本的整文件均可读取。

使用示例 [examples/products.cpp](examples/products.cpp) 展示逐块状态及保存项访问，构建后运行 `nwd_products_example model.nwd`。

NWD/NWC 支持 `lichunk-007/008` 容器及内部格式版本 `103/112/431/448`；内部版本不等同于软件发布年份。NWF 当前主要支持内部版本 `448` 的引用、路径关联和外观覆盖记录。

`appearance_stack(project, node, instance)` 返回模型保存的基础 Appearance，以及按内层到外层顺序排列的原始 NWF 覆盖记录。跨文件覆盖保留各自的 `source/model/appearance` 身份和 `Appearance.flags`；调用方可据 flags 解释哪些字段由该层提供。兼容字段 `appearance_overrides` 和 `effective_appearance()` 只投影最外层显式选择材质（flags bit 1）的覆盖；其它字段从分层接口读取。项目纹理活动性按最外层显式 asset 字段处理；未显式写入 asset 时沿用前一层。

以下内容仍存在限制：

- 碰撞自定义字段/状态、动画脚本/事件/动作仍未完整读取。
- LightWorks支持Blowfish/AES128、zlib及无加密/无压缩组合，支持 encoding0 编码密钥、encoding1 密钥标识、任意起始页和非顺序页链。归档内不属于根页链的独立物理页、未见内置类型与特殊值类型明确返回部分解析。部分标志语义、插件图像 codec 和复杂别名分支尚未完整覆盖。旧密钥与非顺序页链缺少充分的原生实样覆盖。
- 未知SQLite虚拟表等布局明确返回部分解析或错误；发布对象上的非法属性存储标志返回错误。新增版本分支不代表所有导出器均已覆盖。
- 图纸、现代灯光、空间树和SQLite记录已有读取接口，但部分枚举标志、数据库应用BLOB与模型关系、图纸来源与引用节点关系仍未完整解释。
- 数据库链接的非空配置已通过独立算法向量及记录测试，仍缺少非空原生样本验证。轴线的部分标志与参数保持源值；GUID 仓库尚未完成到模型对象的身份绑定。
- 旧式 TimeLiner 定义通过 `LegacyTimeLinerDefinitions` 返回；状态参数和旧名称保持原值，尚未全部转换为现代阶段枚举与外观关联。
- 碰撞容器支持内部版本 `103/112` 与 `301–450` 的已实现布局；`301–425` 的新增分支主要经过边界测试，缺少充分的真实文件验证，不能据此推断整文件兼容性。验证项为 `SavedItem.type == 58`，通过 `selection.path_links` 返回路径编号。
- NWD/NWC 保存项的显式 PathLink 支持分区绑定；NWF 保存项支持引用出现绑定及选择集名称定位；属性树等其他定位根、动态搜索和跨保存项关系尚未全部求值。部分关键帧和产品配置分支缺少非空原生文件验证，未知子类型保留诊断。
- 外部 `.rcs` 路径成功解析时，项目加载器通过 Schema 的唯一 `int32 index` 关联 RCS voxel；RCP 容器、其他插件几何和坐标变换上下文仍未完整支持。
- NWF 的混合单位变换、源单位重定义、多分区聚合放置，以及跨文件部分 Appearance 的最终字段合成语义仍未完整支持；原始覆盖层和字段标志已保留。
- 嵌套引用中的对象覆盖、源模型已有覆盖的重置仍有限制，项目会报告不完整并拒绝提供对应世界矩阵。
- 纹理映射向量和方向枚举尚未全部赋予通用语义；部分布局缺少非空原生样本验证。
- 某些复杂联合树需要额外的 GUID 或校验值才能消歧；未知 schema、对象类型或资产变体可能无法解析。

`Project.complete` 表示本次加载范围内未检测到缺失或不支持项，不代表所有格式变体均受支持，也不代表整个文件已经完全解析。调用方应同时检查 `warnings`、引用节点状态、`placement_supported`、纹理和产品块状态。设置 `load_textures = false` 时，完整状态不包含纹理检查；关闭 `reader.products` 时，不检查尚未读取的产品块。

缺失或歧义引用会保留诊断，不随意选择同名文件；未知布局和越界数据会报错。部分解码接口可能抛出 `nwd::Error`，文件及索引访问也可能产生标准 C++ 异常，调用方应设置异常边界。

引用 RVT、DWG 等原始设计文件时，需要提供已导出的 NWC，并通过路径映射关联；本库不执行原生格式导出器。

库负责提取模型信息。曲面离散化、通用图片格式解码、着色器和渲染由调用方实现；LightWorks 内置图像 codec 可使用上述按需接口。

## 第三方组件

使用 zlib 进行解压，使用 JSON for Modern C++ 处理引用表与材质资产，使用 SQLite 在内存中读取嵌入数据库，复用 tiny-AES-c 与 Blowfish 实现读取格式内的编码载荷。版本及许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

Schema XML 使用固定版本 Expat 2.8.5，静态链接并隔离符号；源码及许可证随库提供，构建无需联网。

## 许可证 / License

本项目原创代码采用 [PolyForm Noncommercial License 1.0.0](LICENSE)，项目署名见 [NOTICE](NOTICE)。符合许可证条款的非商业用途可获得相应的使用、修改和分发许可，无须向维护者支付许可费；具体范围以正式英文条款为准。请阅读[非正式中文摘要](LICENSE.zh-CN.md)。拟进行商业使用者应在使用前联系维护者；超出英文许可证授权范围的商业使用须另行协商付费授权并签署书面协议，详见[商业授权政策](COMMERCIAL-LICENSING.md)。公开仓库本身不自动授予商业使用权。第三方组件仍受各自许可证约束，见[第三方声明](THIRD_PARTY_NOTICES.md)。由于商业用途受到限制，本项目属于源码可见许可，不应称为符合 OSI 开源定义的开源软件。

The project's original code is offered under the [PolyForm Noncommercial License 1.0.0](LICENSE); attribution is in the project [NOTICE](NOTICE). Use, modification, and distribution for permitted noncommercial purposes are subject to the official English terms, with no license fee payable to the maintainer for those permitted uses; a [nonofficial Chinese summary](LICENSE.zh-CN.md) is also available. Anyone planning commercial use should contact the maintainer beforehand; commercial use outside the English license grant requires a separately negotiated paid license and a signed written agreement. See the [commercial licensing policy](COMMERCIAL-LICENSING.md). A public repository does not automatically grant commercial use rights. Bundled third-party components keep their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). Because commercial use is restricted, this is a source-available license and should not be described as open source under the OSI Open Source Definition.
