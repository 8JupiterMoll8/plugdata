# 🚀 Disaster Recovery & Standalone Build Guide

> **Purpose**: Complete, step-by-step restoration guide to re-clone, compile, and run `plugdata_standalone` and `PlugData-MCP-Server` from scratch on a new or recovered machine.

---

## 1. Fast Track: 3-Step Standalone Build

If your system already has dependencies installed, run this in your terminal:

```bash
# 1. Clone the repository with all submodules
git clone --recursive https://github.com/8JupiterMoll8/plugdata.git
cd plugdata
git checkout feat/v4-zero-dropout-copilot

# 2. Configure with CMake (Release mode)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Compile the standalone executable using all CPU cores
cmake --build build --target plugdata_standalone -j$(nproc)
```

**Result**: Your compiled standalone binary is placed at:
```bash
Plugins/Standalone/plugdata
```
Run it directly with:
```bash
./Plugins/Standalone/plugdata
```

---

## 2. System Dependencies (Fresh Linux Install)

On a completely fresh Ubuntu/Debian/Arch machine, install the required C++ and audio build libraries:

### Ubuntu / Debian:
```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  ninja-build \
  libasound2-dev \
  libjack-jackd2-dev \
  libgl1-mesa-dev \
  libx11-dev \
  libxrandr-dev \
  libxinerama-dev \
  libxcursor-dev \
  libfreetype6-dev \
  libcurl4-openssl-dev
```

### Arch / Manjaro:
```bash
sudo pacman -S --needed base-devel cmake git ninja alsa-lib jack2 mesa libx11 libxrandr libxinerama libxcursor freetype2 curl
```

---

## 3. Fast Track: MCP Server Setup

The MCP server coordinates AI sound design with your running PlugData standalone.

```bash
# 1. Clone the MCP Server repository
git clone https://github.com/8JupiterMoll8/PlugData-MCP-Server.git
cd PlugData-MCP-Server
git checkout feat/v4-zero-dropout-copilot

# 2. Install Node.js dependencies (requires Node 18+)
npm install

# 3. Compile TypeScript
npm run build

# 4. Verify system integrity (runs full 47-check test battery)
npm run validate
```

---

## 4. How Standalone & MCP Server Connect (OSC Bridge)

The C++ Standalone and TypeScript MCP server communicate over local UDP/OSC:

| Component | Default Port | Role |
| :--- | :---: | :--- |
| **`plugdata_standalone`** | `19011` | C++ engine receives commands (`/pd/batch_atomic`, `/meter`, `/transport`) |
| **`mcp-server`** | `19020` | TypeScript receives C++ telemetry & command receipts |

1. Launch `plugdata_standalone`:
   ```bash
   ./Plugins/Standalone/plugdata
   ```
2. Enable the MCP Bridge:
   * Open PlugData **Settings** $\rightarrow$ **MCP Bridge**.
   * Ensure it is toggled **ON** and ports match (`19011` / `19020`).
3. Connect your AI client (Antigravity IDE or OpenCode). The server auto-handshakes in $\sim 2\text{ms}$.

---

## 5. Troubleshooting & Gotchas

* **Submodules Missing / Compilation Fails**:
  If you cloned without `--recursive`, submodules in `Libraries/` will be empty. Fix with:
  ```bash
  git submodule update --init --recursive
  ```
* **Port Conflict (`EADDRINUSE: 19020`)**:
  Kill any lingering background processes holding port 19020:
  ```bash
  fuser -k 19020/udp
  ```
* **Clean Rebuild from Scratch**:
  To completely purge old build artifacts:
  ```bash
  rm -rf build
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target plugdata_standalone -j$(nproc)
  ```
