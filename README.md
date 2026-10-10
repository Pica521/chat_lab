# chat_lab

一个用 C++17 实现的类 QQ 聊天软件。

- 阶段一：命令行聊天（已完成）
- 阶段二：网页版聊天（待实现）
- 阶段三：文件传输（待实现）

## 1.目录结构

```
chat_lab/
├── CMakeLists.txt          # 构建配置
├── README.md               # 本文档
├── .clang-format           # 代码格式化规则
├── .clang-tidy             # 静态检查规则
├── .gitignore
├── common/                 # 网络与协议基础库
│   ├── net.h / net.cpp     # send_all / recv_exact / FdGuard
│   └── protocol.h / .cpp   # 帧结构、编解码、消息类型、错误码
├── server/
│   └── main.cpp            # 服务端：accept、线程、在线表、业务逻辑
├── client/
│   └── main.cpp            # 命令行客户端
├── tests/
│   ├── protocol_test.cpp   # 协议层单元测试
│   ├── client_stub.py      # 端到端功能测试
│   └── stress_test.py      # 压力与边界测试
├── docker/
│   └── Dockerfile          # 客户端容器镜像
├── scripts/
│   ├── client.sh           # 启动容器客户端
│   └── run_client.sh       # 启动虚拟机内的客户端
└── docs/
    ├── protocol.md         # 协议说明
    └── design.md           # 设计说明
```

## 2.环境要求

- Debian 12
- g++ 12.2 / clang++ 14.0.6
- CMake 3.25.1+
- Ninja 1.11.1+
- nlohmann-json3-dev、libssl-dev
- Docker（用于容器化客户端）

安装依赖：

```bash
sudo apt update
sudo apt install -y git build-essential clang cmake ninja-build gdb valgrind \
  libssl-dev nlohmann-json3-dev tcpdump iproute2 netcat-openbsd curl
```

## 3.编译

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

## 4.运行

### 服务端（虚拟机里）

```bash
./build/chat_server          # 默认监听 0.0.0.0:9000
./build/chat_server 9001     # 或指定端口
```

### 客户端（虚拟机里）

```bash
./scripts/run_client.sh                      # 默认 127.0.0.1:9000
./scripts/run_client.sh 127.0.0.1 9001       # 或指定 host 和 port
```

### 客户端（Docker 容器里）

确保 `chatlab:dev` 镜像和 `chatnet` 网络已创建：

```bash
docker build -t chatlab:dev docker/
docker network create chatnet    # 只需执行一次
```

启动一个容器客户端：

```bash
./scripts/client.sh
```

## 5.演示流程

1. 终端 1：启动服务端
   ```bash
   ./build/chat_server
   ```

2. 终端 2：启动容器客户端 A，登录 `alice`
   ```bash
   ./scripts/client.sh
   ```

3. 终端 3：启动容器客户端 B，登录 `bob`
   ```bash
   ./scripts/client.sh
   ```

4. 在客户端里输入命令：

   | 命令 | 作用 |
   |---|---|
   | `/list` | 查看在线用户列表 |
   | `/broadcast <消息>` | 群发 |
   | `/msg <用户名> <消息>` | 私聊 |
   | `/quit` | 退出 |

## 6.测试

### 单元测试

```bash
ctest --test-dir build --output-on-failure
```

### 端到端功能测试

```bash
python3 tests/client_stub.py
```

### 压力与边界测试

```bash
python3 tests/stress_test.py
```

覆盖内容：
- 1 MiB 消息完整送达
- 超限消息收到 ERROR 1005
- 一条消息分三次发送，服务端正确还原
- 坏数据（MAGIC/VERSION/LENGTH/JSON/未知类型）不崩

## 7.文档

- [协议说明](docs/protocol.md)：帧结构、消息类型、错误码、边界处理
- [设计说明](docs/design.md)：设计选型、并发模型、实现细节

## 8.协议概览

帧格式（大端序）：

```
+--------+---------+------+--------+------------------+
| MAGIC  | VERSION | TYPE | LENGTH | PAYLOAD          |
| 2 B    | 1 B     | 1 B  | 4 B    | LENGTH 字节       |
+--------+---------+------+--------+------------------+
```

- MAGIC：`0x434C`（ASCII "CL"）
- VERSION：`0x01`
- LENGTH：PAYLOAD 的字节数
- PAYLOAD：UTF-8 JSON

上限：

- 协议层：4 MiB
- 业务层 (单挑内容)：1 MiB

## 9.已知限制

- CLI 输入输出交错。
- 未实现心跳，无法检测被 `docker pause` 冻结的客户端。
- 未实现文件传输。