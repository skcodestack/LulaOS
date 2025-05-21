# LulaOS

### 一.development environment configuration
#### 1.complier and build tools
-   GCC/G++ complie tool
    ```
      sudo apt install build-essential
    ```

- cross-compile toolchain
  source code compile ,please see [GCC_Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler),if you not want to compile source code ,you can download toolchain from [here](https://pan.baidu.com/s/1pIJBDcw3SzheAG5anMj5Cg?referer=https%3A%2F%2Fcloud.tencent.com%2Fdeveloper%2Farticle%2F2030533%3FpolicyId%3D1003#list/path=%2F)
  
  

#### 2.debug tools
- GDB debug tool
    ```
    sudo apt install gdb
    ```

- qemu emulator

    ```
    sudo apt install qemu-system-x86 qemu-utils
    ```

- bochs emulator
  
  1.download bochs source from [bochs 3.0](https://sourceforge.net/projects/bochs/files/bochs/3.0/)
  
  2.install dependence

  ```
  sudo apt install  libgtk2.0-dev libsdl1.2-dev libx11-dev libncurses5-dev libncursesw5-dev
  ```
  3.compile and install

  ``` 
  export CC=gcc
  export CXX=g++
  export CFLAGS="-Wall -O2 -fomit-frame-pointer -pipe"
  export CXXFLAGS="$CFLAGS"
  
  (./configure --enable-smp \
              --enable-cpu-level=6	\
              --enable-all-optimizations	\
              --enable-x86_64	\
              --enable-pci	\
              --enable-vmx	\
              --enable-debugger	\
              --enable-disasm	\
              --enable-debugger-gui	\
              --enable-logging	\
              --enable-fpu	\
              --enable-3dnow	\
              --enable-sb16=dummy	\
              --enable-cdrom	\
              --enable-x86-debugger	\
              --enable-iodebug	\
              --disable-plugins	\
              --disable-docbook	\
              --with-x -with-x11 -with-term -with-sdl2 ) || exit
  
  make && make install 
  ```



#### 3.grub toolchain

```
sudo apt install grub2-common grub-pc-bin xorriso  
```



