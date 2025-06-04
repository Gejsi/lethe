# Lethe

Compile all executables

```bash
mkdir -p build
cd build
cmake -G Ninja ..
ninja
```

## Playground

Useful to play around with the local-memory swapper or any VoliMem APIs.

```bash
build/bin/playground
```

## RDMA example

Server
```bash
build/bin/server
```

Client
```bash
build/bin/client -a 10.0.0.1 -s foo
```
