# SignManifest

宿主侧 C++20 工具：用 Ed25519 私钥填写内核集清单的签名区。编译后输出 `Tools/Bin/SignManifest`。

签名覆盖按[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 2.1：签名值 64 字节视作零，算法与密钥编号留在覆盖范围内，对整份清单签名；签名之后清单的任何字节变化都必须重新生成并重签。引导侧按同一段字节验签，用的是编在 BaleenCore 里的信任锚（见 [MakeKeys](../MakeKeys/README.md)）。

```sh
make -C Tools/Build/SignManifest
Tools/Bin/SignManifest --in Packages/LikesProgramOS/Static/LikesProgram/System/KernelSets/<BuildId>/Manifest.mft \
    --key .cache/Keys/Development.sec --key-id 1
```

## 参数

| 参数 | 含义 |
| --- | --- |
| `--in` | 已生成的清单；签名前逐项核对结构——标记、版本、头长、长度字段、标志与保留字段、条目序列、条目数、至少一条 `.os` |
| `--key` | 32 字节 Ed25519 种子文件；私钥不进仓库、不进镜像，长度不符即报错 |
| `--key-id` | 密钥编号，取值 `1`..`255`，默认 `1`；验签方按它在内嵌信任锚表里查公钥 |
| `--out` | 输出路径；给出时不动输入文件，不给时就地写回 |

## 行为与约定

- 写出的签名先自检：用派生的公钥对签名结果验一遍，通过才落盘
- 只改签名区的字节（算法 `1`、`KeyId`、保留写 0 与 64 字节签名值）；清单其余字节原样保留
- 换密钥或换 `KeyId` 要连同引导器一起更新——锚在引导器里，旧锚验不过新签名，见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md)第五节的轮换顺序
- 引用的实现是 `Tools/Common` 的 `Ed25519`、`Sha512` 与 `Sha256`，与目标侧 `Packages/Baleen/Common` 的同一算法由 [CheckCrypto](../../Test/CheckCrypto/README.md) 互验

谁在用它：`Packages/LikesProgramOS/Makefile` 在 `MakeManifest` 之后签名清单；种子文件由 `MakeKeys` 生成在 `.cache/Keys`。
