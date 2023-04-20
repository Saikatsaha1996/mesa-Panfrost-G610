Valhall CSF Tests
=================

The ``csf`` branch contains a test program for v10 Valhall GPUs (G710
etc.) which uses the Arm ``kbase`` kernel driver, which is generally
present on vendor kernels but is not in the upstream Linux kernel.

However, the kernel driver source can also be downloaded `from Arm
<https://developer.arm.com/downloads/-/mali-drivers/valhall-kernel>`_,
of which the newer releases should work well enough with a mainline
kernel (though some work may be needed to integrate the vendor
platform).

Making sure that the ``libmali`` blob drivers work before trying this
program is recommended, otherwise you will be trying to debug
userspace and kernel bugs at the same time.

Note that firmware is required for these GPUs, for RK3588 try
downloading the file from the Rockchip `libmali
<https://github.com/JeffyCN/rockchip_mirrors/tree/libmali/firmware/g610>`_
repo, and placing it in ``/lib/firmware/``.

Compiling
---------

.. code-block:: sh

  $ mkdir build
  $ cd build
  $ meson --buildtype=debug -Dgallium-drivers=panfrost -Dvulkan-drivers=
  $ ninja src/panfrost/csf_test

Running
-------

.. code-block:: sh

  $ src/panfrost/csf_test

will run the tests.

Normally it will start running cleanup steps as soon as one test
fails, though setting the environment variable ``TEST_KEEP_GOING=1``
will change this behaviour.

Test failures
-------------

Gitlab issues can be created against `my repo
<https://gitlab.freedesktop.org/icecream95/mesa/-/issues>`_, though
some problems should be easy to fix (wrong permissions on
``/dev/mali0``?).

Include all output from running the test program. Including logs from
``strace`` might also help.

Command stream test script
--------------------------

``src/panfrost/csf_test/interpret.py`` is a test script for assembling
and executing command streams.

To use it, symlink the ``csf_test`` binary into ``$PATH`` and optionally
also write a ``rebuild-mesa`` script which recompiles ``csf_test``.

Then running ``interpret.py`` will execute the ``cmds`` variable,
which is defined inside the script file.

Example:

.. code-block:: txt

  @ comments are started with '@'

  @ run on command stream 2
  !cs 2
  @ allocate some memory
  !alloc x 4096
  @ allocate event memory, for evstr instructions
  !alloc ev 4096 0x8200f

  mov x50, $x

  @ dump all registers to the memory starting at x50
  regdump x50

  @ dump the memory region named 'x'
  !dump x 0 4096
