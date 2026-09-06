# nwd-reader

独立的 C++20 Navisworks 文件解析库，面向模型导入、格式转换和工程数据处理，提供**几何、属性、材质、纹理和结构树**的读取接口。

支持读取 NWD、NWC 中的嵌入模型，以及 NWF 引用的模型文件。无需安装 Navisworks，不依赖 Autodesk SDK 或运行时。

## 功能

- **几何**：读取三角带、折线、点、文字及圆、圆柱等参数化几何，保留坐标索引、法线、颜色和 UV。
- **实例与变换**：共享几何通过实例引用，保留变换、单位和对象关联，避免按实例复制顶点。
- **属性与结构树**：读取分区、模型层级、属性分组及有类型的属性值，提供子节点和实例查询索引。
- **材质**：读取环境色、漫反射、高光、自发光、光泽度和透明度，以及资产节点、参数和贴图连接。
- **纹理**：保留内嵌文件的原始字节，支持外部纹理查找、路径重映射和可用状态查询。
- **引用加载**：递归加载 NWF 引用，复用同一源文件，同时保留各次引用的独立节点和材质覆盖。
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
| `load_project()` | 加载文件及其引用，返回项目节点、共享源和纹理 |
| `Project.sources` / `Project.nodes` | 共享源文件与独立引用出现构成的文件树 |
| `Model` / `ModelIndex` | 模型几何、实例、属性、结构路径及查询索引 |
| `path_reference()` / `resolve_object()` | 从结构路径定位对象，再沿属性引用访问数据 |
| `triangle_indices()` / `attribute_value()` | 读取三角槽索引以及槽对应的法线、颜色或 UV |
| `world_matrix()` / `project_world_matrix()` | 获取源单位矩阵或项目米制矩阵 |
| `effective_appearance()` / `appearance_arena()` | 定位应用项目覆盖后的实例外观和所属材质存储区 |
| `describe_asset()` | 获取材质资产节点、参数、连接和纹理 URI |
| `Project.textures` | 获取纹理归属、路径、状态和共享原始字节 |

### 几何、属性与对象身份

`ModelIndex::children(path)` 返回直接子节点，`instances(path)` 返回直接关联的实例；遍历整棵树时继续访问子节点。实例通过 ID 引用共享几何和外观，应保留这种关联以降低内存占用。

`triangle_indices()` 返回的索引指向顶点槽。位置通过 `geometry.coordinate_indices[slot]` 索引 `geometry.coordinates`，顶点属性通过 `attribute_value(attribute, slot)` 获取，以保留 UV 和法线接缝。

对象属性使用 `(graph, object)` 引用，字符串 ID 只在所属图中有效。`Property.name` 指向同图的名称对象；`Value` 保留字符串、整数、数值、向量或对象引用等源类型。不要跨图直接比较对象 ID。

对象引用和字符串视图依赖所属模型的生命周期；查询索引依赖原模型，模型修改后需要重建索引。

### 单位、材质与纹理

矩阵使用列主序。`world_matrix()` 保持源单位，`model.meters_per_unit` 提供到米的比例。`project_world_matrix()` 将受支持的引用放置和单位换算组合为米制矩阵；遇到不支持的引用放置会抛出异常。

材质通过 `Material` 的 `ambient()`、`diffuse()`、`specular()`、`emissive()`、`shininess()` 和 `transparency()` 访问。资产图保留原始参数类型、连接和 URI，完整 JSON 仍在 `Asset.json` 中，便于调用方处理扩展字段。

纹理保留编码后的原始字节，不做图片解码。通过 `TextureFile` 的源、模型、资产及别名关联材质；`active` 和 `thumbnail` 区分活动资产与缩略图。外部文件可使用 `ProjectOptions.search_paths` 查找，或通过 `remaps` 指定原路径到本机文件的映射。

## 支持范围与错误处理

NWD/NWC 支持 `lichunk-007/008` 容器及内部格式版本 `103/112/431/448`；内部版本不等同于软件发布年份。NWF 当前主要支持内部版本 `448` 的引用与完整材质覆盖。

以下内容仍存在限制：

- 外部 RCS 等几何返回描述和源路径，尚不提供点云坐标。
- NWF 的额外引用变换、单位重定义、非空片段变换、纹理空间覆盖和部分材质覆盖合成尚未完整支持。
- 某些复杂联合树需要额外的 GUID 或校验值才能消歧；未知 schema、对象类型或资产变体可能无法解析。

`Project.complete` 表示本次加载未检测到缺失或不支持项，不代表所有格式变体均受支持。调用方应同时检查 `warnings`、引用节点状态、`placement_supported` 和纹理状态。设置 `load_textures = false` 时，完整状态不包含纹理检查。

缺失或歧义引用会保留诊断，不随意选择同名文件；未知布局和越界数据会报错。部分解码接口可能抛出 `nwd::Error`，文件及索引访问也可能产生标准 C++ 异常，调用方应设置异常边界。

库负责提取模型信息。曲面离散化、图片解码、着色器和渲染由调用方实现。

## 第三方组件

使用 zlib 进行解压，使用 JSON for Modern C++ 处理引用表与材质资产。版本及许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
