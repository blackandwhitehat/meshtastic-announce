# Meshtastic Announcement Module

Remote-controlled periodic announcement broadcaster for Meshtastic nodes.
Designed for unattended solar nodes that need to broadcast messages
(e.g., network migration notices) without any external connection.

## How It Works

**Challenge/response auth — password never appears on the public channel.**

```
You (broadcast):     !ann start
                          │
              ┌───────────┼───────────┐
              ▼           ▼           ▼
          Node "A"    Node "B"    Node "C"
              │           │           │
              ▼           ▼           ▼
          DM to you:  DM to you:  DM to you:
          "Auth       "Auth       "Auth
          required.   required.   required.
          Reply with  Reply with  Reply with
          password"   password"   password"
              │           │           │
You DM each: password  password   password
      (encrypted, channel can't see it)
              │           │           │
              ▼           ▼           ▼
          Verified ✓  Verified ✓  Verified ✓
          "ON ✓"      "ON ✓"      "ON ✓"
```

1. Broadcast `!ann start` — no password, just the command
2. Every announcement node DMs you back asking for the password
3. You reply to each DM with the password (encrypted, private)
4. Each node independently verifies and executes

## Commands

Send these as **broadcast messages** on the primary channel:

| Command | Description |
|---------|-------------|
| `!ann start` | Start broadcasting |
| `!ann stop` | Stop broadcasting |
| `!ann status` | Get current state |
| `!ann msg:New text here` | Change the announcement message |
| `!ann interval:900` | Change interval (seconds, min 60) |

All responses come back as private DMs.
Each node includes its short name in replies so you know which is which.

## Features

- **Password never exposed** — broadcast triggers a challenge,
  password is only sent via encrypted DM
- **One broadcast reaches all nodes** — every node on the mesh
  independently challenges and authenticates
- **Persists across reboots** — settings saved to flash
- **Airtime-aware** — skips broadcast if channel is >25% utilized
- **2-minute auth window** — challenges expire automatically
- **Wrong password = one chance** — challenge consumed on failure

## Installation

### 1. Copy files into firmware source

```bash
cp AnnouncementModule.h  firmware/src/modules/
cp AnnouncementModule.cpp firmware/src/modules/
```

### 2. Register in Modules.cpp

Edit `firmware/src/modules/Modules.cpp`:

```cpp
// Add include at top:
#include "modules/AnnouncementModule.h"

// Add in setupModules() function:
announcementThread = new AnnouncementModuleThread();
```

### 3. Configure

Edit `AnnouncementModule.h` before building:

```cpp
#define ANN_DEFAULT_INTERVAL   (30 * 60)  // seconds between broadcasts
#define ANN_PASSWORD           "changeme"  // shared secret — change this!
#define ANN_AUTH_TIMEOUT_MS    120000     // auth window (2 min)
```

The default message can be changed in `loadConfig()`,
or remotely via the `msg:` command after flashing.

### 4. Build and flash

```bash
cd firmware
platformio run -e tbeam      # T-Beam
platformio run -e heltec-v3  # Heltec V3
platformio run -e rak4631    # RAK WisBlock
# etc.
```

## Security Model

| What | Visible to channel? |
|------|-------------------|
| `!ann start` command | Yes (but no password) |
| Auth challenge from node | No (encrypted DM) |
| Your password reply | No (encrypted DM) |
| Confirmation from node | No (encrypted DM) |
| Broadcast announcements | Yes (that's the point) |

- The public channel only ever sees `!ann <action>` — someone might
  know you're controlling announcement nodes, but they can't execute
  commands without the password
- Password exchange happens entirely over Meshtastic's PKI-encrypted DMs
- Wrong passwords consume the challenge (no brute force)
- Challenges expire after 2 minutes
- No response to unknown commands (module stays hidden)

## License

Same as Meshtastic firmware (GPL-3.0).
