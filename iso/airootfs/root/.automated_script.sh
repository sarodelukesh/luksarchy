#!/bin/bash

if [[ $(tty) == "/dev/tty1" ]]; then
    setfont ter-v32b
    pacman-key --init
    pacman-key --populate archlinux
    clear
    /usr/local/bin/luksarchy-run
    exec /bin/bash
fi
