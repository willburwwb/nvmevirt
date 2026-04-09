# NVMeVirt 多 Dispatcher 设计与使用说明

## 1. 背景与目标

在原始实现中，NVMeVirt 使用单个 `dispatcher` 线程轮询 doorbell、扫描所有 SQ/CQ 并分发请求。  
当目标 IOPS 提升到较高规模时，单线程分发路径会成为瓶颈，导致吞吐上限受限。

本次改造目标：

- 将单 `dispatcher` 改为多 `dispatcher` 并行分发；
- 每个 `dispatcher` 固定绑定一组 `worker`；
- 保持默认配置向后兼容（`nr_dispatchers=1` 时行为等价旧版本）。

---

## 2. 架构设计

### 2.1 线程模型

- `dispatcher` 线程：`nr_dispatchers` 个
  - 负责轮询自己分片的 SQ/CQ；
  - `dispatcher 0` 额外负责 BAR/Admin queue 处理。
- `io_worker` 线程：由 `cpus` 参数中剩余 CPU 数量决定
  - 负责数据搬运（memcpy/DMA）与 CQ 填充、触发中断；
  - 每个 worker 固定归属于一个 dispatcher。

### 2.2 SQ/CQ 分片规则

- SQ 到 dispatcher：
  - `dispatcher_id = (sqid - 1) % nr_dispatchers`
- CQ 到 dispatcher：
  - `dispatcher_id = (cqid - 1) % nr_dispatchers`

这样每个 dispatcher 只处理自己的队列分片，避免所有 SQ 串行经过同一线程。

### 2.3 Worker 归属与选择

- worker 按连续区间分配给 dispatcher（尽量均分）；
- 对于某个 SQ，worker 选择在该 dispatcher 的 worker 子集内进行。

---

## 3. 关键并发点处理

### 3.1 `io_unit_stat` 并发更新

多 dispatcher 会并发调用 FTL 调度逻辑，`io_unit_stat[]` 需要并发安全。  
当前在 `simple_ftl.c` 中已改为 CAS 原子更新（`cmpxchg64` + `READ_ONCE`），保证：

- 更新不丢失；
- 时间模型语义保持一致（返回 completion time，统计 busy-until）。

### 3.2 初始化顺序

为避免 worker 启动后访问未初始化的 dispatcher 信息，初始化顺序为：

1. `NVMEV_DISPATCHER_INIT`
2. `NVMEV_IO_WORKER_INIT`

并且 worker 的 `dispatcher_id/cpu_nr_dispatcher` 在 `wake_up_process()` 前完成赋值。

---

## 4. 参数与配置方式

### 4.1 新增参数

- `nr_dispatchers`：dispatcher 线程数量（默认 `1`）

### 4.2 `cpus` 参数语义

`cpus` 现在解释为：

- 前 `nr_dispatchers` 个 CPU：dispatcher 使用；
- 剩余 CPU：io_worker 使用。

例如：

- `nr_dispatchers=2`
- `cpus=7,8,9,10,11,12`

则：

- Dispatcher0 -> CPU7
- Dispatcher1 -> CPU8
- Worker -> CPU9,10,11,12（再按分组分给两个 dispatcher）

---

## 5. 使用示例

## 5.1 编译

```bash
make
```

## 5.2 加载模块（2 个 dispatcher）

```bash
sudo rmmod nvmev
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=2 \
  cpus=7,8,9,10,11,12
```

## 5.3 兼容旧行为（单 dispatcher）

```bash
sudo rmmod nvmev
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=32G \
  nr_dispatchers=1 \
  cpus=7,8,9,10,11
```

---

## 6. 如何验证改造生效

## 6.1 看内核日志线程启动信息

可通过 `dmesg` 确认：

- `nvmev_dispatcher_0`, `nvmev_dispatcher_1` 等线程均已启动；
- 每个 dispatcher 的 CPU 与 worker 区间符合预期。

## 6.2 对比性能

建议固定同样 workload，比较：

- `nr_dispatchers=1`
- `nr_dispatchers=2`（或更多）

重点看：

- fio IOPS
- fio tail latency（p99/p99.9）
- `/proc/nvmev/stat` 中分发与 in-flight 统计

---

## 7. 当前已知限制

- worker 侧 IRQ 仍存在按 CQ 轮询路径，CQ 数量很大时可能带来额外开销；
- 请求有序插入队列仍是 O(n)；
- 回收路径仍是轮询式。

这些不会影响功能正确性，但在超高 IOPS/高队列规模下可能继续影响尾延迟。

---

## 8. 涉及文件

- `nvmev.h`
- `main.c`
- `io.c`
- `simple_ftl.c`
- `pci.c`
- `channel_model.c`
- `kv_ftl.c`
