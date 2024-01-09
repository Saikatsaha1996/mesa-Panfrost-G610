`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

Valhall v10 "CSF" support branch—for Mali G710/G610.
# All credits - 
`@icecream95 (Panfrost dev)`

`@mesa Gpu driver development team`

# Panfrost GPU driver credits without root

`@icecream95 (Panfrost dev)`

`Mali G610 & 710 GPU driver Released for proot & Glibc`

<https://github.com/Saikatsaha1996/mesa-Panfrost-G610/tree/mesa-23.0.0-devel-20240109_armhf_arm64>


```

git clone -b Panfrost-G610 --depth 1 https://github.com/Saikatsaha1996/mesa-Panfrost-G610
cd mesa-Panfrost-G610
mkdir build
cd build

CFLAGS="-O3" meson -Dgallium-drivers=panfrost,swrast -Dvulkan-drivers= -Dbuildtype=release -Dllvm=disabled -Dprefix=/usr

ninja -j8

ninja install
```

# Note :- you must disable sys call with `--no-sysvipc` proot loging time..

  
