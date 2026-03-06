# v2_epoll

`v2_epoll` 是在 `v1_threadpool` 基础上演进的 IO 多路复用版本。

## 目标

- 使用 `epoll` 管理高并发连接，避免“一连接一线程”。
- 继续复用原有协议：`list/get/put/quit`。
- 保持与 v1 客户端交互兼容。

## 构建

```bash
make clean
make server_host
make client_host
```

AArch64 交叉编译：

```bash
make server_aarch64
make client_aarch64
```

## 运行

服务端：

```bash
./server/server_host 0.0.0.0 9000
```

客户端：

```bash
./client/client_host <server_ip> 9000
```

## 压测

沿用脚本：

```bash
./scripts/stress_test.sh <server_ip> 9000 8 5 128
```

说明：
- 默认成功自动清理临时目录。
- 失败会保留日志目录，便于定位问题。
