# wl-screenshare
Use your old tablet as second monitor on Wayland

## Note

This project is under development, I work on it with I find time
It has been tested only on Sway. (22/09/2024)

At the current state this project is more a POC.
I use it to study and work, but sometimes it could crash or not work properly.

As of now I have no intention in writing from scratch my own 
implementation to obtain the frames. Instead I will use the already working
code provided by [wf-recorder](https://github.com/ammen99/wf-recorder)
and [gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder/about/) (crazy fast).

Application and server code need refactoring!.

## Design

### Application

The application receives the encoded frames from the TCP server and display
them.

### Server

The server gets the pixel of the screen, encodes the stream and sends it to the
application using TCP socket.

## Build

### Application

Just open the `application/wl-screenshare-app` folder using Android Studio.

### Server

You need `meson` and `ninja` to compile the project.

```
cd server/wl-screenshare-server
meson build --prefix=/usr --buildtype=release
ninja -C build
```

To compile the modified version of `wf-recoder`:
```
cd server/wf-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build install
```

To compile the modified version of `gpu-screen-recorder`:
```
cd server/gpu-screen-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build install
```

## Usage

The server must be reachable from the application over the network throught the port `53516`. 
> [!WARNING]
> There is no encryption so I suggest to not use it under public wifi

The server must run first!

### Application

Insert the IP of the server in the textfield and select the encoding you prefer.
> [!WARNING]
> Encoding of application and server must match

### Server

Example for `wf-recorder` and `wl-screenshare-server`:
```
# h264
wf-recorder -c h264 -d /dev/dri/renderD128 -D -y
wf-recorder -c h264 -d /dev/dri/renderD128 -D -y -o "eDP-1"
# h264
wl-screenshare-server -c h264 -d /dev/dri/renderD128 -D -y
wl-screenshare-server -c h264 -d /dev/dri/renderD128 -D -y -o "eDP-1"

# h265
wf-recorder -c hevc_vaapi -d /dev/dri/renderD128 -D -y
# h265
wl-screenshare-server -c hevc_vaapi -d /dev/dri/renderD128 -D -y
```
where `-o` is the name of your display

Example for `gpu-screen-recorder`:
```
# h264
gpu-screen-recorder -q high -w "eDP-1" -encoder gpu -f 60 -a default_output -k h264 -o wow.mp4
# h265
gpu-screen-recorder -q high -w "eDP-1" -encoder gpu -f 60 -a default_output -k h265 -o wow.mp4
```
where `-w` is the name of your display

### Headless Display on Sway

If you want to use a headless display on Sway:
```
swaymsg create_output
```
This command will create a display `HEADLESS-1`.

`wf-recorder` and `wl-screenshare-server` suport headless display, while
`gpu-screen-recorder` does not.


## Limitations

- single connection 
- data over TCP is not encrypted
- only h264 and hevc are supported by the application
- no headless display with `gpu-screen-recorder`

## TODO

- support `ext-image-capture-source-v1` and `ext-image-copy-capture-v1` screen copy protocols
- allow multiple screens
- refactor application and server
- write unit tests

## Thanks

The server code in `server/wl-screenshare-server` is to be considered a fork of [wf-recorder](https://github.com/ammen99/wf-recorder)
Kudos for the work done by:
- [wf-recorder](https://github.com/ammen99/wf-recorder)
- [gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder/about/)

