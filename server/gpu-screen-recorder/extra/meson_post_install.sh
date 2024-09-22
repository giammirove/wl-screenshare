#!/bin/sh

# Needed to remove password prompt when recording a monitor (without desktop portal option) on amd/intel or nvidia wayland
/usr/sbin/setcap cap_sys_admin+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gsr-kms-server \
    || echo "\n!!! Please re-run install as root\n"

# Cant do this because it breaks desktop portal (create session)!!!.
# For some reason the desktop portal tries to access /proc/gpu-screen-recorder-pid/root from the portal process
# which doesn't work because for some reason CAP_SYS_NICE on a program makes /proc/self/root not readable by other processes.
# The reason portal reads that file might be because portal seems to have a security feature where its able to identify the
# process and if the session token is stolen by another application then it will ignore the session token as it wasn't that
# application that created the session token.
# ---
# This is needed (for EGL_CONTEXT_PRIORITY_HIGH_IMG) to allow gpu screen recorder to run faster than the heaviest application on AMD.
# For example when trying to record a game at 60 fps and the game drops to 45 fps in some place that would also make gpu screen recorder
# drop to 45 fps unless this setcap is used.
#/usr/sbin/setcap cap_sys_nice+ep ${MESON_INSTALL_DESTDIR_PREFIX}/bin/gpu-screen-recorder
