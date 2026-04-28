# NVMevirt-multithread 使用说明

本文档说明当前 NVMeVirt 多线程版本的整体设计、编译方式和运行方法。示例命令中的
CPU、内存地址、设备节点和 PCI BDF 需要按实际机器调整。

## 整体设计

### NVMevirt 多线程支持

原始 NVMeVirt 主要由一个 dispatcher 线程轮询 BAR/doorbell，扫描所有 I/O SQ/CQ，
再把请求交给 io_worker。高 IOPS 场景下，单 dispatcher 对活跃队列的串行扫描和请求
分发会成为瓶颈。本实现将该路径扩展为多个 dispatcher 并行处理。

- **dispatcher 线程**
  - 由模块参数 `nr_dispatchers` 指定，默认值为 `1`，保持兼容原有行为。
  - `dispatcher 0` 负责 BAR 和 Admin Queue 处理。
  - 所有 dispatcher 都只轮询自己负责的 I/O SQ/CQ 分片。
  - 当前最大 dispatcher 数由 `NR_MAX_DISPATCHERS` 限制，代码中为 `8`。

- **io_worker 线程**
  - 由 `cpus` 参数中扣除 dispatcher CPU 后的剩余 CPU 数决定。
  - 每个 worker 固定归属于一个 dispatcher，负责实际数据搬运、按目标完成时间填充 CQ，
    并触发中断。
  - worker 按连续区间尽量均匀地分给各个 dispatcher。

- **CPU 参数语义**
  - `cpus` 的前 `nr_dispatchers` 个 CPU 用作 dispatcher。
  - `cpus` 的剩余 CPU 用作 io_worker。
  - 因此需要预留的空闲 CPU 数量为：

    ```text
    nr_dispatchers + nr_io_workers
    ```

  - 例如 `nr_dispatchers=5` 且希望使用 `10` 个 io_worker，则 `cpus` 中至少需要提供
    `15` 个 CPU。

- **SQ/CQ 分片规则**
  - SQ 到 dispatcher：

    ```text
    dispatcher_id = (sqid - 1) % nr_dispatchers
    ```

  - CQ 到 dispatcher：

    ```text
    dispatcher_id = (cqid - 1) % nr_dispatchers
    ```

  - dispatcher 内部的 worker 选择默认按 SQ/CQ 的 qid 映射到该 dispatcher 的 worker
    子集，使同一队列的请求和中断处理路径更加稳定。

- **并发安全**
  - 多 dispatcher 会并发进入 FTL 调度路径。
  - NVM/simple FTL 中的 `io_unit_stat[]` 使用 `cmpxchg64` 和 `READ_ONCE` 原子推进
    busy-until 时间，避免多个 dispatcher 同时更新时丢失状态。
  - Admin Queue 创建/删除 I/O SQ/CQ 时，会把 qid 注册到对应 dispatcher 和 worker 的
    活跃队列表中；dispatcher 循环只扫描自己的 qid 列表。

### Reclaim 优化

io_worker 的 work queue 是固定大小队列，完成的请求需要回收到 free list 后才能继续
接收新 I/O。当前实现做了以下优化：

- **dispatcher 侧批量 reclaim**
  - 完成请求的回收由 dispatcher 触发。
  - 每分发一批请求后再回收，批大小由 `NVMEV_RECLAIM_BATCH_SIZE` 控制，当前为 `16`。
  - 这样避免每个请求都付出 reclaim 成本，同时能及时释放 worker 队列中的已完成 entry。

- **队列满时快速 reclaim**
  - 如果目标 worker 队列已满，dispatcher 会立即尝试 reclaim 自己名下 worker 的已完成
    entry。
  - 如果 reclaim 后仍无空闲 entry，则本轮分发停止，等待后续循环继续处理，避免无限制占用 CPU。

- **CQ 中断路径优化**
  - worker 在填 CQ 时只把需要触发中断的 CQ 放入 pending CQ 链表。
  - worker 主循环优先处理 pending CQ，避免在 CQ 数量较多时反复扫描全部 CQ。

- **调试观测**
  - 加载模块时设置 `debug=1` 后，可通过 `/proc/nvmev/debug` 查看 dispatcher/worker 的
    reclaim 次数、平均耗时、队列满事件、SQ/CQ 映射等信息。

## 编译说明

