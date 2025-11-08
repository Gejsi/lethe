# Lethe

Compile all executables

```bash
mkdir -p build
cd build
cmake -G Ninja ..
ninja
```

## Running

Let the swapper manage the memory of an application by hooking via LD_PRELOAD.

```bash
LD_PRELOAD=build/lib/liblethe.so <cmd>
```

## RDMA example

Simple example running the swapper by
triggering faults on the client manually.

Server (e.g., memory node)

```bash
build/bin/server
```

Client (e.g., compute node)

```bash
build/bin/client
# optionally specify address and port
build/bin/client -a 10.0.0.1 -p 20000
```

## Playground

Useful to play around with the local-memory swapper or any VoliMem APIs.

```bash
build/bin/playground
```
