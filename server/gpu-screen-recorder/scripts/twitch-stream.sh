#!/bin/sh

[ "$#" -ne 3 ] && echo "usage: twitch-stream.sh <window_id> <fps> <livestream_key>" && exit 1
active_sink=default_output
gpu-screen-recorder -w "$1" -c flv -f "$2" -q high -a "$active_sink" -o "rtmp://live.twitch.tv/app/$3"
