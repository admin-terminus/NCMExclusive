# THIRD PARTY NOTICES

## AMExclusive

本项目（NCMExclusive）在设计上参考了 AMExclusive 的实现思路，其源码片段（独占模式
初始化重试、`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 处理、float32 -> int32 左对齐打包）
为独立重新实现。

- Project: https://github.com/AntiOblivionis/AMExclusive
- License: MIT
- Copyright (c) AMExclusive contributors

MIT License 全文见本项目 LICENSE 文件。

## MinHook / rigtorp/SPSCQueue

AMExclusive 使用了上述库；NCMExclusive 不直接依赖它们（IAT 钩子等均为本项目自行实现），
特此说明并致谢。
