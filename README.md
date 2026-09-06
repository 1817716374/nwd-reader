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
- **引用加载**：递归加载 NWF 引用，复用同一源文件，同时保留各次引用的独立节点、材质覆盖、对象变换和纹理映射记录。
- **视点与选择集**：读取视点目录、相机与裁剪设置、批注、外观覆盖、静态选择和有类型的搜索条件。
- **动画**：读取场景、轨道、关键帧、时间和插值开关，以及平移、旋转、缩放、颜色、不透明度和相机参数。
- **TimeLiner**：读取任务层级、计划与实际日期、进度、费用、数据源字段映射、仿真外观和列设置，以及旧式任务类型、外观和默认状态定义。
- **碰撞**：读取测试、结果、结果组、规则、审批与模拟事件，以及验证选择中保存的路径编号。
- **轴网**：读取系统坐标框架、轴线直线段和圆弧、楼层标高、活动系统与锁定层。
- **数据库链接**：读取保存的 SQL、连接字符串、开关和字段映射，保留共享名称与原始编码字节。
- **文件数据库**：在内存中读取 SQLite 表结构、行、列、原始单元格类型、外键和定义，不执行源视图或外部数据库查询。
- **空间树**：读取原生空间层级及分片槽引用，保留父子结构并关联模型实例。
- **旧式 Presenter**：读取 LightWorks 实体树、有类型的着色参数、编码图像、灯光、材质槽分配和纹理映射，保留归档内共享身份与路径关联。
- **灯光与文档信息**：读取现代灯光资产、位置/目标点、图纸目录、默认图纸、发布字段、缓存配置、超链接、序列号和几何压缩参数。
- **文档辅助数据**：读取 GUID 仓库、统计报告、节点覆盖、工具与图形设置。
- **读取状态**：保留块目录，区分已解析、部分解析、读取失败和尚未由产品模块处理的内容。
- **并行读取**：支持几何记录、压缩块和属性页并行处理；线程数可配置。

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
| `effective_appearance()` / `appearance_arena()` | 定位应用项目覆盖后的实例外观和所属材质存储区 |
| `describe_asset()` | 获取材质资产节点、参数、连接和纹理 URI |
| `Scene.schemas` / `Model.schema_references` | 共享字段定义及分区引用 |
| `NwfData.option_values` / `NwfReference.cache_plugins` | 缓存插件配置、嵌套选项及枚举对象 |
| `Project.textures` | 获取纹理归属、路径、状态和共享原始字节 |

### 几何、属性与对象身份

`ModelIndex::children(path)` 返回直接子节点，`instances(path)` 返回直接关联的实例；遍历整棵树时继续访问子节点。实例通过 ID 引用共享几何和外观，应保留这种关联以降低内存占用。

`triangle_indices()` 返回的索引指向顶点槽。位置通过 `geometry.coordinate_indices[slot]` 索引 `geometry.coordinates`，顶点属性通过 `attribute_value(attribute, slot)` 获取，以保留 UV 和法线接缝。

对象属性使用 `(graph, object)` 引用，字符串 ID 只在所属图中有效。`Property.name` 指向同图的名称对象；`Value` 保留字符串、整数、数值、向量或对象引用等源类型。不要跨图直接比较对象 ID。

`Value` 的整数类型还包括 tag14 无符号32位、tag15 有符号64位、tag16 无符号64位。有符号值读取 `integer`，无符号值读取 `unsigned_integer()`；后者复用原有存储位，不增加每个 Value 的内存大小，也不经过浮点转换。

对象引用和字符串视图依赖所属模型的生命周期；查询索引依赖原模型，模型修改后需要重建索引。

`Scene.schemas` 保存字段名、显示名、类型、默认值和嵌套结构。`ExternalGeometry.schema` 与 `Model.schema_references` 使用这个表的零基 ID，`none` 表示空引用。外部描述的 `properties.children` 与定义中的字段顺序对应；`payload_decoded` 表示描述载荷已经读取，外部点坐标是否可用需要另外判断。GUID 保留原始16字节。

NWF 缓存插件及选项保留在引用记录中。`CacheOption.value` 的字符串 ID 和对象引用属于 `NwfData.option_values`，枚举对象继续共享；这些设置仅供调用方读取。

旧式 Protein 材质属性保留有类型的字段、连接和 URI。`ProteinProperty.embedded_files` 索引所属 `ObjectGraph.embedded_files`，可取得内嵌 URI 与二进制资源的原始字节、前后缀及所属对象。此类资源直接通过属性图访问；`Project.textures` 的活动状态查询目前针对实例关联的 JSON 资产。

### 单位、材质与纹理

矩阵使用列主序。`world_matrix()` 保持源单位，`model.meters_per_unit` 提供到米的比例。`project_world_matrix()` 将受支持的引用放置和单位换算组合为米制矩阵；遇到不支持的引用放置会抛出异常。

同单位 NWF 引用支持替换源模型的基准变换，包含旋转、平移、缩放和剪切；源几何仍共享。`model_base_matrix()` 返回源基准矩阵，`reference_placement_matrix()` 返回引用所需的米制修正矩阵。方向提示的变化单独保存在节点的 `orientation_changed` 中。

