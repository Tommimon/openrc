# INSTRUCTIONS

## VM setup

Download x86_64 standard iso from [https://alpinelinux.org/downloads/](https://alpinelinux.org/downloads/)

Setup the VM on virt manager, 10GB storage is fine, no special setup needed.

## Alpine setup

Run `setup-alpine`:

- Leave default choice for mirrors
- Set `root` as password for user `root`
- Don't create any other user
- Set `root` login to `yes` when asked
- Leave other default ssh settings

Create a `openrc` folder into `/root`

Install the following packages:

```bash
nano
meson
gcc
musl-dev
cmake
linux-pam-dev
audit-dev
libcap-dev
```

## Build and install

Find out ip in guest system with `ifconfig`.

Make sure to have `sshpass` installed, navigate to `openrc` folder and run the following command:

```fish
sshpass -p 'root' scp -r . root@192.169.122.124:/root/openrc; and sshpass -p 'root' ssh root@192.169.122.124 'rm -rf /root/openrc/builddir; cd /root/openrc; meson setup builddir; meson compile -C builddir; DESTDIR=/ meson install -C builddir; reboot'
```
