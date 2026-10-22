# LikesProgramOS — 系统本体

本包是操作系统本体，与引导器子项目 `Packages/Baleen` 平级。卷内目录树的正式定义在[系统卷布局](../../Docs/Specs/Common/系统卷布局.md)，本包负责把它组装成系统卷镜像。

## 产出

| 产物 | 来源 | 说明 |
| --- | --- | --- |
| `Core/Out/Bin/LikesProgram.os` | [Core](Core/README.md) | 内核映像：文件起点是内核镜像数据，`0x80` 起是长模式入口与内核侧第一阶段 |
| `Static/LikesProgram/System/KernelSets/<BuildId>/` | 本目录 Makefile 从 Core 的 `Out` 拷入并生成签名清单 | 内核集的卷内落点（`LikesProgram.os` 与 `Manifest.mft`），随构建重建、不进 Git |
| `Out/LikesProgram.img` | 本目录 Makefile | Ext4 系统卷，卷根来自 `Static/`；顶层组装器读它的卷标识与长度写进布局描述 |

## `Static/` 是系统卷根的源

`Static/` 下的目录树按原样写进系统卷，卷内路径直接对应目录里的同一路径：

```text
Static/
├── LikesProgram/          系统目录：随系统更新，用户不单独增删
│   ├── System/            内核集：LikesProgram.os（构建期拷入）、Modules/、Drivers/
│   ├── Programs/          系统程序：Init、Session、桌面、外壳与系统服务
│   ├── Config/            系统配置
│   └── Resources/         系统资源
├── ProgramFiles/          可单独卸载的程序
└── Users/                 用户目录
```

`.keep` 是 Git 的存在标记（空目录不进 Git），组装时被丢掉，不写进系统卷。各模块产物由本目录 Makefile 从自己的 `Out/` 拷进 `Static/` 的对应落点——当前是内核映像，`Modules/`、`Drivers/`、`Programs/`、`ProgramFiles/` 的落点随对应模块阶段接入；映射逐项列在 Makefile 里。

## 构建

```sh
make                 # 构建系统卷（卷根来自 Static/，内含内核映像）
make kernel          # 只构建内核映像并复核
make check           # 复核内核映像与卷内落点
make clean-all       # 连同 Core 的产物一起清理
```

系统卷由 `mke2fs -d` 从卷根副本就地写入，不挂载、不需要 root；卷大小、卷标识与内核路径都可按顶层模板覆盖（见 `make help`）。内核集落在 `/LikesProgram/System/KernelSets/<BuildId>/`，当前集的卷内路径写进 `Out/KernelPath`，由顶层组装器传给 MakeImage / TestInstaller 回填布局描述；`KERNEL_PATH` 变量已不再使用，落点由 `BuildId` 决定。

`LikesProgram.os` 现在是内核核心的原生引导镜像本体：内核镜像数据加内核核心代码，镜像数据里的 `ModuleMajor` 是发布线。构建链按**发布线身份**组织：本包先产出 `BuildId` 与 `Digest` 全零的原型，`MakeManifest` 在系统卷组装期派生发布线 `BuildId`、写入最终 `.os`、生成 `Modules.list` 与清单；集目录名与卷内路径都由该 `BuildId` 推出，模块补丁不改变它。`Modules/` 的落点、13 个引导闭包模块的打包与集目录、模块修订与版本保留已由 P5 的 M1（打包与构建链接入）、M2（修订快照与选择）与 M5/M6（真实模块与运行期夹具）落地（见[模块修订与更新](../../Docs/Specs/模块修订与更新.md)、[内核模块容器](../../Docs/Specs/内核模块容器.md)与[内核架构与自举闭包](../../Docs/Specs/内核架构与自举闭包.md)）。`.os` 作为容器的 `kind` 元数据格式属于后续容器阶段，本文不提前定义。
