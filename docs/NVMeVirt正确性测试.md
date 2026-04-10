# NVMeVirt 数据正确性测试说明

本文档针对以下典型加载方式与设备节点，说明如何用 **fio 校验** 等手段验证 NVMeVirt 读写与数据拷贝路径是否正确。

---

## 1. 测试环境约定

### 1.1 模块加载参数（多 dispatcher 示例）

```bash
sudo rmmod nvmev 2>/dev/null
sudo insmod ./nvmev.ko \
  memmap_start=64G \
  memmap_size=10G \
  nr_dispatchers=2 \
  cpus=7,8,9,10,11,12
```

说明：

- 前 2 个 CPU（7、8）为 dispatcher，其余（9–12）为 4 个 IO worker。
- `memmap_*` 需与机器实际保留内存一致；此处仅作示例。

### 1.2 目标设备（`nvme list` 示例）

加载成功后，在主机上应能看到类似条目（具体 `Node` 编号以本机为准）：

| 字段 | 示例值 |
|------|--------|
| Node | `/dev/nvme3n1` |
| SN | `CSL_Virt_SN_01` |
| Model | `CSL_Virt_MN_01` |
| Namespace | `1` |
| Usage | 与 `memmap_size` 减去元数据后的容量一致（例如约 10.74 GB） |
| Format | `512 B + 0 B` |
| FW Rev | `CSL_002` |

**注意**：`nvme list` 只列出 **namespace 块设备**；若只有 `/dev/nvme3` 而无 `nvme3n1`，需先排查 namespace 枚举与 `/dev` 节点（勿在 `/dev` 下手工创建同名普通文件）。

### 1.3 测试前检查

```bash
# 必须是块设备（b），不能是普通文件
ls -l /dev/nvme3n1

# 确认控制器与 namespace
sudo nvme list
sudo nvme id-ctrl /dev/nvme3 | grep '^nn'
```

`nn` 应为 `1`（表示声明 1 个 namespace）。

---

## 2. 为什么 fio 可以测正确性

默认 fio 只报吞吐与时延；开启 **`verify`** 后，写入时附带校验元数据，读回时比对，可发现：

- 数据写错、读错、错位；
- 多线程互相覆盖导致的“假失败”（需按下面方式配置，避免误判）。

---

## 3. 推荐测试用例

以下命令中 **`/dev/nvme3n1` 请按本机 `nvme list` 实际节点替换**。

### 3.1 Smoke：单 job 随机写 + 校验读（必做）

覆盖 1 GiB，深度 64，单进程无地址重叠问题。

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

**通过标准**：最后一行 job 报告 `err= 0`，无 `verify:` / `bad header` 类错误。

### 3.2 多 job 并行（分区不重叠）

**禁止**多个 job 同时对同一地址空间做 `randwrite + verify`（会互相覆盖校验头，产生假失败）。

使用 `offset_increment` 让每个 job 独占一段 LBA 区间（示例：4 job × 256 MiB = 1 GiB）：

```bash
sudo fio --name=verify4 \
  --filename=/dev/nvme3n1 \
  --ioengine=io_uring \
  --direct=1 \
  --bs=4k \
  --iodepth=64 \
  --numjobs=4 \
  --size=256M \
  --offset_increment=256M \
  --rw=randwrite \
  --verify=crc32c \
  --verify_fatal=1 \
  --do_verify=1 \
  --group_reporting=1
```

**通过标准**：各子 job 均为 `err= 0`；`Disk stats` 中对应设备有非零 `ios/sectors`。

若仍出现 `multiple writers may overwrite` 警告，只要 offset 区间严格不重叠，结果仍可采信。

### 3.3 顺序写 + 校验（可选，便于定位大块顺序路径）

```bash
sudo fio --name=seq-verify \
  --filename=/dev/nvme3n1 \
  --ioengine=io_uring \
  --direct=1 \
  --bs=128k \
  --iodepth=32 \
  --numjobs=1 \
  --rw=write \
  --size=2G \
  --verify=crc32c \
  --verify_fatal=1 \
  --do_verify=1
```

### 3.4 混合读写 + 校验（可选，压力更大）

```bash
sudo fio --name=mix-verify \
  --filename=/dev/nvme3n1 \
  --ioengine=io_uring \
  --direct=1 \
  --bs=4k \
  --iodepth=64 \
  --numjobs=1 \
  --rw=randrw \
  --rwmixread=50 \
  --size=2G \
  --runtime=300 \
  --time_based \
  --verify=crc32c \
  --verify_fatal=1 \
  --do_verify=1
```

### 3.5 Soak（长时间稳定性，可选）

在容量允许的前提下增大 `size` 或 `runtime`，例如 `--runtime=1800 --time_based`，仍使用 `numjobs=1` 或带 `offset_increment` 的多 job，观察数小时内是否出现 verify 错误。

---

## 4. 对比测试（多 dispatcher vs 单 dispatcher）

为验证多 dispatcher 改造未引入回归，建议固定同一组 fio 命令，仅改模块参数：

```bash
# A：单 dispatcher
sudo insmod ./nvmev.ko memmap_start=64G memmap_size=10G nr_dispatchers=1 cpus=7,8,9,10,11

# B：双 dispatcher（与本文 1.1 一致）
sudo insmod ./nvmev.ko memmap_start=64G memmap_size=10G nr_dispatchers=2 cpus=7,8,9,10,11,12
```

每次加载后确认 `nvme list` 中目标节点仍为预期设备（SN/Model 一致），再跑 **3.1** 与 **3.2**。  
A、B 均应 `err=0`；若仅 B 失败，再重点查并发与 admin 路径。

---

## 5. 常见问题

| 现象 | 可能原因 |
|------|----------|
| `verify: bad header` + 多 job 同文件随机写 | job 间覆盖 LBA，非设备 bug；改用单 job 或 `offset_increment` |
| `nvme list` 无盘但 `dmesg` 有控制器 | 仅有 `/dev/nvmeX`，无 namespace 块设备；检查 Identify/udev |
| `/dev/nvme3n1` 为普通文件 `-rw-r--r--` | 误创建文件占用了设备名；`sudo rm -f` 后重新触发 udev 或重载模块 |
| `nvme list-ns -a` 报 `INVALID_OPCODE` | 部分 Identify CNS 与 `nvme-cli` 版本组合下固件/模拟器未实现；**不以该命令作为唯一正确性依据**，以 fio verify 为主 |

---

## 6. 参考

- 多 dispatcher 架构与参数说明：[多Dispatcher设计与使用.md](./多Dispatcher设计与使用.md)
