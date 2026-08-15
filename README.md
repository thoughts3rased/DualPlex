# DualPlex

A Plex music client for the Nintendo 3DS family of systems, using the HTTP API to stream music.

## Features
- Connects to your personal Plex Media Server
- Browse and play music directly from your library
- Playback status; volume follows the console's physical volume slider

## Prerequisites
To compile this project, you will need `devkitPro` with the following packages installed:
- `3ds-dev`
- `3ds-curl`
- `3ds-mpg123`
- `3ds-mbedtls`

## Setup
1. Clone this repository: `git clone <repo>`
2. Run the setup script to download required third-party libraries (e.g. cJSON):
   - Windows: Run `setup.bat`
   - Linux/macOS: Run `./setup.sh`

## Building
Run `make` in the root of the project to build `DualPlex.3dsx`.

## Usage
Configuration is stored on your SD card at `/3ds/dualplex/config.txt`.
You must manually specify your server URL and token.
If you're upgrading from the old "3DS Plex Client" build, your existing config at
`/3ds/3ds-plex-client/config.txt` is picked up automatically on first launch and
copied to the new location.
Example `config.txt`:
```
server_url=http://192.168.1.100:32400
auth_token=YOUR_PLEX_TOKEN
```

## Controls
- **A**: Select / Play / Pause
- **B**: Back / Stop
- **D-Pad**: Navigate menus
- **Volume Slider**: Adjust volume (no separate in-app volume - the slider controls it directly, like any other 3DS app)
- **L/R**: Cycle the top screen between Now Playing, Lyrics, and Visualizer views
- **L+R together**: Toggle the live log viewer
- **X**: Change visualizer style (while the Visualizer view is active)
- **New 3DS C-Stick left/right**: Toggle shuffle / cycle repeat mode (off → all → one)
- **New 3DS ZL/ZR**: Previous / next track
- **Touch the progress bar** (Now Playing Controls, bottom screen): seek to that point in the track
- **Now Playing Controls** (bottom screen) also has touch buttons for shuffle, prev, play/pause, next, and repeat
- **START**: Exit application

## Project Structure
- `source/`: Contains the main application C source files
- `source/lib/`: Third-party dependencies (like cJSON)
- `gfx/`: Graphical assets
- `romfs/`: File assets to pack into the RomFS
- `data/`: Additional data files
- `tests/`: Host-run unit tests (`make -f tests/Makefile`) - see `tests/README.md`

## License
MIT License.

Playback control icons use [Font Awesome Free](https://fontawesome.com) 6.5.1 (solid style),
licensed under the [SIL OFL 1.1](licenses/fontawesome-LICENSE.txt); only the handful of glyphs
actually used are baked into `data/iconfont.bin` and linked directly into the binary.
