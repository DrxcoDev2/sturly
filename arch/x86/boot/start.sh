#!/bin/bash

set -e

# 1. Crear estructura básica
mkdir -p initramfs/{bin,sbin,etc,proc,sys,usr/bin,usr/sbin,dev}

# 2. Copiar BusyBox
cp /bin/busybox initramfs/bin/

# 3. Crear symlinks para todos los comandos de BusyBox
cd initramfs/bin
for cmd in $(./busybox --list); do
  [ "$cmd" != "busybox" ] && ln -sf busybox "$cmd"
done

cd ../../

# 4. Crear archivo /init
cat > initramfs/init << 'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
echo "¡Bienvenido a tu propio OS con Linux Kernel!"
exec /bin/sh
EOF

chmod +x initramfs/init

# 5. Crear initramfs.cpio.gz
cd initramfs
find . | cpio -o -H newc | gzip > ../initramfs.cpio.gz
cd ..

echo "✅ initramfs.cpio.gz creado correctamente"
