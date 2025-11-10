# chroma-overlay

X11 overlay that chroma-keys **green** out of a specific window in real time.


## Build

Arch deps:
sudo pacman -S base-devel cmake gcc glfw-x11 glew libx11 libxcomposite libxext libxrender libxfixes

Then:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j


## Run

1. Get the window id:
xwinfo
2. Run the overlay (full-screen transparent window):
./build/chroma_overlay 0x3200007

If your compositor is active, green areas in the target window should appear **transparent**.


## Notes

- This uses `XComposite` + `GLX_EXT_texture_from_pixmap`.
- Overlay is click-through (XShape) and set above (EWMH hint).
- Shader thresholds live in `src/shaders/chromakey.frag` (tune them).
- If the target window resizes, we rebind the pixmap.
- NVIDIA: keep recent drivers; disable “Force Composition Pipeline” if you see lags.