### 1. 环境要求

- Linux kernel 建议 `>= 5.15`，并安装与当前内核匹配的 headers：

  ```bash
  ls /lib/modules/$(uname -r)/build
  ```

- 需要 root 权限加载/卸载内核模块。
- 需要在启动参数中为 NVMeVirt 预留物理内存，例如预留从 64G 开始的 32G：

  ```bash
  GRUB_CMDLINE_LINUX="memmap=32G\\\$64G"
  sudo update-grub
  sudo reboot
  ```

- 建议通过 `isolcpus` 预留 NVMeVirt 使用的 CPU，避免 Linux 调度器把普通任务放到这些
  CPU 上。例如 5 个 dispatcher + 10 个 io_worker：

  ```bash
  GRUB_CMDLINE_LINUX="memmap=32G\\\$64G isolcpus=7-21"
  ```

- CPU 数量要求：
  - 至少提供 `nr_dispatchers` 个 dispatcher CPU。
  - 为了真正执行 I/O，还需要提供至少 1 个 io_worker CPU。
  - 推荐按测试目标显式留出 `nr_dispatchers + nr_io_workers` 个空闲 CPU。

### 2. 选择设备模型

编译前在 `Kbuild` 中选择一个目标设备模型。默认配置为 NVM/Optane 模型：

```makefile
CONFIG_NVMEVIRT_NVM := y
#CONFIG_NVMEVIRT_SSD := y
#CONFIG_NVMEVIRT_ZNS := y
#CONFIG_NVMEVIRT_KV := y
```

一次只启用一个模型。SSD/ZNS/KV 模型的参数可继续参考对应配置文件和原有文档。

### 3. 编译内核模块

在仓库根目录执行：

```bash
make clean
make
```

成功后会生成：

```text
nvmev.ko
```

如果编译失败，优先检查：

- `/lib/modules/$(uname -r)/build` 是否存在；
- 当前 gcc/clang 与内核 headers 是否匹配；
- `Kbuild` 是否只启用了一个目标设备模型。

## 使用说明

### 1. 启动 NVMevirt 多线程

以下以 **5 个 dispatcher + 10 个 io_worker** 为例：

```bash
sudo rmmod nvmev 2>/dev/null || true
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=5 \
  nr_max_parallel_io=65536 \
  cpus=7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
```

CPU 分配如下：

| 线程类型 | CPU |
| --- | --- |
| dispatcher 0-4 | 7,8,9,10,11 |
| io_worker 0-9 | 12,13,14,15,16,17,18,19,20,21 |

加载后检查内核日志：

```bash
sudo dmesg | tail -n 80
```

应能看到类似信息：

```text
NVMeVirt: Configured 5 dispatcher(s), 10 IO worker(s), work queue depth 65536
NVMeVirt: nvmev_dispatcher_0 started on cpu 7 ...
NVMeVirt: nvmev_dispatcher_1 started on cpu 8 ...
NVMeVirt: nvmev_io_worker_0 started on cpu 12 ...
NVMeVirt: Virtual NVMe device created
```

确认块设备：

```bash
sudo nvme list
ls -l /dev/nvme*n1
```

如果需要查看 dispatcher/worker 映射与统计，可开启 debug：

```bash
sudo rmmod nvmev 2>/dev/null || true
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=5 \
  cpus=7,8,9,10,11,12,13,14,15,16,17,18,19,20,21 \
  debug=1

cat /proc/nvmev/debug
echo reset | sudo tee /proc/nvmev/debug
```

### 2. 启动 SPDK

如果用 SPDK 绕过内核块层测试 NVMeVirt，需要先找到 NVMeVirt 暴露出的 PCI BDF，再把该
设备绑定给 SPDK 使用的 userspace driver。

1. 查找 NVMeVirt 设备 BDF：

   ```bash
   lspci -nn | grep -i -E '0c51|nvme'
   ```

   内核日志中也会打印类似 `nvme nvme0: pci function 0001:10:00.0` 的 BDF。

2. 准备 hugepage 并绑定设备。以下命令以 `0001:10:00.0` 为例，实际请替换为本机 BDF：

   ```bash
   cd /path/to/spdk
   sudo HUGEMEM=4096 PCI_ALLOWED="0001:10:00.0" ./scripts/setup.sh
   ```

