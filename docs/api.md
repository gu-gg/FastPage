# 接口与接入

## 平台与构建

仅支持小端 AArch64 Linux，CPU须有NEON与CRC32指令。GCC构建时使用 `-march=armv8-a+crc`。本库没有运行时CPU指令降级选择；调用者需在支持的机器上运行。

```bash
make
gcc -O3 -Iinclude examples/roundtrip.c -Lbuild -lfastpage \
    -Wl,-rpath,'$ORIGIN' -o build/example
./build/example
```

静态链接可将 `build/libfastpage.a` 作为链接输入；默认构建使用GCC LTO，建议使用同一套GCC工具链完成链接。共享库不需要其他压缩库。

## 三个公开函数

```c
int codec_init(void);
unsigned codec_encode(const unsigned char *src, unsigned n,
                      unsigned char *dst, unsigned *encoded_size,
                      unsigned *mode);
int codec_decode(const unsigned char *src, unsigned stored,
                 unsigned char *dst, unsigned n);
```

| 函数 | 成功 | 失败 |
|---|---|---|
| `codec_init` | 返回1 | 返回0 |
| `codec_encode` | 返回保存长度，范围1～n | 无效页长度返回0 |
| `codec_decode` | 返回1，并还原n字节 | 无效尺寸或不合法编码返回0 |

编码时 `encoded_size` 和 `mode` 是可选统计输出，可传 `NULL`；非空时必须指向可写的 `unsigned`。编码成功时，`encoded_size` 等于返回的保存长度，`mode` 为 `0xFF`（原文）、`0xC2`（数值页）或 `0xE5`（LZ页）。无效页长返回0时不更新这两个输出。解码不需要这些统计输出。

## 缓冲区与长度

- 一次输入一页：`1 <= n <= 4096`。
- 编码输入精确包含n字节，编码输出容量至少 **4352字节**。此容量允许内部宽写，最终只保存返回的前 `stored` 字节。
- 解码输入精确包含 `stored` 字节，输出容量至少n字节，不要求额外读写余量。
- 输入和输出不得重叠。指针必须有效，库不提供空指针或调用者实际容量的自动检测。
- 调用方另存 `n` 和 `stored`；页内数据不重复保存原长。`stored==n` 时整页是原文，首字节没有格式含义。
- 解码失败时输出可能已被部分写入，不得使用。

## 初始化和并发

使用前调用一次 `codec_init()`。当前查表数据均为只读常量，该函数直接返回成功，不分配内存。独立缓冲区上的编码和解码可以并发。编码器的搜索表属于本次调用，不跨页复用，不会因调用顺序改变输出。

## 无损与数据完整性

合法输入编码后能够逐字节还原。解码器检查长度、边界和匹配距离，但格式没有校验和；受损数据也可能变成另一条合法编码，成功返回不代表内容未被篡改或损坏。需要数据完整性保证时由上层另存校验信息。

## 系统集成范围

当前是用户态C库：没有页目录、内存池、内核接口或页面换入换出管理。基准测试中的5120字节压缩槽是测量缓冲布局，不是实际存储分配方案，也不计入保存载荷。内核集成还需评估栈空间、SIMD使用约束、调度和页元数据开销。
