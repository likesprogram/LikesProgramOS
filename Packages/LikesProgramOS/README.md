# LikesProgramOS — 系统本体

本包是操作系统本体，与引导器子项目 `Packages/Baleen` 平级。卷内目录树的正式定义在[系统卷布局](../../Docs/Specs/Common/系统卷布局.md)，本包负责把它组装成系统卷镜像。

## 产出

| 产物 | 来源 | 说明 |
| --- | --- | --- |
| `Core/Out/Bin/LikesProgram.os` | [Core](Core/README.md) | 内核映像：文件起点是内核镜像数据，`0x80` 起是长模式入口与内核侧第一阶段 |
| `Static/LikesProgram/System/LikesProgram.os` | 本目录 Makefile 从 Core 的 `Out` 拷入 | 内核映像在卷内的落点，拷贝产物不进 Git |
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

系统卷由 `mke2fs -d` 从卷根副本就地写入，不挂载、不需要 root；卷大小、卷标识与内核路径都可按顶层模板覆盖（见 `make help`）。**内核路径必须与布局描述的默认值一致**：`/LikesProgram/System/LikesProgram.os`，覆盖 `KERNEL_PATH` 时布局模板要一起改。

`LikesProgram.os` 现在是内核的原生引导镜像本体：内核镜像数据加第一阶段代码。`.os` 作为容器（`kind` 元数据、同集 `.osm` 与构建清单）的内部格式随[模块与驱动装载](../../Docs/Specs/模块与驱动装载.md)与[内核集更新与回滚](../../Docs/Specs/内核集更新与回滚.md)的阶段确定，届时本包按同一布局升级产物形态。
