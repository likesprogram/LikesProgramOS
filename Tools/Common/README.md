# Common — 宿主工具的跨分类共享代码

放在这里的代码被 `Tools/Build`（构建期工具）与 `Tools/Test`（测试与回归工具）两侧共用，**不是**目标系统的容器或模块：

| 模块 | 内容 |
| --- | --- |
| `Sha256` | SHA-256 摘要算法的宿主侧实现，与目标侧 `Packages/Common` 同算法、用测试向量与真实镜像互验 |
| `Sha512` | SHA-512 摘要算法的宿主侧实现，Ed25519 的内部依赖，与目标侧同算法、用 FIPS 180-4 向量与两侧互验 |
| `Ed25519` | Ed25519 签名算法（RFC 8032）的宿主侧实现，与目标侧同算法、用标准向量与两侧互验；供 MakeKeys 生成密钥与信任锚、SignManifest 签清单 |
| `BaleenImage` | Baleen 镜像完整性头：Stub 与 BaleenCore 同构的字段常量、BuildId 与 Digest 填充、头字段与摘要校验，供 PackImage、MakeImage、TestInstaller、MakePayloads 与 CheckStub 使用 |
| `HostIo` | 宿主文件读取、数字解析、布局记账（区间不重叠）与清单 JSON 片段 |

为什么放在这一层：镜像完整性头是构建与回归两侧共同判定的同一份格式——组装器用它放行 Stub 与 BaleenCore 载荷，CheckStub 用它核对被测镜像的头与摘要；两侧各写一份就会在字段、摘要范围或长度上限上悄悄漂移，而被检查的与被放行的必须是同一套判定。密码学实现同理：清单验签的签名侧（宿主工具）与验签侧（BaleenCore）必须同一套算法与编码，`CheckCrypto` 用两边的实现互相验证同一批向量。只被构建期工具用到的宿主代码在 [Build/Common](../Build/Common/README.md)。

约定：C++20，无外部依赖，头文件在 `Include/`、实现在 `Src/`；写出的字段值与 `Packages/Baleen/Common/Include/ImageHeader.hpp` 的镜像头布局、`Packages/Baleen/Stub/Include/StubHeader.inc` 保持一致，改动时必须同步。各工具用 `vpath` 把这里的源码编译进自己的 `Out/Obj`，因此每个工具目录仍可单独构建。
