#pragma once

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include <Arduino.h>

// ============================================================
// AnnouncementModule — Remote-controlled periodic announcements
//
// Broadcasts a configurable message on the primary text channel.
// Controlled via mesh with challenge/response authentication:
//
// 1. Send broadcast:  !ann start (or stop, status, msg, interval)
// 2. Each node DMs you back asking for password
// 3. You DM the password back — encrypted, invisible to channel
// 4. Node verifies and executes
//
// All settings persist in flash (survives reboot).
// ============================================================

#define ANN_MAX_MESSAGE_LEN    200
#define ANN_DEFAULT_INTERVAL   (30 * 60)  // 30 minutes in seconds
#define ANN_MIN_INTERVAL       60         // 1 minute minimum
#define ANN_CMD_PREFIX         "!ann "
#define ANN_PASSWORD           "changeme"  // <-- CHANGE THIS to your mesh network password
#define ANN_AUTH_TIMEOUT_MS    120000     // 2 minutes to reply with password
#define ANN_MAX_PENDING        4          // max concurrent auth challenges

// Persistent config stored in flash
struct AnnouncementConfig {
    bool enabled;
    uint32_t intervalSec;
    char message[ANN_MAX_MESSAGE_LEN + 1];
    uint32_t magic;  // validation marker
};

#define ANN_CONFIG_MAGIC 0x4A415821  // "JAX!"

// Pending auth challenge
struct PendingAuth {
    NodeNum from;
    uint32_t expiry;       // millis() deadline
    char action[80];       // the command to execute after auth
    bool active;
};

class AnnouncementModuleThread : private concurrency::OSThread
{
    bool firstTime = true;

  public:
    AnnouncementModuleThread();

    // State
    AnnouncementConfig cfg;
    PendingAuth pending[ANN_MAX_PENDING];

    void loadConfig();
    void saveConfig();

    void sendAnnouncement();
    void sendDM(NodeNum to, const char *text);

    // Command flow
    void handleBroadcastCommand(const meshtastic_MeshPacket &mp);
    void handleDMResponse(const meshtastic_MeshPacket &mp);
    void executeAction(NodeNum from, const char *action);

    // Pending auth helpers
    PendingAuth *findPending(NodeNum from);
    PendingAuth *allocPending();
    void clearExpired();

  protected:
    virtual int32_t runOnce() override;
};

class AnnouncementModuleRadio : public SinglePortModule
{
  public:
    AnnouncementModuleRadio()
        : SinglePortModule("announce", meshtastic_PortNum_TEXT_MESSAGE_APP)
    {
        isPromiscuous = true;
    }

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
};

extern AnnouncementModuleThread *announcementThread;
extern AnnouncementModuleRadio *announcementRadio;
