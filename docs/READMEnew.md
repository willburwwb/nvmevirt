# NVMevirt-multithread 使用说明

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

### Reclaim 优化

io_worker 的 work queue 是固定大小队列，完成的请求需要回收到 free list 后才能继续
接收新 I/O。当前实现做了以下优化：

- **dispatcher 侧批量 reclaim**
  - 完成请求的回收由 dispatcher 触发。
  - 每分发一批请求后再回收，批大小由 `NVMEV_RECLAIM_BATCH_SIZE` 控制，当前为 `16`。
  - 这样避免每个请求都付出 reclaim 成本，同时能及时释放 worker 队列中的已完成 entry。

- **调试观测**
  - 加载模块时设置 `debug=1` 后，可通过 `/proc/nvmev/debug` 查看 dispatcher/worker 的
    reclaim 次数、平均耗时等信息。

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

- 通过 `isolcpus` 预留 NVMeVirt 使用的 CPU，避免 Linux 调度器把普通任务放到这些
  CPU 上。例如 5 个 dispatcher + 10 个 io_worker：

  ```bash
  GRUB_CMDLINE_LINUX="memmap=32G\\\$64G isolcpus=7-21"
  ```

### 2. 编译内核模块

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
dmesg | tail -n 80
```

应能看到类似信息：

```text
NVMeVirt: Configured 5 dispatcher(s), 10 IO worker(s), work queue depth 65536
NVMeVirt: nvmev_dispatcher_0 started on cpu 7 ...
NVMeVirt: nvmev_dispatcher_1 started on cpu 8 ...
NVMeVirt: nvmev_io_worker_0 started on cpu 12 ...
NVMeVirt: Virtual NVMe device created
```

### 2. 启动 SPDK

如果希望使用 **SPDK + fio** 绕过 Linux 内核块层测试 NVMeVirt，需要先编译 SPDK 的 fio
插件，再把 NVMeVirt 暴露出的 PCI 设备绑定到 SPDK 使用的 userspace driver。

1. 编译 SPDK fio 插件：

   ```bash
   cd /path/to/spdk
   ./configure --with-fio=/path/to/fio-3.28-src
   make -j"$(nproc)"
   ```

   编译成功后应能看到：

   ```text
   /path/to/spdk/build/fio/spdk_bdev
   ```

2. 查找 NVMeVirt 设备 BDF：

   ```bash
   lspci -nn | grep -i -E '0c51|nvme'
   ```

   内核日志中也会打印类似 `nvme nvme0: pci function 0001:10:00.0` 的 BDF。

3. 准备 hugepage 并绑定设备。以下命令以 `0001:10:00.0` 为例，实际请替换为本机 BDF：

   ```bash
   cd /path/to/spdk
   sudo PCI_ALLOWED="0001:10:00.0" HUGEMEM=1024 ./scripts/setup.sh
   ```

   可以先检查当前绑定状态：

   ```bash
   cd /path/to/spdk
   sudo PCI_ALLOWED="0001:10:00.0" ./scripts/setup.sh status
   ```

   成功后该设备通常会从内核 `nvme` 驱动切换到 `vfio-pci` 或 `uio_pci_generic`。

5. 准备 SPDK bdev 配置文件，例如 `/tmp/nvmevirt_bdev.json`：

   ```json
   {
     "subsystems": [
       {
         "subsystem": "bdev",
         "config": [
           {
             "method": "bdev_nvme_attach_controller",
             "params": {
               "name": "Nvme0",
               "trtype": "PCIe",
               "traddr": "0001:10:00.0",
               "io_queue_size": 8192
             }
           }
         ]
       }
     ]
   }
   ```

   其中：

   - `traddr` 需要替换成当前 NVMeVirt 的 PCI BDF；
   - `name` 这里使用 `Nvme0`，后续 fio 里会用到 `Nvme0n1`；
   - `io_queue_size` 可以按测试需要调整。



### 3. fio 基准测试

注意：使用 **SPDK + fio** 时，`fio` 里不能再直接写 `/dev/nvmeXn1`。设备一旦被 SPDK
接管，测试目标应写成 SPDK bdev 名，例如 `Nvme0n1`。

1. 准备 fio job，例如 `/tmp/nvmevirt_randread.fio`：

   ```ini
   [global]
   ioengine=/home/wwb/StorageXpress/spdk/build/fio/spdk_bdev
   spdk_json_conf=/tmp/nvmevirt_bdev.json
   thread=1
   direct=1
   group_reporting=1
   time_based=1
   runtime=30
   ramp_time=5
   norandommap=1
   bs=4k
   rw=randread
   iodepth=64
   numjobs=16

   [nvmevirt]
   filename=Nvme0n1
   ```

   关键参数说明：

   - `thread=1` 是必须的，SPDK fio 插件不支持 fio 默认的进程模型；
   - `ioengine` 需要指向 `spdk_bdev` 插件的实际路径；
   - `spdk_json_conf` 指向上一步生成的 JSON 文件；
   - `filename=Nvme0n1` 中的 `Nvme0` 来自 JSON 里的控制器名，`n1` 表示 namespace 1。

2. 运行测试：

```bash
sudo fio /tmp/nvmevirt_randread.fio
```

### 4. 测试结果

```
(base) ➜  spdk git:(master) ✗ sudo fio /home/wwb/StorageXpress/nvme6_randread.fio            
nvme6: (g=0): rw=randread, bs=(R) 1024B-1024B, (W) 1024B-1024B, (T) 1024B-1024B, ioengine=spdk_bdev, iodepth=64
...
fio-3.28
Starting 16 threads
Jobs: 5 (f=5): [_(1),r(2),_(6),r(1),_(4),r(2)][29.8%][r=8158MiB/s][r=8354k IOPS][eta 01m:25s]
nvme6: (groupid=0, jobs=16): err= 0: pid=124707: Tue Apr 28 10:06:52 2026
  read: IOPS=8537k, BW=8337MiB/s (8742MB/s)(244GiB/30001msec)
    slat (nsec): min=90, max=1761.1k, avg=159.09, stdev=891.25
    clat (usec): min=15, max=1995, avg=119.21, stdev=58.68
     lat (usec): min=16, max=1996, avg=119.37, stdev=58.67
    clat percentiles (usec):
     |  1.00th=[   37],  5.00th=[   38], 10.00th=[   39], 20.00th=[   81],
     | 30.00th=[   87], 40.00th=[  104], 50.00th=[  128], 60.00th=[  137],
     | 70.00th=[  145], 80.00th=[  151], 90.00th=[  217], 95.00th=[  221],
     | 99.00th=[  227], 99.50th=[  229], 99.90th=[  231], 99.95th=[  233],
     | 99.99th=[ 1778]
   bw (  MiB/s): min= 8163, max= 8671, per=99.80%, avg=8320.57, stdev= 9.17, samples=945
   iops        : min=8359243, max=8879177, avg=8520262.94, stdev=9390.62, samples=945
  lat (usec)   : 20=0.01%, 50=19.11%, 100=20.20%, 250=60.67%, 500=0.01%
  lat (msec)   : 2=0.01%
  cpu          : usr=99.80%, sys=0.00%, ctx=777, majf=0, minf=0
  IO depths    : 1=0.1%, 2=0.1%, 4=0.1%, 8=0.1%, 16=0.1%, 32=23.1%, >=64=76.9%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=99.8%, 8=0.2%, 16=0.1%, 32=0.1%, 64=0.1%, >=64=0.0%
     issued rwts: total=256129457,0,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=64

Run status group 0 (all jobs):
   READ: bw=8337MiB/s (8742MB/s), 8337MiB/s-8337MiB/s (8742MB/s-8742MB/s), io=244GiB (262GB), run=30001-30001msec
```
可以看到此时 IOPS 达到 8.537M，带宽达到 8.337GB/s。