# MakeKeys

宿主侧 C++20 工具：生成、复用与导出开发期的 Ed25519 密钥，并从公钥生成引导器内嵌的信任锚源文件。编译后输出 `Tools/Bin/MakeKeys`。

密钥目录默认放 `.cache/Keys`（已被 Git 忽略，**不进仓库、不进镜像**）：`Development.sec` 是 32 字节种子（权限 600），`Development.pub` 是对应公钥。已有密钥一律复用、不覆盖；**换锚 = 换密钥文件并重建引导器**，轮换与撤销的规则见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)第四、五节。

```sh
make -C Tools/Build/MakeKeys
Tools/Bin/MakeKeys --out .cache/Keys                                                 # 生成或复用开发期密钥
Tools/Bin/MakeKeys --out .cache/Keys --anchor-cpp Out/TrustAnchors.cpp --key-id 1    # 导出信任锚源
Tools/Bin/MakeKeys --out .cache/Keys --anchor-cpp Out/TrustAnchors.cpp --key-id 1 \
    --anchors Packages/Baleen/Core/Anchors/Retained.txt                              # 带上轮换窗口里的保留锚
```

## 参数

| 参数 | 含义 |
| --- | --- |
| `--out` | 密钥目录；`Development.sec` 与 `Development.pub` 写在这里 |
| `--key-id` | 当前密钥的信任锚编号，取值 `1`..`255`，默认 `1`；与清单签名区的 `KeyId` 是同一编号 |
| `--anchors` | 附加锚输入（进版本库的轮换窗口列表）：每行 `<KeyId> <公钥 64 位十六进制>`，`#` 开头与空行忽略；与当前密钥合并成同一张表，默认不读；须与 `--anchor-cpp` 一起用 |
| `--anchor-cpp` | 生成的 C++ 源路径：定义 `Baleen::Crypto` 的 `kTrustAnchors` 与 `kTrustAnchorCount`，由 BaleenCore 编译进镜像 |

## 产物与约定

- 种子从 `/dev/urandom` 取，公钥私钥由种子派生（Ed25519，RFC 8032）；种子文件长度不是 32 字节时直接报错，不猜测
- `--anchor-cpp` 的产物是生成物：表内条目按 `KeyId` 升序，同一组输入（密钥 + 附加锚输入）导出同样的文本；密钥不变时重复构建不换锚
- **新旧两把公钥并存的复现路径**：轮换窗口里的旧公钥写在进版本库的附加锚输入里（`Packages/Baleen/Core/Anchors/Retained.txt`），当前密钥来自仓库外的种子；`MakeKeys --anchors` 把两者合并成同一张表，因此“两把公钥并存”由仓库里的输入复现，不依赖本地产物目录
- 同一 `KeyId` 出现两次（当前密钥与附加锚输入撞号、或输入内部重复）直接报错，不按顺序静默取一条；条目数超容量由生成源里的 `static_assert` 在编译期拦下，上限是 4 条（轮换窗口两把并存，另留吊销授权与轮换余量，见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)第四节）
- 引用的实现是 `Tools/Common` 的 `Ed25519` 与 `Sha512`，与目标侧 `Packages/Baleen/Common` 的同一算法由 [CheckCrypto](../../Test/CheckCrypto/README.md) 互验；轮换窗口（两把公钥并存、按 `KeyId` 选钥验签）与生成器输出也由该工具的用例覆盖
- **锚表少于两条时给出警告**（不失败）：签名吊销记录必须由一把未被吊销的密钥签，表里只有一把密钥时，它泄露之后没有任何密钥能签这份记录。第二把要在轮换窗口就装进设备，所以警告指向附加锚输入 `--anchors`；判定与理由见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 5.1

谁在用它：`Packages/Baleen/Core/Makefile` 生成密钥并按 `Anchors/Retained.txt` 导出 `Out/TrustAnchors.cpp`（锚编进 BaleenCore 镜像），`Packages/LikesProgramOS/Makefile` 用同一把密钥签内核集清单。
