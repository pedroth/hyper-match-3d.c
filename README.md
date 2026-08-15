# HyperMatch 3D (C Port)

HyperMatch 3D is a manifold match-3 game where you swap neighboring spheres by selecting two adjacent nodes on a mesh. When three or more connected spheres share the same color, they are removed. The goal is to remove 10% of the spheres as quickly as possible.

This repository is a native C port based on [pedroth/hyper-match-3d](https://github.com/pedroth/hyper-match-3d).

This port was generated using AI code generation (VS Code agents) from the original JavaScript project. It leverages the [tela.c](https://github.com/pedroth/tela.c) library, and its [src/index.c](https://github.com/pedroth/tela.c/blob/main/src/index.c) was copied into the project after removing unnecessary code.

## Features

- Native C application using SDL2 for windowing and audio.
- Ray-traced mode (default) plus a faster raster mode.
- Mouse-driven selection and camera orbit controls.
- Looping background music compatible with the original project asset format.

## Build and Run

From the repository root:

    gcc -O3 -march=native -ffast-math -fopenmp -o app game.c -lSDL2 -lm
    ./app

Optional runtime flags:

- -r: use raster rendering instead of ray tracing.
- -s: disable the parallel ray-trace path.

Example:

    ./app -r

## Controls

- Left click: select spheres and perform swaps when two selected spheres are adjacent.
- Right click + drag: orbit the camera.
- Mouse wheel: zoom in/out.
- Enter: start the game from the title screen, or restart after finishing.
- Esc: quit.

## Assets and Paths

The game expects assets in `./assets`.

Expected files include:

- bunny.obj
- index.jpg (background)
- fonts.png (font atlas)
- index.wav (music)

## Music Format Note (Important)

In the original JavaScript project, the file named `index.wav` is actually raw f32le mono audio at 48,000 Hz (not always a RIFF WAV container).

This C port supports both:

- Standard RIFF WAV via SDL_LoadWAV.
- Raw f32le fallback (same format used by [pedroth/hyper-match-3d](https://github.com/pedroth/hyper-match-3d)).

If you want to generate a compatible raw audio file from another source:

    ffmpeg -i input_audio.ext -ac 1 -ar 48000 -f f32le -c:a pcm_f32le assets/index.wav

## Gameplay Objective

- Match and clear connected groups of 3+ spheres of the same color.
- Clear at least 10% of the mesh to finish.
- Score is based on the cleared ratio and completion time.

## Credits (from the original project)

- Sound by Mikhail from Pixabay.
- Background image from Texturify.
- Mesh based on the Simplified Stanford Bunny.

## Reference Project

Original project used as reference:

- [pedroth/hyper-match-3d](https://github.com/pedroth/hyper-match-3d)
