# 来源与许可说明

FastPage 是用于独立内存页压缩的研究实现，使用页内LZ匹配、FOR、差分、ZigZag及位打包等已有方法。

`src/encoder.c` 和 `src/decoder.c` 的直接匹配与复制逻辑含基于 LZ4 1.9.4 改写的部分。其版权与 BSD-2-Clause 许可全文保留于 [licenses/LZ4_LICENSE.txt](licenses/LZ4_LICENSE.txt)。此声明不表示项目提供LZ4兼容格式或包含独立LZ4库。

Silesia测试数据不随仓库分发。可选下载工具使用Silesia Corpus提供方的地址和校验值；语料数据的权利属于各自权利人，项目代码许可不改变数据许可。

项目整体按 [BSD-2-Clause](LICENSE) 发布。项目新增与修改部分的声明为 `Copyright (c) 2026, FastPage contributors`；上游部分的原版权及许可声明一并保留。
