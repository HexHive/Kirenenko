# Kirenenko

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## Pre-reading

For those who only need the original features, see [ChengyuSong/Kirenenko](https://github.com/ChengyuSong/Kirenenko). But noticed that it is no longer maintained, using [R-Fuzz/symsan](https://github.com/R-Fuzz/symsan) maybe a better choice.

If the Constraints Dumping feature brought by this repository is just what you are looking for, please read on.

## Preparations

Before the tutorial starts, let's do some preparations to deploy Kirenenko in your environment.

:warning: It is strongly recommended that using *Ubuntu 18.04 on amd64 architecture* to deploy it. Otherwise you may encounter strange problems in certain situations. Welcome to [create an issue](https://github.com/SonicStark/Kirenenko/issues/new/choose) if you need some help.

### Step 1: Get source code of *Kirenenko*

Currently it is recommended that get the source code as a git repository:

```shell
git clone https://github.com/SonicStark/Kirenenko.git
```

Kirenenko depends on [z3](https://github.com/Z3Prover/z3.git) and [libdft64](https://github.com/AngoraFuzzer/libdft64.git). After cloning the repo, get those sub-modules prepared:

```shell
git submodule init
git submodule update
```

### Step 2: Prepare proper version of clang

:warning: For various reasons, today's Kirenenko can **ONLY** work with **clang from LLVM 6.0.0**.

Here are two options. You **MUST** choose and **ONLY** choose one of them. However using `apt` should be your first choice because this can avoid potential compilation failure and is more friendly to normal users.

---

:star: **Option A: Use `apt`**

For **Ubuntu 18.04**:

```shell
sudo apt-get install clang
```

For other Linux distributions, things become a little complicated. For example, in **Ubuntu 16.04** you firstly need

```shell
sudo apt-get install clang-6.0
```

But this is not enough. It allows you call `clang-6.0` or `clang++-6.0` in your shell, but not `clang` and `clang++` which are exactly what we need. (Admittedly, this is strange:confused:, but it is!) So in the next you should run

```shell
ln -s /usr/bin/clang-6.0 /usr/bin/clang
ln -s /usr/bin/clang++-6.0 /usr/bin/clang++
```

to make exactly `clang` and `clang++` callable in your shell.

---

:star: **Option B:  Get from `releases.llvm.org`**

You can find various versions of **LLVM** at [LLVM Download Page](https://releases.llvm.org/), and clang is released as part of regular LLVM releases. What we need can be found at https://releases.llvm.org/download.html#6.0.0. Usually for Ubuntu 16.04 or later [this one](https://releases.llvm.org/6.0.0/clang+llvm-6.0.0-x86_64-linux-gnu-ubuntu-16.04.tar.xz) is what you need.

After downloading and extracting the archive, you should make the libraries searchable, and the binaries including `clang` as well as `clang++` callable in your shell. Usually you can do this by setting `PATH` and `LD_LIBRARY_PATH` in your shell.

An example of the process mentioned above can be found at `./build/install_llvm.sh`. You can use `PREFIX=/path-to-install ./build/install_llvm.sh` and then append the following entries in the shell configuration file (~/.bashrc, or ~/.zshrc, or others). After appending, restart your shell and run `ldconfig`.

```shell
export PATH=/path-to-clang/bin:$PATH
export LD_LIBRARY_PATH=/path-to-clang/lib:$LD_LIBRARY_PATH
```

---

To ensure the needed clang is ready, follow these steps:

:one:    Run `clang --version` and `clang++ --version` in your shell. `clang version 6.0.0` is expected in the output of both.

:two:    Run `clang -print-search-dirs` in your shell. `llvm-6.0/lib` is expected in the output.

:three:  Run `ldconfig -p` in your shell. `libclang-6.0.so` and `libLLVM-6.0.so` is expected in the output.


### Step 3: Install dependency

```shell
sudo apt-get install          \
  build-essential  cmake      \
  gcc-multilib  g++-multilib  \
  zlib1g-dev                  \
  libstdc++6  linux-libc-dev  \
  libc++-dev  libc++abi-dev   \
  python  python-pip
```

### Step 4: Compile source code

The build script will resolve most dependencies and setup the 
runtime environment.

#### Normal Version

If you want to build the original version **without** Constraints Dumping feature, use

```shell
BUILD_TYPE=0 ./build/build.sh
```

#### Constraints Dumping

If you need the Constraints Dumping feature, use 

```shell
BUILD_TYPE=1 ./build/build.sh
```

:warning: Enable Constraints Dumping means
  1. Branch flipping will be disabled. Kirenenko will flip the condition and generate the corresponding new input by default in a *Normal Version*.
  2. Input generation will be disabled.
  3. Additional file may be created in each run to store the symbolic constraints.

If you want to separate the constraints dumped each time for some debugging purposes, you can use

```shell
BUILD_TYPE=2 ./build/build.sh
```

Then each line consisting of 50 '+' in the dump file will indicate the split point.



## Tutorial: Use Constraints Dumping Feature to Collect Constraints

### Step 1: Enable the feature when building

```shell
BUILD_TYPE=1 ./build/build.sh
```

### Step 2: Build target with *ko-clang*/*ko-clang++*

Constraints Dumping requires instrumentation based on a customized DataFlow Sanitizer. You need to compile the target with the drop-in replacement of clang provided by Kirenenko. Usually after running `./build/build.sh` successfully, you can find a directory called `bin` in the directory where this *README* is located, in which `ko-clang` and `ko-clang++` are stored. Use `ko-clang` as your `CC` and `ko-clang++` as your `CXX`.

Here we use `tests/mini/mini.c` as an example target.

```shell
cd ./tests/mini;
../../bin/ko-clang mini.c -o mini.out
```

Then we generate an input for this target:

```shell
echo "AAAAAAAAAAAAAAAAAAAA" > input.txt
```

### Step 3: Determine where to dump constraints

As [SanitizerCommonFlags](https://github.com/google/sanitizers/wiki/SanitizerCommonFlags) says, here we use `TAINT_OPTIONS` to store options.

The following flags are closely related to constraints dumping:

|  Flag  |  Default  |  Description  |
|--------|-----------|---------------|
| dmp_constraints | ./constraints.dmp | Dump branch condition to the file located at "dmp_constraints.pid". If flag *log_exe_name* is set, executable name will be appended before pid. (as in "dmp_constraints.exe_name.pid") The special values are "stdout" and "stderr". It is strongly recommended that NOT to use "stderr" or any other file path that may overlap with flag *log_path*. |
| log_path | stderr | Write logs to "log_path.pid". The special values are "stdout" and "stderr". The default is "stderr". |
| log_exe_name | false | Mention name of executable when reporting error and append executable name to logs (as in "log_path.exe_name.pid"). |

Here we use 

```shell
TAINT_OPTIONS=taint_file=./input.txt:dmp_constraints=./cons.dmp
```

for our target `mini.out` and the corresponding input PoC `input.txt` (both are located in */home/Kirenenko/tests/mini* and assume that we are working in this directory).

:warning: The path of input file to target must be specified correctly to `taint_file`. It should be ensured that there are no characters which can affect path parsing and flag parsing in the path string, such as colons and spaces, otherwise unexpected accidents may occur. And for stability, absolute path should be preferred.

### Step 4: Launch

Run

```shell
TAINT_OPTIONS=taint_file=./input.txt:dmp_constraints=./cons.dmp ./mini.out ./input.txt
```

This time we got a `cons.dmp.28219`. That's exactly what we expect. Here are its contents:

```text {.line-number}
(bvugt (concat k!2 k!1) #x300c)
(let ((a!1 (and (= ((_ extract 7 6) k!7) #b00)
                (bvule (concat ((_ extract 5 0) k!7) k!6 k!5 k!4)
                       #b111010110111100110100010110000))))
(let ((a!2 (not (or (bvule #x3ade68b6 (concat k!7 k!6 k!5 k!4)) a!1))))
  (and (not (bvule #x303e (concat k!2 k!1)))
       (= ((_ extract 7 2) k!10) #b111111)
       (= k!11 #x1e)
       (= k!12 #x0a)
       (= k!13 #xfa)
       (not (and (= k!10 #xfd) (= k!11 #x1e) (= k!12 #x0a) (= k!13 #xfa)))
       a!2
       (= k!14 #x15)
       (= k!15 #xcd)
       (= k!16 #x5b)
       (= k!17 #x07))))
```