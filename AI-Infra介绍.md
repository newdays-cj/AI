# AI Infra 详细介绍

AI Infra（AI Infrastructure，人工智能基础设施）是支撑 AI 系统全生命周期运行的技术栈总称。它不是某一个单一工具，而是一整套覆盖**算力、数据、训练、推理、部署、监控**的工程体系。

---

## 一、核心分层

```
┌──────────────────────────────────────┐
│          应用层 (Applications)         │  Chatbot、搜索、推荐、自动驾驶...
├──────────────────────────────────────┤
│          平台层 (Platform)             │  MLOps、模型服务、特征平台、实验管理
├──────────────────────────────────────┤
│          框架层 (Frameworks)           │  PyTorch, JAX, TensorFlow, vLLM, TRT-LLM
├──────────────────────────────────────┤
│          调度层 (Orchestration)        │  Kubernetes, Slurm, Volcano
├──────────────────────────────────────┤
│          算力层 (Compute)              │  GPU/TPU/NPU, InfiniBand/RoCE, NVLink
├──────────────────────────────────────┤
│          存储层 (Storage)              │  分布式文件系统、对象存储、向量数据库
└──────────────────────────────────────┘
```

---

## 二、关键组成部分

### 1. 算力基础设施（Compute）

AI 的底层燃料是算力，核心硬件包括：

| 硬件 | 典型产品 | 用途 |
|------|----------|------|
| GPU | NVIDIA H100/B200, AMD MI300X | 训练 & 推理主力 |
| TPU | Google TPU v5p | Google Cloud 专用 |
| NPU | 昇腾 910B, 寒武纪 | 国产替代方案 |
| 互联 | InfiniBand NDR (400Gb/s), NVLink, NVSwitch | GPU 间高速通信 |

关键指标：**FLOPS**（算力）、**显存带宽**（HBM）、**互联带宽**（卡间/节点间）。

### 2. 分布式训练（Distributed Training）

单卡装不下大模型，必须分布式训练。核心范式：

- **数据并行（Data Parallelism, DP）**：每张卡持有一份完整模型副本，数据切分——FSDP / DeepSpeed ZeRO
- **模型并行（Model Parallelism）**：
  - **张量并行（Tensor Parallelism, TP）**：单层内的矩阵运算切分到多卡
  - **流水线并行（Pipeline Parallelism, PP）**：按层切分，不同卡负责不同层
- **3D 并行 = DP + TP + PP**：GPT-4 / Llama 405B 级别模型的标配
- **混合精度训练**：FP16/BF16 + FP32 master weights，配合 loss scaling
- **Checkpointing（激活重计算）**：用时间换显存

主流框架：

- **DeepSpeed**（Microsoft）：ZeRO 优化器，从 Stage-1 到 Stage-3
- **Megatron-LM**（NVIDIA）：TP + PP 的标准实现
- **FSDP**（PyTorch 原生）：数据并行的官方方案
- **torchrun / torch.distributed**：底层通信后端（NCCL/Gloo）

### 3. 高性能网络（HPC Networking）

GPU 之间通信是瓶颈。关键技术：

- **NCCL**（NVIDIA Collective Communications Library）：GPU 集合通信标准库
- **AllReduce / AllGather / ReduceScatter**：分布式通信原语
- **InfiniBand / RoCE v2**：RDMA（远程直接内存访问），绕过 CPU 实现 GPU 间直接传数据
- **GPUDirect RDMA**：GPU 显存直接通过网卡传输，零拷贝
- **SHARP**（NVIDIA Mellanox）：在交换机上做 Reduce，减轻 GPU 负担

### 4. 模型推理（Inference）

训练完之后，服务用户请求。核心挑战：**延迟**和**吞吐量**。

关键技术：

| 技术 | 说明 |
|------|------|
| KV Cache | 缓存已计算的 Key/Value，避免重复计算 |
| Continuous Batching | 动态组 batch，不等所有请求完成 |
| FlashAttention | 分块计算 attention，减少 HBM 读写 |
| 量化（INT8/INT4/FP8） | 压缩模型，减少显存和计算量 |
| Speculative Decoding | 小模型"猜"下一步，大模型验证 |
| Prefix Caching | 共享 system prompt 的 KV Cache 复用 |

