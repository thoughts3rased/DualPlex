# 3DS Plex Client

A Plex music client for the Nintendo 3DS family of systems, using the HTTP API to stream music.

## Features
- Connects to your personal Plex Media Server
- Browse and play music directly from your library
- Volume control and playback status

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
Run `make` in the root of the project to build `3ds-plex-client.3dsx`.

## Usage
Configuration is stored on your SD card at `/3ds/3ds-plex-client/config.txt`.
You must manually specify your server URL and token.
Example `config.txt`:
```
server_url=http://192.168.1.100:32400
auth_token=YOUR_PLEX_TOKEN
volume=80
```

## Controls
- **A**: Select / Play / Pause
- **B**: Back / Stop
- **D-Pad**: Navigate menus
- **L/R**: Adjust volume
- **START**: Exit application

## Project Structure
- `source/`: Contains the main application C source files
- `source/lib/`: Third-party dependencies (like cJSON)
- `gfx/`: Graphical assets
- `romfs/`: File assets to pack into the RomFS
- `data/`: Additional data files

## License
MIT License.
