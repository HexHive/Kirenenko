# Kirenenko

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

**STILL IN DEVELOPMENT**

I'm really bad at naming so we probably will change it.
I don't have much time to code so progress will slowdown.

## Building

### Build Requirements

- Linux-amd64 (Tested on Ubuntu 18.04)
- [LLVM 4.0.0 - 12.0.0](http://llvm.org/docs/index.html) :
  run `sudo apt install clang` or
  `PREFIX=/path-to-install ./build/install_llvm.sh`.

### Environment Variables

If installed from source,
append the following entries in the shell configuration file (`~/.bashrc`, `~/.zshrc`).

```
export PATH=/path-to-clang/bin:$PATH
export LD_LIBRARY_PATH=/path-to-clang/lib:$LD_LIBRARY_PATH
```

### Compilation

The build script will resolve most dependencies and setup the 
runtime environment.

```shell
./build/build.sh
```

### System Configuration

As with AFL, system core dumps must be disabled.

```shell
echo core | sudo tee /proc/sys/kernel/core_pattern
```

## Test
Running test from Angora
```
cd /path-to-angora/tests/mini
../../bin/ko-clang mini.c -o mini.taint
python -c "print('A'*20)" > i
TAINT_OPTIONS="taint_file=i" ./mini.taint i
./mini.taint id-0-0-0
```

It doesn't support input growth yet so we need to use a large enough
seed input. It also lacks a driver yet, so we need to manually run
the newly generated test case(s).

Currently I've tested with `bitflip`, `call_fn`, `call_fn2`, `call_fn3`,
`cf1`, `context`, `gep`, `gep2`, `if_eq`, `infer_type`, `memcmp`, `mini`,
`pointer`, `shift_and`, `sign`, `strcmp`, `strcmp2`, `switch` and `switch2`.

## Usage

### Instrument target
I have provided the instrument script under /path-to-Kirenenko/script, users need to modify the
source code path in the script to make it work on your machine

```shell
cd /path-to-Kirenenko/script
./instrument.sh
```

### Collect constraints 
The code is not gorgeous now, I modify the original code of Kirenenko to 1) print branch condition
to the scree  2) disable branch flipping (Kirenenko will flip the condition and generate the 
corresponding new input by default)  3) disable the input generation

I use the *libtiff* as an example, the execute parameter of tiff2pdf is:
```shell
./tiff2pdf /your_poc_path -o tmp.out
```

The symbolic constraints collection command is:
```shell
TAINT_OPTIONS="taint_file=/absolute_path_to_poc" /path_to_tiff2pdf /path_to_poc -o tmp.out > /tmp/flush.data
```
The branch condition will be dumped in to the *flush.data*


