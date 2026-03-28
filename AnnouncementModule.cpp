/**
 * @file AnnouncementModule.cpp
 * @brief Remote-controlled periodic announcement module for Meshtastic.
 *
 * Uses challenge/response auth so the password is never sent in public:
 *   1. User broadcasts: "!ann start" (no password, visible to all)
 *   2. Each node DMs the user: "Auth required. Reply with password."
 *   3. User DMs password back (encrypted, channel can't see it)
 *   4. Node verifies and executes the command
 *
 * All state persists in flash.
 */

#include "AnnouncementModule.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "Router.h"
#include "airtime.h"
#include "configuration.h"
#include <Arduino.h>

AnnouncementModuleThread *announcementThread = nullptr;
AnnouncementModuleRadio *announcementRadio = nullptr;

static const char *ANN_CONFIG_FILE = "/prefs/announce.dat";

// ============================================================
// Config persistence
// ============================================================

void AnnouncementModuleThread::loadConfig()
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = false;
    cfg.intervalSec = ANN_DEFAULT_INTERVAL;
    strncpy(cfg.message,
            "JAXMesh is switching to MediumFast. "
            "Update your modem preset to stay connected.",
            ANN_MAX_MESSAGE_LEN);
    memset(pending, 0, sizeof(pending));

#ifdef FSCom
    if (FSCom.exists(ANN_CONFIG_FILE)) {
        File f = FSCom.open(ANN_CONFIG_FILE, FILE_READ);
        if (f) {
            AnnouncementConfig loaded;
            size_t bytesRead = f.read((uint8_t *)&loaded, sizeof(loaded));
            f.close();
            if (bytesRead == sizeof(loaded) && loaded.magic == ANN_CONFIG_MAGIC) {
                memcpy(&cfg, &loaded, sizeof(cfg));
                LOG_INFO("Announcement config loaded (enabled=%d, interval=%ds)",
                         cfg.enabled, cfg.intervalSec);
                return;
            }
        }
    }
#endif
    LOG_INFO("Announcement config: using defaults");
}

void AnnouncementModuleThread::saveConfig()
{
#ifdef FSCom
    cfg.magic = ANN_CONFIG_MAGIC;
    FSCom.mkdir("/prefs");
    File f = FSCom.open(ANN_CONFIG_FILE, FILE_WRITE);
    if (f) {
        f.write((uint8_t *)&cfg, sizeof(cfg));
        f.close();
        LOG_INFO("Announcement config saved");
    }
#endif
}

// ============================================================
// Pending auth helpers
// ============================================================

void AnnouncementModuleThread::clearExpired()
{
    uint32_t now = millis();
    for (int i = 0; i < ANN_MAX_PENDING; i++) {
        if (pending[i].active && now > pending[i].expiry) {
            LOG_INFO("Announcement: auth expired for 0x%x", pending[i].from);
            pending[i].active = false;
        }
    }
}

PendingAuth *AnnouncementModuleThread::findPending(NodeNum from)
{
    for (int i = 0; i < ANN_MAX_PENDING; i++) {
        if (pending[i].active && pending[i].from == from)
            return &pending[i];
    }
    return nullptr;
}

PendingAuth *AnnouncementModuleThread::allocPending()
{
    clearExpired();
    // Reuse existing or find empty slot
    for (int i = 0; i < ANN_MAX_PENDING; i++) {
        if (!pending[i].active)
            return &pending[i];
    }
    // All full — evict oldest
    return &pending[0];
}

// ============================================================
// Thread
// ============================================================

AnnouncementModuleThread::AnnouncementModuleThread()
    : concurrency::OSThread("Announce")
{
}

int32_t AnnouncementModuleThread::runOnce()
{
    if (firstTime) {
        firstTime = false;
        loadConfig();
        announcementRadio = new AnnouncementModuleRadio();
        LOG_INFO("Announcement Module initialized (enabled=%d)", cfg.enabled);

        if (cfg.enabled) {
            return 15000;
        }
        return 30000;
    }

    // Clean up expired auth challenges
    clearExpired();

    if (!cfg.enabled) {
        return 30000;
    }

    if (airTime->isTxAllowedChannelUtil(true)) {
        sendAnnouncement();
    } else {
        LOG_WARN("Announcement skipped — channel busy");
        return 60000;
    }

    return cfg.intervalSec * 1000;
}