3. 使用 SPDK `perf` 进行 4K 随机读测试：

   ```bash
   sudo ./build/examples/perf \
     -q 64 \
     -o 4096 \
     -w randread \
     -t 60 \
     -c 0x3c0000 \
     -r 'trtype:PCIe traddr:0001:10:00.0'
   ```

   其中：

   - `-q 64`：每个 qpair 的队列深度；
   - `-o 4096`：4 KiB I/O；
   - `-w randread`：随机读；
   - `-c`：SPDK reactor 使用的 CPU mask，建议不要与 NVMeVirt 的 dispatcher/worker CPU
     重叠；
   - `traddr`：NVMeVirt 设备的 PCI BDF。

如果希望改回 Linux 内核驱动测试，需要先让 SPDK 解绑并重新绑定内核驱动，或重启后重新
加载 NVMeVirt。

### 3. Linux fio 基准测试

内核块设备路径可直接使用 fio。以下假设设备为 `/dev/nvme3n1`，请按 `nvme list` 输出替换：

```bash
sudo fio --name=nvmevirt-randread \
  --filename=/dev/nvme3n1 \
  --ioengine=io_uring \
  --direct=1 \
  --rw=randread \
  --bs=4k \
  --iodepth=64 \
  --numjobs=16 \
  --runtime=60 \
  --time_based \
  --group_reporting=1
```

正确性测试可使用 fio verify：

```bash
sudo fio --name=verify1 \
  --filename=/dev/nvme3n1 \
  --ioengine=io_uring \
  --direct=1 \
  --bs=4k \
  --iodepth=64 \
  --numjobs=1 \
  --rw=randwrite \
  --size=1G \
  --verify=crc32c \
  --verify_fatal=1 \
  --do_verify=1 \
  --group_reporting=1
```

多 job 做 verify 时要用 `offset_increment` 避免多个 job 写同一 LBA 区间导致误报。

### 4. 测试结果与观测方法

建议固定 workload，对比以下配置：

```bash
# 单 dispatcher 基线
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=1 \
  cpus=7,8,9,10,11

# 多 dispatcher
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=5 \
  cpus=7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
```

重点观察：

- fio/SPDK 输出中的 IOPS、平均时延、p99/p99.9 tail latency；
- `/proc/nvmev/stat` 中每个 SQ 的 in-flight、dispatch、dispatched、total_io；
- `/proc/nvmev/debug` 中：
  - 每个 dispatcher 的 `sq_entries`、`avg_proc_io_ns`、`avg_enqueue_ns`；
  - reclaim 调用次数和平均耗时；
  - worker 队列满事件 `full_events`；
  - `sq_map` / `cq_map` 是否均匀分布到多个 dispatcher 和 worker。

已有实验结论表明：在高性能设备模型下，单 dispatcher 对活跃 SQ 的串行处理会限制整体
IOPS；增加 io_worker 数量到一定程度后收益变小，而多 dispatcher 能把 SQ/CQ 分发路径
并行化，是继续提升高 IOPS 场景上限的关键。

## 常见问题

| 现象 | 可能原因与处理 |
| --- | --- |
| `insmod` 报 reserved memory 相关错误 | `memmap_start/memmap_size` 与 grub 中 `memmap=` 不匹配，或该地址仍是可用 RAM |
| 看不到 `/dev/nvmeXn1` | 检查 `dmesg`、`nvme list`，确认 namespace 是否枚举成功 |
| dispatcher/worker 没有按预期 CPU 启动 | 检查 `nr_dispatchers` 和 `cpus` 顺序，前 N 个 CPU 才是 dispatcher |
| 多 job fio verify 报 `bad header` | 多个 job 写入区域重叠，使用单 job 或设置 `offset_increment` |
| SPDK 找不到设备 | 确认 BDF 正确、设备已绑定到 SPDK 使用的 driver，且 IOMMU/vfio 配置满足 SPDK 要求 |
| 高 IOPS 下 worker 队列满 | 增大 `nr_max_parallel_io`，增加 worker，或通过 `/proc/nvmev/debug` 查看是否 reclaim 成本过高 |

## 参考文档

- [多 Dispatcher 设计与使用说明](./多Dispatcher设计与使用.md)
- [NVMeVirt 正确性测试说明](./NVMeVirt正确性测试.md)
- [实验结果](./实验结果.md)
