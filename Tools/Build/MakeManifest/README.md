# MakeManifest

宿主侧 C++20 工具：生成内核集清单 `Manifest.mft`（格式标记 `LPMS`，后缀见 [后缀设计](../../../Docs/Specs/后缀设计.md)），签名区留零，再由 [SignManifest](../SignManifest/README.md) 填写签名。编译后输出 `Tools/Bin/MakeManifest`。

清单的字节表、产物条目与 `BuildId` 的来源见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)第二节；本工具与引导侧的解析共用 `Packages/Common/Include/Manifest.hpp` 一处偏移定义，两侧不会各写一份。

```sh
make -C Tools/Build/MakeManifest
Tools/Bin/MakeManifest --out Out/Manifest.mft --build-id-from Out/LikesProgram.os \
    --security-version 1 --artifact 1:LikesProgram.os:Out/LikesProgram.os
```

## 参数

| 参数 | 含义 |
| --- | --- |
| `--out` | 输出的清单路径 |
| `--build-id-from` | 从内核镜像数据的 `BuildId` 字段取集标识（要求该文件已由 `PackImage` 填充并通过校验） |
| `--build-id` | 显式给出 64 位十六进制的集标识（64 字符）；与 `--build-id-from` 必须给出且只给一个 |
| `--security-version` | 单调安全版本号，发行侧给定、只增不减；比较规则见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)第六节 |
| `--artifact` | 一条产物：`<type>:<相对路径>:<文件>`；`type` `1` = `.os`、`2` = `.osm`；可多次，上限 32 条 |

## 产物与约定

- 每条产物登记相对集目录的路径、字节数与来源文件的 SHA-256；路径必须是相对路径，不得以 `/` 开头、不得含空分量与 `.` / `..`
- 产物里必须有一条 `.os`：`SignManifest` 与引导侧验证都会核对，引导侧要求恰好一条
- 签名区（算法、密钥编号、保留字节与 64 字节签名值）全部写零，等待 `SignManifest` 填写
- **清单生成后不得再改**：签名工具只填签名区，清单的其他字节一变就要重新生成并重签
- 内容确定性：字段顺序固定、无时间戳、产物按命令行给出的顺序排列、保留字段写 0，同一份输入产出同样的字节
- 清单身份摘要 = 把签名值视作 64 个零后对整份清单的 SHA-256，与签名覆盖的是同一段字节（引导侧据此计算并交给状态记录）

谁在用它：`Packages/LikesProgramOS/Makefile` 在把内核映像放进 `KernelSets/<BuildId>/` 之后生成清单，紧接着调 `SignManifest` 签名。
