# IMPLEMENTATION PLAN — MCP Bridge Enable/Disable + Status (Advanced Settings)

> Feature: expose the native C++ MCP bridge in PlugData's Advanced settings tab
> (on/off toggle + configurable ports + status), and make the bridge log its
> lifecycle to the PlugData console so connection problems are visible instead
> of silent.
>
> Repo: `plugdata-core` (bridge + GUI). Optional coordinated change: `mcp-server`.

---

## 1. Scope & goals

- **Enable/disable** the bridge at runtime (default ON).
- **Show status** ("connected" / "listening" / "stopped" / "port in use").
- **Configurable listen/send ports** (with clear warnings about restart / server coordination).
- **Console log lines** on: started, stopped, port conflict, server handshake, server lost.
- **No regression** for the default AI/MCP workflow (must be ON by default).

Non-goals (for this pass): auto-discovery, authentication, remote hosts, TLS.

---

## 2. Settings (keys + defaults)

Register defaults in `SettingsFile::initialise()` (alongside `native_window`, etc.):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `mcp_bridge_enabled` | bool | `true` | master on/off |
| `mcp_bridge_listen_port` | int | `9000` | inbound UDP (server → bridge) |
| `mcp_bridge_send_port` | int | `19010` | outbound UDP (bridge → server) |

Ports are read **at bridge construction / on explicit "Apply"** — not live-rebound on every keystroke (see §6).

---

## 3. `MCPBridge.{h,cpp}` changes

### 3.1 New public API
```cpp
// status for the GUI
juce::String getStatus() const;        // "connected" | "listening" | "stopped" | "error: <msg>"
bool isConnected() const;              // already exists (socket bound)
int  getListenPort() const;
int  getSendPort() const;

// lifecycle for the toggle
bool start();                          // already exists — add logging
void stop();                           // already exists — add logging

// mark server handshake / activity
void noteServerActivity();             // called on ANY inbound OSC message
```

### 3.2 Internal state
- `juce::int64 lastServerActivity = 0;` (updated in `oscMessageReceived`/`oscBundleReceived`).
- `juce::String statusMessage;` (error text when bind fails).

### 3.3 `start()` — bind + log
```cpp
bool MCPBridge::start() {
    if (active.load()) return true;
    bool recvOk = receiver.connect(listenPort);
    if (recvOk) {
        receiver.addListener(this);
        active.store(true);
        processor->logMessage("MCP bridge listening on UDP " + juce::String(listenPort));
    } else {
        statusMessage = "port " + juce::String(listenPort) + " in use";
        processor->logMessage("MCP bridge: could not bind UDP " + juce::String(listenPort) + " (port in use?)", true);
    }
    sender.connect("127.0.0.1", sendPort);
    startTimer(1000); // heartbeat/timeout if we add connection-lost detection (§3.5)
    return active.load();
}
```

### 3.4 `stop()` — log
```cpp
void MCPBridge::stop() {
    stopTimer();
    if (!active.load()) return;
    receiver.removeListener(this);
    receiver.disconnect();
    sender.disconnect();
    active.store(false);
    processor->logMessage("MCP bridge stopped");
}
```

### 3.5 Server handshake + connection-lost (optional but recommended)
- In `handleBridgeDomain("connect")` (and `handlePdDomain("ping")`): call `noteServerActivity()`, and on the **first** one, `processor->logMessage("MCP server connected")`.
- In `timerCallback()`: if `active` and `(now - lastServerActivity) > 15000`, post `"MCP server connection lost"` (warning) once, then suppress until activity resumes.

### 3.6 `getStatus()`
```
not active            -> "stopped"
active && no activity -> "listening"        (bound, server not seen yet)
active && recent act. -> "connected"
bind failed           -> "error: " + statusMessage
```

---

## 4. `PluginProcessor` wiring

