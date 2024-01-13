`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

Valhall v10 "CSF" support branch—for Mali G710/G610.
# All credits - 
`@icecream95 (Panfrost dev)`

`@mesa Gpu driver development team`

# Panfrost GPU driver credits without root

`@icecream95 (Panfrost dev)`

`Mali G610 & 710 GPU driver Released for Termux Glibc`

[Released for Termux Glibc](https://github.com/Saikatsaha1996/mesa-Panfrost-G610/releases)

```

git clone -b Panfrost-G610 --depth 1 https://github.com/Saikatsaha1996/mesa-Panfrost-G610
cd mesa-Panfrost-G610
mkdir build
cd build

CFLAGS="-O3" meson -Dgallium-drivers=panfrost,swrast -Dvulkan-drivers= -Dbuildtype=release -Dllvm=disabled -Dprefix=/usr

ninja -j8

ninja install
```
![Screenshot_2024-01-10-16-12-23-027_com termux x11](https://github.com/Saikatsaha1996/mesa-Panfrost-G610/assets/72664192/3e37849b-fca4-4046-b97e-9b592271cfe0)

# Note :- you must disable sys call with `--no-sysvipc` proot loging time..

  