直接、同单位引用且源模型没有已保存的对象覆盖时，`project_world_matrix()` 还会应用 NWF 对象变换。`nwf_transform_matrix()` 提供原始载荷到列主序矩阵的转换，`Project.transform_overrides` 保留每个实例的米制覆盖矩阵。

`Instance.auxiliary_transform` 引用 `Model.auxiliary_transforms` 中的辅助仿射、平移或平移＋旋转记录。这些数据保持源值，与当前实例矩阵分开返回。

材质通过 `Material` 的 `ambient()`、`diffuse()`、`specular()`、`emissive()`、`shininess()` 和 `transparency()` 访问。资产图保留原始参数类型、连接和 URI，完整 JSON 仍在 `Asset.json` 中，便于调用方处理扩展字段。

纹理保留编码后的原始字节，不做图片解码。通过 `TextureFile` 的源、模型、资产及别名关联材质；`active` 和 `thumbnail` 区分活动资产与缩略图。外部文件可使用 `ProjectOptions.search_paths` 查找，或通过 `remaps` 指定原路径到本机文件的映射。

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

使用示例 [examples/products.cpp](examples/products.cpp) 展示逐块状态及保存项访问，构建后运行 `nwd_products_example model.nwd`。

NWD/NWC 支持 `lichunk-007/008` 容器及内部格式版本 `103/112/431/448`；内部版本不等同于软件发布年份。NWF 当前主要支持内部版本 `448` 的引用与完整材质覆盖。

以下内容仍存在限制：

- 碰撞自定义字段/状态、动画脚本/事件/动作仍未完整读取。
- LightWorks支持Blowfish/AES128、zlib及无加密/无压缩组合，支持 encoding0 编码密钥、encoding1 密钥标识、任意起始页和非顺序页链。归档内不属于根页链的独立物理页、未见内置类型与特殊值类型明确返回部分解析。部分标志语义、插件图像 codec 和复杂别名分支尚未完整覆盖。旧密钥与非顺序页链缺少充分的原生实样覆盖。
- Publish附加属性、未知SQLite虚拟表等布局明确返回部分解析或错误；新增版本分支不代表所有导出器均已覆盖。
- 图纸、现代灯光、空间树和SQLite记录已有读取接口，但部分枚举标志、数据库应用BLOB与模型关系、图纸来源与引用节点关系仍未完整解释。
- 数据库链接的非空配置已通过独立算法向量及记录测试，仍缺少非空原生样本验证。轴线的部分标志与参数保持源值；GUID 仓库尚未完成到模型对象的身份绑定。
- 旧式 TimeLiner 定义通过 `LegacyTimeLinerDefinitions` 返回；状态参数和旧名称保持原值，尚未全部转换为现代阶段枚举与外观关联。
- 碰撞容器支持内部版本 `103/112` 与 `301–450` 的已实现布局；`301–425` 的新增分支主要经过边界测试，缺少充分的真实文件验证，不能据此推断整文件兼容性。验证项为 `SavedItem.type == 58`，通过 `selection.path_links` 返回路径编号。
- NWD/NWC 保存项的显式 PathLink 支持分区绑定；NWF 保存项支持引用出现绑定及选择集名称定位；属性树等其他定位根、动态搜索和跨保存项关系尚未全部求值。部分关键帧和产品配置分支缺少非空原生文件验证，未知子类型保留诊断。
- 外部 RCS 等几何返回描述、schema字段、包围盒和源路径，尚不提供点云坐标。
- NWF 的混合单位变换、源单位重定义、多分区聚合放置和部分材质覆盖合成尚未完整支持。
- 嵌套引用中的对象覆盖、源模型已有覆盖的重置仍有限制，项目会报告不完整并拒绝提供对应世界矩阵。
- 纹理映射向量和方向枚举尚未全部赋予通用语义；部分布局缺少非空原生样本验证。
- 某些复杂联合树需要额外的 GUID 或校验值才能消歧；未知 schema、对象类型或资产变体可能无法解析。

`Project.complete` 表示本次加载范围内未检测到缺失或不支持项，不代表所有格式变体均受支持，也不代表整个文件已经完全解析。调用方应同时检查 `warnings`、引用节点状态、`placement_supported`、纹理和产品块状态。设置 `load_textures = false` 时，完整状态不包含纹理检查；关闭 `reader.products` 时，不检查尚未读取的产品块。

缺失或歧义引用会保留诊断，不随意选择同名文件；未知布局和越界数据会报错。部分解码接口可能抛出 `nwd::Error`，文件及索引访问也可能产生标准 C++ 异常，调用方应设置异常边界。

引用 RVT、DWG 等原始设计文件时，需要提供已导出的 NWC，并通过路径映射关联；本库不执行原生格式导出器。

库负责提取模型信息。曲面离散化、通用图片格式解码、着色器和渲染由调用方实现；LightWorks 内置图像 codec 可使用上述按需接口。

## 第三方组件

使用 zlib 进行解压，使用 JSON for Modern C++ 处理引用表与材质资产，使用 SQLite 在内存中读取嵌入数据库，复用 tiny-AES-c 与 Blowfish 实现读取格式内的编码载荷。版本及许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