### 4.1 Constructor (line ~195)
Replace the hard-coded construction:
```cpp
auto const enabled = settingsFile->getProperty<bool>("mcp_bridge_enabled"); // default true
auto const listen  = settingsFile->getProperty<int>("mcp_bridge_listen_port"); // default 9000
auto const send    = settingsFile->getProperty<int>("mcp_bridge_send_port");   // default 19010
mcpBridge = std::make_unique<MCPBridge>(this, listen, send);
if (!enabled) mcpBridge->stop();   // constructed but not listening
```

### 4.2 React to toggle/port changes
`PluginProcessor` already runs `settingsFile->startChangeListener()`. Add a `SettingsFileListener` override (or a `Value::Listener` on the two/three keys) that:
```cpp
if (name == "mcp_bridge_enabled") {
    bool on = settingsFile->getProperty<bool>(name);
    if (on) mcpBridge->start(); else mcpBridge->stop();
}
else if (name == "mcp_bridge_listen_port" || name == "mcp_bridge_send_port") {
    // Recreate the bridge with new ports (stop + make_unique again).
    // Log a console note that a reconnect is needed.
}
```
(Check how `PluginProcessor` currently inherits `SettingsFileListener`; if it doesn't, add the base and `valueChanged`/`settingsFileChanged` hook.)

### 4.3 Status getter for the GUI
```cpp
MCPBridge* getMCPBridge() const { return mcpBridge.get(); }   // already exists
```

---

## 5. `AdvancedSettingsPanel` UI

Add a new **"MCP bridge"** section (following the existing `propertiesPanel.addSection` pattern):

```cpp
PropertiesArray mcpProperties;
mcpBridgeEnabled.referTo(settingsFile->getPropertyAsValue("mcp_bridge_enabled"));
mcpBridgeEnabled.addListener(this);
mcpProperties.add(new PropertiesPanel::BoolComponent("Enable MCP bridge", mcpBridgeEnabled, { "Off", "On" }));

mcpListenPort.referTo(settingsFile->getPropertyAsValue("mcp_bridge_listen_port"));
mcpProperties.add(new PropertiesPanel::EditableComponent<int>("Listen port", mcpListenPort, false, 1024, 65535));

mcpSendPort.referTo(settingsFile->getPropertyAsValue("mcp_bridge_send_port"));
mcpProperties.add(new PropertiesPanel::EditableComponent<int>("Send port", mcpSendPort, false, 1024, 65535));

// Status row (read-only label + refresh via Timer or on section open)
mcpProperties.add(new PropertiesPanel::ActionComponent([this]{ refreshMcpStatus(); }, Icons::Refresh, "Refresh status"));

propertiesPanel.addSection("MCP bridge", mcpProperties);
```

Status rendering options (pick one):
- **A. Simple**: a `PropertiesPanel` `EditableComponent`-style read-only text field showing `getStatus()`.
- **B. Indicator dot**: a small coloured `Label`/`Component` (green/amber/grey/red) updated by a `Timer(1000)` polling `processor->getMCPBridge()->getStatus()`.

`valueChanged()` additions:
```cpp
if (v.refersToSameSourceAs(mcpBridgeEnabled)) { /* handled in PluginProcessor listener */ }
if (v.refersToSameSourceAs(mcpListenPort) || v.refersToSameSourceAs(mcpSendPort)) {
    // show hint: "Port changes apply after restarting the bridge / app"
}
```

Note: `AdvancedSettingsPanel` currently receives a `Component* editor` — `PluginEditor` can be obtained via `dynamic_cast<PluginEditor*>(editor)` to reach `getPluginProcessor()->getMCPBridge()`.

---

## 6. Port change semantics (the tricky part)

The server (`mcp-server`) hard-codes `127.0.0.1:9000` (listen) and replies to `19010`. If the user changes the **listen port**, the server keeps knocking on 9000 → breakage.

Recommended approach (least risk):
- **Listen-port change** → show a note: *"The MCP server must be configured to match. Applies after restart."* Keep it a restart-time setting (recreate bridge on next launch), not a live rebind.
- **Send-port change** → safe-ish to rebind live via the existing `sender.disconnect()/connect()` (`handleBridgeDomain("connect")` path), but still recommend restart for consistency.

For a future pass: have the bridge advertise its ports in `/bridge/capabilities` and let the server read them dynamically — this makes ports fully self-coordinating (documented, not built now).

---

## 7. Console integration (how the bridge logs to the GUI)

Path: `MCPBridge` → `processor->logMessage(...)` → PlugData console panel (and onward to the server via `onConsoleMessage`).

Lines to emit:
| Event | Level | Message |
|---|---|---|
| bind OK | info | `MCP bridge listening on UDP 9000 → 19010` |
| bind fail | error | `MCP bridge: UDP 9000 in use — enable a different listen port` |
| first server msg | info | `MCP server connected` |
| timeout | warning | `MCP server connection lost` |
| toggle off | info | `MCP bridge stopped` |

(These appear in the same console panel artists already watch — no new UI surface needed.)

---

## 8. File-by-file change list

**plugdata-core**
| File | Change |
|---|---|
| `Source/Utility/SettingsFile.h/.cpp` | register 3 defaults (`mcp_bridge_enabled`, `mcp_bridge_listen_port`, `mcp_bridge_send_port`) |
| `Source/Pd/MCPBridge.h` | add `getStatus()`, `getListenPort/getSendPort`, `noteServerActivity()`, `lastServerActivity`, `statusMessage` |
| `Source/Pd/MCPBridge.cpp` | `start()`/`stop()` logging, bind-failure handling, handshake log, timeout log, status string |
| `Source/PluginProcessor.cpp` | read settings at construction; react to toggle/port changes via settings listener |
| `Source/PluginProcessor.h` | (if needed) inherit `SettingsFileListener` / add listener plumbing |
| `Source/Dialogs/AdvancedSettingsPanel.h` | add "MCP bridge" section (toggle + ports + status) + `valueChanged` wiring |

**mcp-server (optional, for port flexibility)**
| File | Change |
|---|---|
| `src/transport/osc-client.ts` | read listen/send ports from config (already reads config? verify) instead of hard-coded 9000/19010 |
| docs / README | note the new setting and port coordination |

---

## 9. Edge cases to handle

1. **Port already bound** at startup → bridge enters `error` state, logs it, GUI status shows red; app must not crash.
2. **Toggle off while server is connected** → in-flight OSC messages dropped; server's `verify_connection` should surface "not connected". Confirm no server-side state corruption (identities/caches) — the server already treats tool calls defensively.
3. **Change port, forget to update server** → document + warn in the panel ("applies after restart; server must match").
4. **Two PlugData instances** → second instance's `receiver.connect(9000)` fails → now *visible* as a console error instead of silent.
5. **Standalone vs plugin** — bridge only makes sense in standalone/DAW-with-MCP; the toggle should still show in Advanced (no harm if off).

---

## 10. Verification checklist

- [ ] Default launch: console shows `MCP bridge listening on UDP 9000 → 19010`.
- [ ] Start MCP server: `MCP server connected` appears; `verify_connection` pings OK.
- [ ] Advanced tab shows: `Enable MCP bridge = On`, ports `9000`/`19010`, status `connected`.
- [ ] Toggle off → console `MCP bridge stopped`; server reports "not connected".
- [ ] Toggle back on → `listening` → `connected`.
- [ ] Occupy port 9000 with another process, relaunch → red status + console error, no crash.
- [ ] Full MCP tool smoke test (construct_patch_v2 create/connect/delete, analyze_spectrum) after re-enabling.
- [ ] Port change → warning shown; after restart, bridge binds the new port.

---

## 11. Open decisions for the artist-facing wording

- Section title: "MCP bridge" vs "AI / MCP bridge" (clearer for non-technical artists).
- Status label text: "connected" / "listening" / "stopped" / "port in use" — keep short.
- Whether ports are editable inline or behind a "Change ports…" sub-panel (cleaner, avoids accidental edits).
