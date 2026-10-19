# FastPage

面向小端 ARM64 Linux 的独立 4 KiB 页无损压缩库。项目只提供一套固定实现：数值页采用 FOR/差分位打包，其他页采用页内 LZ 匹配，无法缩小时保存原文。

每页独立编码，支持 1～4096 字节；编码结果不依赖先前处理过的页。接口适用于用户态实验与性能评估，尚未集成操作系统内存压缩子系统。

## 快速开始

需要支持 **NEON 和 CRC32 指令**的小端 AArch64 CPU、Linux、GCC、GNU Make、Python 3.9 或更新版本。算法不依赖其他压缩库；构建和合成测试无需下载数据。

```bash
make
make example
make test
```

生成 `build/libfastpage.so` 和 `build/libfastpage.a`。编译目标为 `-march=armv8-a+crc`；不支持在 x86 主机上直接构建运行。

## 接口

```c
#include "codec.h"

unsigned char compressed[4352], restored[4096];
unsigned stored, encoded_size, mode;

/* input 指向实际的 n 字节输入；1 <= n <= 4096。 */
if (!codec_init()) return 1;  /* 开始使用前，串行调用一次。 */
stored = codec_encode(input, n, compressed, &encoded_size, &mode);
if (!stored || !codec_decode(compressed, stored, restored, n)) return 1;
```

调用方必须保存原长 `n` 和返回的 `stored`。输入、输出缓冲区不能重叠；编码输出至少4352字节，解码输出至少 `n` 字节。完整可运行示例见 [examples/roundtrip.c](examples/roundtrip.c)，具体返回值与约束见 [接口说明](docs/api.md)。

## 性能测试

测速仅调用 FastPage。默认使用 Silesia 的12个文件，逐文件切为独立4096字节页，保留文件末尾不足一页的部分。

```bash
# 已有语料可直接指定 CORPUS=/path/to/silesia，无需下载。
make fetch
make verify CORPUS=corpus OUTPUT=runs/verify-01
make bench CORPUS=corpus CPU=0 ROUNDS=5 SECONDS=1.5 OUTPUT=runs/bench-01
```

每次使用新的输出目录。工具保存全部速度样本、压缩比、构建命令、源码与二进制哈希及运行环境。结束标志是进程退出码0、`status.json` 中 `state` 为 `completed`；结果读 `REPORT.md` 或 `summary.json`。

Silesia 共211,938,580字节、51,746页；本实现的保存载荷为 **129,137,098字节**，压缩比 **1.641190512**，节省空间 **39.0686%**。压缩比按原长/保存载荷计算，包含原文回退页和页内元数据，不包含调用方页目录。吞吐取决于硬件和编译器，应在目标机器上重新测量。详细方法及本次整理验证见 [性能与验证](docs/benchmark.md)。

## 文档与阅读顺序

1. [算法运行过程](docs/algorithm.md)：页分类、数值压缩、LZ匹配和解码。
2. [接口与接入](docs/api.md)：容量、返回值、初始化与并发。
3. [二进制格式](docs/format.md)：C2、E5和原文页的精确定义。
4. [性能与验证](docs/benchmark.md)：测试方法、结果解释与复现命令。
5. 源码顺序：`include/codec.h` → `src/encoder.c` → `src/numeric_encode.inc` → `src/decoder.c` → `src/numeric_decode.inc`。

```text
include/codec.h          公开 C 接口
src/                    唯一的固定编解码实现
examples/roundtrip.c     最小接入示例
bench/benchmark.c       单算法 C 测量循环
tools/                  语料获取与性能测试入口
tests/                  独立向量、精确边界、往返与并发检查
docs/                   算法、接口、格式、性能文档
licenses/               必需的上游许可文本
```

## 来源与许可

项目采用 [BSD-2-Clause](LICENSE)，来源说明见 [NOTICE.md](NOTICE.md)。页内匹配实现含 LZ4 衍生代码，因此随附其版权及许可文本；项目没有包含可单独使用的 LZ4 库或其性能测试入口。
