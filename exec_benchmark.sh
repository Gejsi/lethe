#!/bin/sh

export LD_PRELOAD="$HOME/desk/lethe/build/lib/liblethe.so"
exec "$HOME/desk/lethe/build/bin/run_benchmark" "$@"

# export LD_PRELOAD="./build/lib/liblethe.so"
# exec "./build/bin/run_benchmark" "$@"
