#!/bin/sh

window=$(xdotool selectwindow)
window_name=$(xdotool getwindowclassname "$window" || xdotool getwindowname "$window" || echo "Game")
window_name="$(echo "$window_name" | tr '/\\' '_')"
gpu-screen-recorder -w "$window" -f 60 -c mkv -a default_output -r 60 -o "$HOME/Videos/Replays/$window_name"
