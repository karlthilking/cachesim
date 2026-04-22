#!/bin/bash

if [[ "$(id -u)" -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

echo 'Checking for the following dependencies:'
echo 'llvm-20'
echo 'llvm-20-dev'
echo 'clang/clang++'

LLVM_INCLUDE=$(find /usr/include -type d -name 'llvm-20')
LLVM_RUNTIME=$(find /usr/lib -type d -name 'llvm-20')
CLANG=$(find /usr/bin -name 'clang-20')

if [[ -n $LLVM_INCLUDE && -n $LLVM_RUNTIME && -n $CLANG ]]; then
    echo 'All required dependencies were found'
    exit 0
fi

if command -v apt >/dev/null 2>&1; then
    read -p "Install llvm-20, llvm-20-dev, clang? [y/n]: " ANS
    if [[ $ANS == 'y' ]]; then
        sudo apt-get update
        sudo apt install llvm-20
        sudo apt install llvm-20-dev
        sudo apt install clang-20
    fi
else
    echo 'apt not available'
    exit 1
fi

exit 0

