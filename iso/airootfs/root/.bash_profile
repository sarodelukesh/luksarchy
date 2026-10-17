[[ -f ~/.bashrc ]] && . ~/.bashrc

if [[ -z $DISPLAY && $(tty) == /dev/tty1 ]]; then
    exec ~/.automated_script.sh
fi