// ============================================================
// Sending
// ============================================================

void AnnouncementModuleThread::sendAnnouncement()
{
    meshtastic_MeshPacket *p = announcementRadio->allocDataPacket();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    p->want_ack = false;
    p->decoded.want_response = false;
    p->hop_limit = 7;

    size_t len = strlen(cfg.message);
    if (len > MAX_LORA_PAYLOAD_LEN)
        len = MAX_LORA_PAYLOAD_LEN;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, cfg.message, len);

    service->sendToMesh(p);
    powerFSM.trigger(EVENT_CONTACT_FROM_PHONE);
    LOG_INFO("Announcement sent: \"%s\"", cfg.message);
}

void AnnouncementModuleThread::sendDM(NodeNum to, const char *text)
{
    meshtastic_MeshPacket *p = announcementRadio->allocDataPacket();
    p->to = to;
    p->channel = 0;
    p->want_ack = true;  // DMs should be acked
    p->decoded.want_response = false;

    size_t len = strlen(text);
    if (len > MAX_LORA_PAYLOAD_LEN)
        len = MAX_LORA_PAYLOAD_LEN;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);

    service->sendToMesh(p);
}

// ============================================================
// Step 1: Handle broadcast command (public, no password)
// ============================================================

void AnnouncementModuleThread::handleBroadcastCommand(const meshtastic_MeshPacket &mp)
{
    auto &payload = mp.decoded.payload;
    char buf[MAX_LORA_PAYLOAD_LEN + 1];
    size_t len = payload.size;
    if (len > MAX_LORA_PAYLOAD_LEN) len = MAX_LORA_PAYLOAD_LEN;
    memcpy(buf, payload.bytes, len);
    buf[len] = '\0';

    // Must start with "!ann "
    if (strncmp(buf, ANN_CMD_PREFIX, strlen(ANN_CMD_PREFIX)) != 0)
        return;

    const char *action = buf + strlen(ANN_CMD_PREFIX);

    // Validate it's a known action before challenging
    if (strncmp(action, "start", 5) != 0 &&
        strncmp(action, "stop", 4) != 0 &&
        strncmp(action, "status", 6) != 0 &&
        strncmp(action, "msg:", 4) != 0 &&
        strncmp(action, "interval:", 9) != 0) {
        return; // unknown command, ignore silently
    }

    // Store pending auth challenge
    PendingAuth *pa = allocPending();
    pa->from = mp.from;
    pa->expiry = millis() + ANN_AUTH_TIMEOUT_MS;
    pa->active = true;
    strncpy(pa->action, action, sizeof(pa->action) - 1);
    pa->action[sizeof(pa->action) - 1] = '\0';

    // DM the sender asking for password (encrypted, private)
    char challenge[120];
    meshtastic_NodeInfoLite *myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    const char *myName = myNode ? myNode->user.short_name : "node";
    snprintf(challenge, sizeof(challenge),
             "[%s] Auth required for '%s'. Reply with password (2 min).",
             myName, action);
    sendDM(mp.from, challenge);

    LOG_INFO("Announcement: auth challenge sent to 0x%x for '%s'", mp.from, action);
}

// ============================================================
// Step 2: Handle DM response (private, contains password)
// ============================================================

void AnnouncementModuleThread::handleDMResponse(const meshtastic_MeshPacket &mp)
{
    // Only process DMs addressed to us
    if (mp.to != nodeDB->getNodeNum())
        return;

    // Check if this sender has a pending challenge
    PendingAuth *pa = findPending(mp.from);
    if (!pa)
        return; // no pending challenge for this node

    auto &payload = mp.decoded.payload;
    char buf[MAX_LORA_PAYLOAD_LEN + 1];
    size_t len = payload.size;
    if (len > MAX_LORA_PAYLOAD_LEN) len = MAX_LORA_PAYLOAD_LEN;
    memcpy(buf, payload.bytes, len);
    buf[len] = '\0';

    // Trim whitespace
    char *pw = buf;
    while (*pw == ' ') pw++;
    char *end = pw + strlen(pw) - 1;
    while (end > pw && *end == ' ') { *end = '\0'; end--; }

    // Ignore if it looks like another !ann command (not a password reply)
    if (strncmp(pw, "!ann", 4) == 0)
        return;

    // Verify password
    if (strcmp(pw, ANN_PASSWORD) != 0) {
        sendDM(mp.from, "[ANN] Wrong password.");
        pa->active = false;
        LOG_WARN("Announcement: wrong password from 0x%x", mp.from);
        return;
    }

    // Password correct — execute the stored action
    char action[80];
    strncpy(action, pa->action, sizeof(action));
    pa->active = false; // consume the challenge

    executeAction(mp.from, action);
}

