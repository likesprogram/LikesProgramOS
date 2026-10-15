# CheckCrypto

宿主侧 C++20 检查工具：SHA-512、Ed25519 与信任锚表轮换窗口的标准向量、两侧实现互验、生成端到端，以及目标侧源码的 32 位双编译与无运行库链接检查。手工执行，不进入默认构建。

```sh
make -C Tools/Test/CheckCrypto
Tools/Bin/CheckCrypto                  # 用例 + 32 位双编译 + 目标侧链接 + 信任锚生成检查
Tools/Bin/CheckCrypto --keep           # 保留工作目录现场
Tools/Bin/CheckCrypto --work <目录>     # 指定工作目录（默认在临时目录下按进程号建）
Tools/Bin/CheckCrypto --strict         # 缺少 32 位工具链时按失败处理
Tools/Bin/CheckCrypto --root <项目根>   # 覆盖项目根（默认从可执行文件位置推断）
```

检查内容：

- **用例**：SHA-512 的 FIPS 180-4 向量、模式数据向量（覆盖块边界前后）、分块流式一致性、目标侧与宿主侧实现互验；Ed25519 的 RFC 8032 向量、拒绝路径（含 `y >= p` 的非规范公钥编码与 `x = 0` 且符号位置一）、宿主侧派生与签名、宿主签名目标验签的互验；信任锚表轮换窗口（旧钥 `KeyId 1` 与新钥 `KeyId 2` 并存）：`FindTrustAnchor` 按编号命中的公钥与固定种子派生结果逐字节一致、表外编号返回空指针、新钥签名只由 `KeyId 2` 的公钥验过而 `KeyId 1` 的公钥验不过（旧钥签名反之），证明路径是按编号选钥，而不是“表里任意一把能过就算过”
- **双编译**：同一批用例在 32 位下重新编译、链接并运行，`Packages/Baleen/Core/Src/TrustAnchor.cpp` 作为目标侧源码一并编入
- **目标侧链接**：按与 Baleen 各阶段一致的选项（`-m32 -ffreestanding -nostdlib`）链接目标侧源码，并核对产物不牵入 64 位除法辅助函数（`__udivdi3` 等）
- **信任锚生成**：在工作目录里跑 `Tools/Bin/MakeKeys --out <工作目录>/anchors/keys --key-id 2 --anchors <工作目录>/anchors/Retained.txt --anchor-cpp <工作目录>/anchors/TrustAnchors.cpp`，核对生成源的锚数量、两条 `KeyId` 都在、`KeyId 2` 的公钥与 `Development.pub` 逐字节一致、`KeyId 1` 的公钥与附加锚输入逐字节一致，同一输入两次生成的生成源字节完全相同；再按 BaleenCore 的目标侧选项单独编译生成源（不链接）；负例覆盖附加锚输入四条条目（`KeyId 2、3、4、6`，当前密钥取 `KeyId 5`）合计 5 条、超过 `TrustAnchor.hpp` 的 4 条容量（编译必须失败在容量 `static_assert` 上，以编译器输出里的 static assertion 文本为据）与附加锚输入出现与 `--key-id` 相同的编号（`MakeKeys` 必须非零退出、给出可读原因，且不留生成源）

目标侧源码取自 `Packages/Baleen/Common/Src` 与 `Packages/Baleen/Core/Src`（信任锚查找），宿主侧共享源码取自 `Tools/Common/Src`；检查的就是将要交付的那些实现。锚表由本工具的 `main.cpp` 自带两条（旧钥与新钥），因此不编译构建期生成的 `Out/TrustAnchors.cpp`，避免同名符号撞车。信任锚生成检查需要随源码交付的 `Tools/Bin/MakeKeys`，缺失时该组检查直接失败并给出提示。

退出码：0 通过、1 失败、2 缺少 32 位工具链（`--strict` 下为失败）。缺工具链时依赖编译器的那部分检查（双编译、目标侧链接、生成源的目标侧编译与容量负例）不执行，结论行写“用例通过，双编译、目标侧链接与信任锚生成编译检查未执行”，不写“全部通过”。
