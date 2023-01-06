FROM ubuntu:18.04

RUN apt-get update
RUN apt-get -y upgrade
RUN apt-get install -y git build-essential sudo wget

RUN mkdir -p kirenenko
COPY . kirenenko
WORKDIR kirenenko
RUN mkdir -p install

ENV PREFIX="/kirenenko/install"
RUN ./build/install_llvm.sh

ENV PATH="$PREFIX/clang+llvm/bin:$PATH"
ENV LD_LIBRARY_PATH="$PREFIX/clang+llvm/lib:$LD_LIBRARY_PATH"
RUN ./build/build.sh

