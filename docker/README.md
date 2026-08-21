# Build images

| File | Purpose |
|---|---|
| `Dockerfile.debian` | Ubuntu 24.04: GCC 13 / Clang 17 host tools, GStreamer + mosquitto dev packages, aarch64 cross toolchain with multiarch dev packages. Anyone can build it. CI uses it on hosted runners. |
| `Dockerfile.petalinux` | PetaLinux tools on Ubuntu 22.04. Needs the AMD installer as a build secret. Base for BSP builds. |
| `Dockerfile.sdk` | Built from the PetaLinux image: reference BSP (zcu104) with the application's libraries, `petalinux-build --sdk`, QEMU artefacts. Final stage keeps only the SDK, QEMU images and host tools. This is the image whose binaries match the target sysroot. |
| `entry.sh` | Runs the stages `lint,host,cross,qemu` against `/work`. |
| `plnx/` | Fragments applied to the reference PetaLinux project: rootfs packages, SDK contents, a UIO device-tree node for QEMU. |

Build and run without vendor tooling:

```
docker build -f docker/Dockerfile.debian -t optronic/debian .
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/work" -v optronic-ccache:/ccache optronic/debian
```

With the PetaLinux installer and BSP at hand:

```
DOCKER_BUILDKIT=1 docker build -f docker/Dockerfile.petalinux -t optronic/plnx:2024.1 \
  --build-arg PLNX_INSTALLER=petalinux-v2024.1-05202009-installer.run \
  --build-context plnx=$HOME/Downloads .
mkdir -p bsp && cp $HOME/Downloads/xilinx-zcu104-v2024.1-final.bsp bsp/
docker build -f docker/Dockerfile.sdk -t optronic/sdk:2024.1 .
docker run --rm -v "$PWD:/work" -v optronic-ccache:/ccache -e STAGES=lint,host,cross,qemu optronic/sdk:2024.1
```

Docker runs on build hosts only. Nothing in these images is deployed to the target; the target runs the Yocto image.