// ============================================================
// Execute verified command
// ============================================================

void AnnouncementModuleThread::executeAction(NodeNum from, const char *action)
{
    char reply[200];
    meshtastic_NodeInfoLite *myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
    const char *myName = myNode ? myNode->user.short_name : "node";

    if (strncmp(action, "start", 5) == 0) {
        cfg.enabled = true;
        saveConfig();
        snprintf(reply, sizeof(reply),
                 "[%s] Broadcasting ON (every %ds)", myName, cfg.intervalSec);
        sendDM(from, reply);
        LOG_INFO("Announcement: STARTED by 0x%x", from);

    } else if (strncmp(action, "stop", 4) == 0) {
        cfg.enabled = false;
        saveConfig();
        snprintf(reply, sizeof(reply), "[%s] Broadcasting OFF.", myName);
        sendDM(from, reply);
        LOG_INFO("Announcement: STOPPED by 0x%x", from);

    } else if (strncmp(action, "status", 6) == 0) {
        snprintf(reply, sizeof(reply),
                 "[%s] %s | every %ds | msg: %.80s",
                 myName,
                 cfg.enabled ? "ACTIVE" : "STOPPED",
                 cfg.intervalSec,
                 cfg.message);
        sendDM(from, reply);

    } else if (strncmp(action, "msg:", 4) == 0) {
        const char *newMsg = action + 4;
        if (strlen(newMsg) == 0) {
            sendDM(from, "[ANN] Error: empty message");
            return;
        }
        strncpy(cfg.message, newMsg, ANN_MAX_MESSAGE_LEN);
        cfg.message[ANN_MAX_MESSAGE_LEN] = '\0';
        saveConfig();
        snprintf(reply, sizeof(reply),
                 "[%s] Message updated: %.120s", myName, cfg.message);
        sendDM(from, reply);
        LOG_INFO("Announcement: message changed by 0x%x", from);

    } else if (strncmp(action, "interval:", 9) == 0) {
        uint32_t newInterval = atoi(action + 9);
        if (newInterval < ANN_MIN_INTERVAL) {
            snprintf(reply, sizeof(reply),
                     "[ANN] Minimum interval is %ds", ANN_MIN_INTERVAL);
            sendDM(from, reply);
            return;
        }
        cfg.intervalSec = newInterval;
        saveConfig();
        snprintf(reply, sizeof(reply),
                 "[%s] Interval set to %ds", myName, cfg.intervalSec);
        sendDM(from, reply);
        LOG_INFO("Announcement: interval=%ds by 0x%x", newInterval, from);
    }
}

// ============================================================
// Radio listener
// ============================================================

bool AnnouncementModuleRadio::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

ProcessMessage AnnouncementModuleRadio::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!announcementThread)
        return ProcessMessage::CONTINUE;

    // Is this a broadcast with our command prefix?
    if (mp.to == NODENUM_BROADCAST &&
        mp.decoded.payload.size >= strlen(ANN_CMD_PREFIX) &&
        memcmp(mp.decoded.payload.bytes, ANN_CMD_PREFIX,
               strlen(ANN_CMD_PREFIX)) == 0) {
        announcementThread->handleBroadcastCommand(mp);
    }
    // Is this a DM to us? Could be a password response.
    else if (mp.to == nodeDB->getNodeNum() && mp.from != nodeDB->getNodeNum()) {
        announcementThread->handleDMResponse(mp);
    }

    return ProcessMessage::CONTINUE;
}