主流推理引擎：

| 引擎 | 特点 |
|------|------|
| **vLLM** | PagedAttention，连续 batching，开源最强 |
| **TensorRT-LLM** | NVIDIA 官方，极致优化但上手难 |
| **SGLang** | RadixAttention，结构化生成 |
| **llama.cpp** | CPU/边缘设备推理 |
| **TGI** (HuggingFace) | 生态好，易部署 |

### 5. 存储系统（Storage）

AI 场景对存储有特殊要求：

- **训练数据存储**：高吞吐顺序读取（PB 级别），通常用 **Lustre / GPFS / JuiceFS / CFS**
- **Checkpoint 存储**：频繁写入大文件（数 TB/次），需要高写入带宽
- **模型仓库**：S3 / MinIO / HDFS 存储模型权重和配置
- **向量数据库**：Milvus / Pinecone / Weaviate——用于 RAG 和语义搜索
- **特征存储**：Feast / Tecton——离线训练和在线推理的特征一致性

### 6. 调度系统（Orchestration）

大规模 GPU 集群的作业管理：

- **Kubernetes + Volcano / Kueue**：GPU 感知调度、队列管理、gang scheduling
- **Slurm**：传统 HPC 调度器，仍在很多 AI 集群使用
- **弹性训练**：节点故障自动恢复，动态扩缩容（torch elastic）

### 7. MLOps / AI 平台

支撑 AI 研发流程的工程平台：

```
数据采集 → 数据清洗 → 特征工程 → 模型训练 → 模型评估 → 模型部署 → 在线监控
   ↑                                                                    │
   └──────────────────── 反馈闭环 (Retraining) ─────────────────────────┘
```

关键组件：

- **实验追踪**：MLflow / W&B（Weights & Biases）/ Aim
- **流水线编排**：Kubeflow Pipelines / Airflow / Argo
- **模型注册 & 版本管理**：MLflow Model Registry / DVC
- **CI/CD for ML**：自动化测试、部署、回滚
- **可观测性**：Prometheus + Grafana（GPU 利用率、显存、温度、训练 loss 曲线）

---

## 三、典型 AI Infra 团队的工作

一个 AI Infra 工程师日常要处理的问题：

1. **"训练速度优化"**——计算 MFU（Model FLOPs Utilization），排查通信瓶颈、数据加载瓶颈
2. **"多机多卡训练不通"**——NCCL 报错、IB 链路故障、PCIe 拓扑不对
3. **"OOM（Out of Memory）"**——调整 batch size / ZeRO stage / gradient checkpointing / offload 策略
4. **"推理延迟 P99 太高"**——优化 KV Cache、调整 batching 策略、量化
5. **"GPU 集群利用率只有 30%"**——调度策略优化、资源碎片整理
6. **"Checkpoint 写得太慢"**——存储路径优化、异步 checkpointing
7. **"模型编译"**——用 `torch.compile` / XLA / TensorRT 将 eager 模式的模型图优化成高效计算图

---

## 四、2025-2026 年的热点方向

- **MoE（Mixture of Experts）**——DeepSeek-V3、Mixtral，稀疏激活降低推理成本
- **长上下文**——百万 token 窗口的 KV Cache 管理（RingAttention、层次化 attention）
- **多模态**——文本、图像、视频、音频统一架构的 Infra 挑战
- **AI Agent**——tool calling、代码执行沙箱、长期记忆管理的 Infra 需求
- **异构计算**——GPU + NPU + FPGA 混合调度
- **绿色 AI**——液冷散热、动态功耗管理、碳排放追踪

---

## 总结

**AI Infra 就是让 AI 模型从"实验室能跑"变成"生产环境可大规模、稳定、高效运行"的整个工程体系。** 如果说算法团队解决的是"What to compute"，那AI Infra 团队解决的是"How to compute at scale"。
