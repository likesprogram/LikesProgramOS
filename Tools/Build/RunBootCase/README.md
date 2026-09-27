# RunBootCase

宿主侧 C++20 进程运行工具，等待串口或调试日志中的字面标记。看到标记立即停止模拟器；未见标记而自行退出、异常退出或超时均返回失败。构建只编译工具，不执行本地启动测试。

```sh
make -C Tools/Build/RunBootCase
Tools/Bin/RunBootCase --timeout 40 --marker PLACEHOLDER-STUB \
    --cwd /tmp/boot-case --console /tmp/boot-case/console.log \
    --log /tmp/boot-case/serial.log --log /tmp/boot-case/debug.log \
    -- qemu-system-x86_64 -display none -monitor none \
       -drive file=/absolute/system.hdd,format=raw,snapshot=on \
       -serial file:/tmp/boot-case/serial.log \
       -debugcon file:/tmp/boot-case/debug.log
```

`--cwd`、`--console`、至少一个 `--log`、正数 `--timeout` 和非空 `--marker` 必填；`--stdin 文件` 可供 Bochs 调试器读取 `continue`。`--` 之后的命令以独立参数执行，不经过 shell；需要 shell 功能时显式传入 `sh -c`。

控制台日志每次截断。其他日志仅从本次启动前的文件长度之后读取，避免旧标记假通过；观察到日志被截短时从头继续。仍建议为每例创建独立目录，避免多个进程改写同一日志。监测支持分批写入的标记。

工具创建独立进程组，成功、超时或收到 INT/TERM/HUP 后先发 TERM，最多等 2 秒再 KILL 并回收模拟器。进程正常退出但没有标记也视为失败；模拟器已异常退出时，之前输出标记不能覆盖错误。

退出码为成功 `0`、运行/启动/超时失败 `1`、信号中断 `128 + 信号号`。每例在 `--cwd/result.json` 记录状态、命令、退出码、耗时和是否因看到标记主动终止；失败时打印日志末尾。因成功而主动停止的模拟器可能显示负信号退出码，判断以 `status` 和 `stopped_after_marker` 为准。磁盘只读或 snapshot 选项由调用者提供。
