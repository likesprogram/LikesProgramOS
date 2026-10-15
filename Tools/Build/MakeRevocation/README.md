# MakeRevocation

宿主侧 C++20 工具：生成并签名内核集签名吊销记录（`LPRV`）。编译后输出 `Tools/Bin/MakeRevocation`。

字节表、签名覆盖与签名者规则见[内核集清单与签名](../../../Docs/Specs/内核集清单与签名.md) 5.1；记录在系统卷里的落点是 `/LikesProgram/System/Revocation.rvk`。工具随源码交付，手工执行。

```sh
make -C Tools/Build/MakeRevocation

# 吊销 KeyId 3，下限抬到 9，纪元 1
Tools/Bin/MakeRevocation --key .cache/Keys/Development.sec --key-id 1 --epoch 1 \
    --min-security-version 9 --revoke-key-id 3 --out Revocation.rvk
```

## 用法

| 选项 | 说明 |
| --- | --- |
| `--key` | 32 字节 Ed25519 种子；私钥不进仓库、不进镜像（格式同 `MakeKeys` 产出的 `Development.sec`） |
| `--key-id` | 签名密钥编号，取值 `1`..`255`，默认 `1`；设备侧按它在内嵌信任锚表里查公钥 |
| `--epoch` | 吊销纪元，取值 `1`..`2^64-1`；只增不减，回滚成旧纪元不解除吊销 |
| `--min-security-version` | 记录给出的最低允许清单安全版本，默认 `0`；低于它的一律拒绝 |
| `--revoke-key-id` | 吊销一个签名密钥编号，可重复；不得与签名者相同 |
| `--out` | 输出路径，默认 `Revocation.rvk` |
| `-h` / `--help` | 打印用法 |

## 约束与自检

- **确定性**：同一组输入产出同样的字节（无时间戳、无随机量）；签名之后记录的任何字节变化都必须重新生成并重签
- **签名者不得自吊销**：签名者出现在本记录的吊销位图里时工具直接拒绝——引导侧同样拒绝这种记录，提前拦下比事后排查便宜
- **结构自检**：生成后调用 `Packages/Common` 的 `Revocation::CheckRecord`（引导侧同一份实现）核对标记、版本、长度、保留字段与签名者编号
- **签名自检**：签名必须能被对应公钥验证，防止写出不可验的记录

## 不做的事

- 不把记录写进镜像或系统卷：落盘是组装与分发环节的事（把 `Revocation.rvk` 放进系统卷的 `/LikesProgram/System/`）
- 不改清单、不动内核集、不生成密钥（密钥由 [`MakeKeys`](../MakeKeys/README.md) 生成）
- 不代表“实时吊销”：设备端从不联网查询，记录经安装包、U 盘或整盘镜像离线投递，撤销的生效点是下一次引导读取这份记录
