# Lethe

Compile all executables

```bash
mkdir -p build
cd build
cmake -G Ninja ..
ninja
```

## RDMA client/server

Run a YCSB-like benchmark over RDMA to see the swapper in action.

Server (e.g., memory node)

```bash
build/bin/server
```

Client (e.g., compute node)

```bash
# Run with defaults (1 thread, 3M keys, 1M ops, zipfian, workload A)
build/bin/client

# Customize benchmark and swapper parameters
build/bin/client -t 1 -k 3000000 -o 1000000 -d zipfian -w A --cache-mb 100

# All options
build/bin/client --help
```

| Flag | Long | Description | 
|------|------|-------------|
| `-t` | `--threads` | Number of threads |
| `-k` | `--keys` | Number of keys to load |
| `-o` | `--ops` | Number of operations |
| `-d` | `--dist` | Distribution: `uniform` or `zipfian` |
| `-w` | `--workload` | YCSB workload: A, B, C, or D |
| `-m` | `--cache-mb` | Cache size in MB |
| `-c` | `--cache-gb` | Cache size in GB |
| `-r` | `--rebalance` | Toggle rebalancer (0/1) |
| `-S` | `--num-shards` | Number of LRU shards |
| `-a` | `--addr` | Server address |
| `-p` | `--port` | Server port |

## Hook

Let the swapper manage the memory of an application by hooking via LD_PRELOAD.

```bash
LD_PRELOAD=build/lib/liblethe.so <cmd>
```