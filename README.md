# Memory Stall Software Harvester
This repository contains the prototype of the Memory Stall Software Harvester (MSH), the first system designed to transparently and efficiently harvest memory-bound CPU stall cycles in software. Why harvest idle cycles in software when there are well-known hardware harvesting mechanisms like Intel Hyperthreads? The answer is that hardware mechanisms are inflexible: they cannot differentiate between latency-sensitive applications and others, and they only provide limited concurrency (e.g., 2 threads), often harvesting too much or too little. MSH allows for adjusting the length and frequency of cycle harvesting more precisely, providing a unique opportunity to utilize stalled cycles of latency-sensitive applications while meeting different latency SLOs. For more details about MSH, please take a look at our OSDI'24 [paper](https://www.usenix.org/system/files/osdi24-luo.pdf).

## Limitation in Prototype
Our prototype has some assumptions on primary and scavenger applications to simplify the implementation
- Primary application must use `pthread` library.
- To use a lightweight context switch, our prototype assumes that scavenger applications are given as shared objects and include several symbols and an entry point. Thus, you need to modify and recompile your scavenger application. The required transformation is straightforward and short.
- We assume x86-64 platform and use gcc/g++ with `-fno-omit-frame-pointer` and `-mno-red-zone` flags enabled.
  - Tested with gcc/g++ 11.4 and 22.04.1-Ubuntu (kernel version 6.5.0).  

## Basic Usage
### Workflow
The use of MSH involves three pieces of software: profiler (scripts based on `perf`), binary instrumentation (`llvm-bolt`), and MSH runtime(`libmsh`). We assume that a user has a primary application and a set of scavenger applications that will run when there are stalled cycles in the primary application. One can use MSH in the following way.
- Modify scavenger applications
- Profile primary/scavenger applications.
- Modify primary/scavenger binaries through binary instrumentation with the profile result.
- Finally, run instrumented binaries with MSH runtime.

We'll show you how this workflow works with one simple primary(`ptrchase`) and scavenger(`compute.so`).

### Prerequisite: Modify Scavenger
MSH requires a scavenger to have the following symbols in the file containing `main` function.
```
extern "C" {
int    crt_pos      = 0;
int    argc         = 0;
char **argv         = 0;
}
```
Then, rename `main` function to `entry` as shown below.

```
extern "C" int
entry(void) {
...
}
```
Lastly, compile the scavenger to a shared object file.

### Prerequisite: BOLT
We implemented all the binary-level instrumentation in BOLT. Here is the patch instruction:
```
git clone https://github.com/llvm/llvm-project.git
git checkout 30c1f31
patch -p1 < msh_bolt.diff
```
Then, compile BOLT by following the instruction in [BOLT page]([url](https://github.com/llvm/llvm-project/tree/main/bolt)). We'll assume that `llvm-bolt` is in `PATH` from now on.

### Profile and Instrument Primary
```
# Compile ptrchase
cd apps
mkdir build
make primary
```

```
# usage: ./do_prof_primary.sh [binary] [args]
# Pointer chase 50MB array
cd ${HOME}
./do_prof_primary.sh ./apps/build/ptrchase 13107200
```

`Makefile` in `apps` has rules that use BOLT to perform binary instrumentation. We assume that `llvm-bolt` is in `PATH`.
```
cd apps
make build/ptrchase.bolt
```

### Profile and Instrument Scavenger
```
# Compile compute.so
cd apps
make scavenger
```

```
cd ${HOME}
echo "$(pwd)/apps/build/compute.so" > scav.txt
./do_prof_scavenger.sh
```

You can instrument scavenger in a similar way. However, you need to specify the average yield distance (in nanoseconds) in scavenger to bound it.
```
cd apps
YIELD_DISTANCE=100 make build/compute.so.bolt
```

### Run with MSH Runtime
We used `LD_PRELOAD` trick to attach the runtime to the primary without recompilation.
```
cd libmsh
make all

cd ..
export LD_PRELOAD=$(pwd)/build/libmsh.so:${LD_PRELOAD}
export LD_LIBRARY_PATH=$(pwd)/build:${LD_LIBRARY_PATH}

echo "$(pwd)/apps/build/compute.so.bolt" > scav.txt
export MSH_SCAV_POOL_PATH=$(pwd)/scav.txt
export SKIP_FIRST_THREAD=1

./build/apps/ptrchase.bolt 13107200
```
Note: don't forget to reset `LD_PRELOAD` when you finish testing MSH. MSH runtime will intercept all the following `pthread` functions otherwise

## TODO
- Add the use of more complex primary (e.g., tailbench) and scavenger (e.g., graph algorithm).
- Explain knobs

## Developed by
Sam Son and Zhihong Luo
