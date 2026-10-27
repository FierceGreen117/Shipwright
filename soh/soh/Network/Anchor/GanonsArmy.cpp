#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/GanonsArmy.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include <ship/Context.h>
#include <ship/window/Window.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Test/z_en_test.h"
#include "src/overlays/actors/ovl_En_Ik/z_en_ik.h"
extern PlayState* gPlayState;
int16_t Entrance_OverrideNextIndex(int16_t nextEntranceIndex);
void func_80077D10(f32* arg0, s16* arg1, Input* input);
}

/**
 * Ganon's Army
 *
 * See GanonsArmy.h for the overview. Packets:
 *
 * GA_ENEMY_STATE  Link -> room       Every frame. Position, pose and health of every shared enemy near Link.
 * GA_CLAIM        Army -> Link       "I want to control enemy <k>".
 * GA_RELEASE      Army -> Link       "I'm done controlling enemy <k>".
 * GA_INPUT        Army -> Link       Every frame while controlling. Stick direction and buttons.
 *
 * Enemies are matched between games by a key built from their actor id and where the scene placed them (home
 * position), which is the same on every game that loads the same scene.
 */

// MARK: - Enemy types

struct GaEnemyType {
    s16 actorId;
    const char* name;
    size_t skelAnimeOffset;
};

static const GaEnemyType sGaEnemyTypes[] = {
    { ACTOR_EN_TEST, "Stalfos", offsetof(EnTest, skelAnime) },
    { ACTOR_EN_IK, "Iron Knuckle", offsetof(EnIk, skelAnime) },
};

static const GaEnemyType* GaGetType(s16 actorId) {
    for (auto& type : sGaEnemyTypes) {
        if (type.actorId == actorId) {
            return &type;
        }
    }
    return nullptr;
}

static const GaEnemyType* GaGetType(Actor* actor) {
    const GaEnemyType* type = GaGetType(actor->id);
    if (type == nullptr) {
        return nullptr;
    }
    // Nabooru's Iron Knuckle (params 0) is a story fight with cutscenes, leave it alone
    if (actor->id == ACTOR_EN_IK && (actor->params & 0xFF) == 0) {
        return nullptr;
    }
    return type;
}

static SkelAnime* GaGetSkelAnime(Actor* actor, const GaEnemyType* type) {
    return (SkelAnime*)((u8*)actor + type->skelAnimeOffset);
}

static const u8 sGaEnemyCategories[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };

template <typename F> static void GaForEachEnemy(F&& callback) {
    if (gPlayState == nullptr) {
        return;
    }
    for (u8 category : sGaEnemyCategories) {
        Actor* actor = gPlayState->actorCtx.actorLists[category].head;
        while (actor != nullptr) {
            Actor* next = actor->next;
            const GaEnemyType* type = GaGetType(actor);
            // init == NULL means the actor has finished initializing, update == NULL means it is being removed
            if (type != nullptr && actor->init == nullptr && actor->update != nullptr) {
                callback(actor, type);
            }
            actor = next;
        }
    }
}

// MARK: - Helpers

static uint32_t GaHashKey(Actor* actor) {
    uint32_t hash = 2166136261u;
    auto mix = [&](int32_t value) {
        for (int i = 0; i < 4; i++) {
            hash ^= (uint32_t)(value >> (i * 8)) & 0xFF;
            hash *= 16777619u;
        }
    };
    mix(actor->id);
    mix((int32_t)lroundf(actor->home.pos.x));
    mix((int32_t)lroundf(actor->home.pos.y));
    mix((int32_t)lroundf(actor->home.pos.z));
    mix(actor->home.rot.y);
    return hash == 0 ? 1 : hash;
}

static uint32_t GaKey(Actor* actor) {
    auto& spawned = Anchor::Instance->gaSpawnedKeys;
    auto it = spawned.find(actor);
    if (it != spawned.end()) {
        return it->second;
    }
    return GaHashKey(actor);
}

static u32 GaFrame() {
    return gPlayState != nullptr ? gPlayState->gameplayFrames : 0;
}

static const char sBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string GaBase64Encode(const u8* data, size_t length) {
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    for (size_t i = 0; i < length; i += 3) {
        u32 chunk = data[i] << 16;
        if (i + 1 < length) {
            chunk |= data[i + 1] << 8;
        }
        if (i + 2 < length) {
            chunk |= data[i + 2];
        }
        out.push_back(sBase64Chars[(chunk >> 18) & 0x3F]);
        out.push_back(sBase64Chars[(chunk >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? sBase64Chars[(chunk >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < length ? sBase64Chars[chunk & 0x3F] : '=');
    }
    return out;
}

static size_t GaBase64Decode(const std::string& in, u8* out, size_t maxLength) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };

    size_t written = 0;
    u32 buffer = 0;
    int bits = 0;
    for (char c : in) {
        int v = value(c);
        if (v < 0) {
            continue;
        }
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written < maxLength) {
                out[written++] = (buffer >> bits) & 0xFF;
            }
        }
    }
    return written;
}

static void GaNotify(const std::string& message) {
    Notification::Emit({
        .prefix = "Ganon's Army",
        .message = message,
    });
}

// MARK: - Roles

u8 Anchor::GetGanonsArmyRole() {
    if (!isConnected) {
        return GANONS_ARMY_ROLE_OFF;
    }
    return (u8)CVarGetInteger(CVAR_REMOTE_ANCHOR("GanonsArmy.Role"), GANONS_ARMY_ROLE_OFF);
}

void Anchor::SetGanonsArmyRole(u8 role) {
    if (gaControlledKey != 0) {
        AnchorClient* link = GetGanonsArmyLink();
        if (link != nullptr) {
            SendPacket_GaRelease(gaControlledKey);
        }
    }

    gaPuppets.clear();
    gaMirrors.clear();
    gaControlledKey = 0;
    gaFollowEntrance = -1;
    GaEndSolo();

    // Give the army player their Link back
    if (role != GANONS_ARMY_ROLE_ARMY && gaSavedPlayerDraw != nullptr && IsSaveLoaded()) {
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr && player->actor.draw == nullptr) {
            player->actor.draw = (ActorFunc)gaSavedPlayerDraw;
        }
    }

    CVarSetInteger(CVAR_REMOTE_ANCHOR("GanonsArmy.Role"), role);
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    SendPacket_UpdateClientState();

    if (role == GANONS_ARMY_ROLE_LINK) {
        GaNotify("You are Link. Ganon's Army is coming for you!");
    } else if (role == GANONS_ARMY_ROLE_ARMY) {
        GaNotify("You joined Ganon's Army.");
    }
}

// The player who is playing as Link (the first one found)
AnchorClient* Anchor::GetGanonsArmyLink() {
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.gaRole == GANONS_ARMY_ROLE_LINK) {
            return &client;
        }
    }
    return nullptr;
}

// MARK: - Link's game: sending enemy state

void Anchor::SendPacket_GaEnemyState() {
    nlohmann::json enemies = nlohmann::json::array();

    GaForEachEnemy([&](Actor* actor, const GaEnemyType* type) {
        uint32_t key = GaKey(actor);
        SkelAnime* skelAnime = GaGetSkelAnime(actor, type);

        nlohmann::json enemy;
        enemy["k"] = key;
        enemy["i"] = actor->id;
        enemy["pa"] = actor->params;
        enemy["p"] = { actor->world.pos.x, actor->world.pos.y, actor->world.pos.z };
        enemy["w"] = actor->world.rot.y;
        enemy["r"] = { actor->shape.rot.x, actor->shape.rot.y, actor->shape.rot.z };
        enemy["sc"] = { actor->scale.x, actor->scale.y, actor->scale.z };
        enemy["h"] = actor->colChkInfo.health;
        enemy["cf"] = { actor->colorFilterTimer, actor->colorFilterParams };
        if (skelAnime->jointTable != nullptr) {
            enemy["j"] = GaBase64Encode((const u8*)skelAnime->jointTable, skelAnime->limbCount * sizeof(Vec3s));
        }

        // Extra values the enemy's drawing code depends on
        if (actor->id == ACTOR_EN_TEST) {
            EnTest* stalfos = (EnTest*)actor;
            enemy["x"] = { stalfos->headRot.x, stalfos->headRot.y, stalfos->headRot.z, stalfos->unk_7C8,
                           stalfos->unk_7DE };
        } else if (actor->id == ACTOR_EN_IK) {
            EnIk* ironKnuckle = (EnIk*)actor;
            enemy["x"] = { ironKnuckle->drawArmorFlag, ironKnuckle->armorStatusFlag, ironKnuckle->unk_2F8,
                           ironKnuckle->unk_2FE };
        }

        auto puppet = gaPuppets.find(key);
        enemy["c"] = puppet != gaPuppets.end() ? puppet->second.clientId : 0;

        enemies.push_back(enemy);
    });

    nlohmann::json payload;
    payload["type"] = GA_ENEMY_STATE;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = gPlayState->roomCtx.curRoom.num;
    payload["enemies"] = enemies;

    SendJsonToRemote(payload);
}

void Anchor::GaLinkFrame() {
    if (!IsSaveLoaded()) {
        return;
    }

    // Only send when someone in Ganon's Army is here to see it
    bool armyHere = false;
    for (auto& [clientId, client] : clients) {
        if (!client.self && client.online && client.gaRole == GANONS_ARMY_ROLE_ARMY &&
            client.sceneNum == gPlayState->sceneNum) {
            armyHere = true;
            break;
        }
    }

    // Let go of enemies that are gone, or whose army player stopped sending input or left
    std::vector<uint32_t> liveKeys;
    GaForEachEnemy([&](Actor* actor, const GaEnemyType* type) { liveKeys.push_back(GaKey(actor)); });

    for (auto it = gaPuppets.begin(); it != gaPuppets.end();) {
        bool alive = std::find(liveKeys.begin(), liveKeys.end(), it->first) != liveKeys.end();
        bool clientOk = clients.contains(it->second.clientId) && clients[it->second.clientId].online &&
                        clients[it->second.clientId].gaRole == GANONS_ARMY_ROLE_ARMY;
        bool talking = (GaFrame() - it->second.lastInputFrame) < 60;
        if (!alive || !clientOk || !talking) {
            it = gaPuppets.erase(it);
        } else {
            ++it;
        }
    }

    if (armyHere) {
        SendPacket_GaEnemyState();
    }
}

// MARK: - Link's game: puppet input

bool Anchor::GaGetPuppetInput(Actor* actor, GanonsArmyPuppetInput* out) {
    // Solo test: our own monster is driven by our own controller
    if (gaSoloMonster != nullptr && actor == gaSoloMonster && gPlayState != nullptr) {
        Input* input = &gPlayState->state.input[0];
        f32 magnitude = 0.0f;
        s16 angle = 0;
        *out = {};
        if (gPlayState->pauseCtx.state == 0 && gPlayState->msgCtx.msgMode == MSGMODE_NONE) {
            func_80077D10(&magnitude, &angle, input);
            out->yaw = (s16)(Camera_GetInputDirYaw(GET_ACTIVE_CAM(gPlayState)) + angle);
            out->mag = magnitude / 60.0f;
            out->held = input->cur.button;
            out->pressed = input->press.button; // Same all frame, like the online version
        }
        return true;
    }

    if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_LINK || gaPuppets.empty()) {
        return false;
    }

    auto it = gaPuppets.find(GaKey(actor));
    if (it == gaPuppets.end()) {
        return false;
    }

    GaPuppet& puppet = it->second;
    if ((GaFrame() - puppet.lastInputFrame) >= 60) {
        return false;
    }

    // Hand out button presses once per frame, no matter how many times the enemy asks
    if (puppet.consumedFrame != GaFrame()) {
        puppet.consumedFrame = GaFrame();
        puppet.input.pressed = puppet.pendingPressed;
        puppet.pendingPressed = 0;
    }

    *out = puppet.input;
    return true;
}

extern "C" s32 GanonsArmy_GetPuppetInput(Actor* actor, GanonsArmyPuppetInput* out) {
    if (Anchor::Instance == nullptr || actor == nullptr || out == nullptr) {
        return false;
    }
    return Anchor::Instance->GaGetPuppetInput(actor, out);
}

void Anchor::HandlePacket_GaClaim(nlohmann::json payload) {
    if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_LINK || !IsSaveLoaded()) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    uint32_t key = payload.at("k").get<uint32_t>();

    if (!clients.contains(clientId) || clients[clientId].gaRole != GANONS_ARMY_ROLE_ARMY) {
        return;
    }

    bool exists = false;
    GaForEachEnemy([&](Actor* actor, const GaEnemyType* type) {
        if (GaKey(actor) == key) {
            exists = true;
        }
    });
    if (!exists) {
        return;
    }

    auto current = gaPuppets.find(key);
    if (current != gaPuppets.end() && current->second.clientId != clientId) {
        return; // Someone else already has it
    }

    // One monster per army player
    for (auto it = gaPuppets.begin(); it != gaPuppets.end();) {
        if (it->second.clientId == clientId && it->first != key) {
            it = gaPuppets.erase(it);
        } else {
            ++it;
        }
    }

    GaPuppet& puppet = gaPuppets[key];
    puppet.clientId = clientId;
    puppet.input = {};
    puppet.pendingPressed = 0;
    puppet.lastInputFrame = GaFrame();
    puppet.consumedFrame = 0xFFFFFFFF;
}

void Anchor::HandlePacket_GaRelease(nlohmann::json payload) {
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    uint32_t key = payload.at("k").get<uint32_t>();

    auto it = gaPuppets.find(key);
    if (it != gaPuppets.end() && it->second.clientId == clientId) {
        gaPuppets.erase(it);
    }
}

void Anchor::HandlePacket_GaInput(nlohmann::json payload) {
    if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_LINK) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    uint32_t key = payload.at("k").get<uint32_t>();

    auto it = gaPuppets.find(key);
    if (it == gaPuppets.end() || it->second.clientId != clientId) {
        return;
    }

    GaPuppet& puppet = it->second;
    puppet.input.yaw = payload.value("y", (s16)0);
    puppet.input.mag = std::clamp(payload.value("m", 0.0f), 0.0f, 1.0f);
    puppet.input.held = payload.value("h", (u16)0);
    puppet.pendingPressed |= payload.value("p", (u16)0);
    puppet.lastInputFrame = GaFrame();
}

// MARK: - Army game: mirroring Link's enemies

void Anchor::HandlePacket_GaEnemyState(nlohmann::json payload) {
    if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_ARMY || !IsSaveLoaded()) {
        return;
    }

    AnchorClient* link = GetGanonsArmyLink();
    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (link == nullptr || link->clientId != clientId) {
        return;
    }

    s16 sceneNum = payload.value("sceneNum", (s16)-1);
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    if (gaMirrorSceneNum != sceneNum) {
        gaMirrors.clear();
        gaMirrorSceneNum = sceneNum;
    }
    gaLinkRoomNum = payload.value("roomNum", (s8)-1);

    // Which shared enemies already exist on this game
    std::unordered_map<uint32_t, Actor*> localEnemies;
    GaForEachEnemy([&](Actor* actor, const GaEnemyType* type) { localEnemies[GaKey(actor)] = actor; });

    static std::unordered_map<uint32_t, u32> sSpawnAttemptFrame;

    for (auto& enemy : payload["enemies"]) {
        uint32_t key = enemy.at("k").get<uint32_t>();
        GaMirror& mirror = gaMirrors[key];
        mirror.state = enemy;
        mirror.receivedFrame = GaFrame();
        mirror.controllerClientId = enemy.value("c", (uint32_t)0);

        if (localEnemies.contains(key)) {
            continue;
        }

        // This game doesn't have that enemy (it was summoned, or its room isn't loaded here), so make a copy of it.
        // Don't retry more than once a second if it fails, e.g. because its graphics aren't loaded here.
        auto attempt = sSpawnAttemptFrame.find(key);
        if (attempt != sSpawnAttemptFrame.end() && (GaFrame() - attempt->second) < 20) {
            continue;
        }
        sSpawnAttemptFrame[key] = GaFrame();

        s16 actorId = enemy.value("i", (s16)0);
        if (GaGetType(actorId) == nullptr) {
            continue;
        }

        s16 params = enemy.value("pa", (s16)0);
        if (actorId == ACTOR_EN_IK) {
            params = (params & 0xFF) | 0x3F00; // Unused switch flag, so the copy isn't removed on spawn
        }

        auto& pos = enemy.at("p");
        auto& rot = enemy.at("r");
        Actor* spawned =
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, pos[0].get<f32>(), pos[1].get<f32>(),
                        pos[2].get<f32>(), rot[0].get<s16>(), rot[1].get<s16>(), rot[2].get<s16>(), params);
        if (spawned != nullptr) {
            gaSpawnedKeys[spawned] = key;
            spawned->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        }
    }
}

// Called instead of the enemy's own update on army games. Copies everything from Link's game.
void Anchor::GaMirrorEnemy(Actor* actor) {
    const GaEnemyType* type = GaGetType(actor);
    if (type == nullptr) {
        return;
    }

    // Keep updating even when off screen, otherwise a copy can get stuck out of view
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;

    auto it = gaMirrors.find(GaKey(actor));
    bool fresh = it != gaMirrors.end() && (GaFrame() - it->second.receivedFrame) < 20;

    if (!fresh) {
        // Link's game doesn't have this enemy (defeated, or not loaded there), so hide it
        actor->world.pos.y = -32000.0f;
        actor->prevPos = actor->world.pos;
        actor->shape.shadowAlpha = 0;
        return;
    }

    const nlohmann::json& state = it->second.state;

    bool wasHidden = actor->world.pos.y < -30000.0f;
    auto& pos = state.at("p");
    actor->world.pos.x = pos[0].get<f32>();
    actor->world.pos.y = pos[1].get<f32>();
    actor->world.pos.z = pos[2].get<f32>();
    if (wasHidden) {
        actor->prevPos = actor->world.pos;
    }
    actor->world.rot.y = state.value("w", (s16)0);

    auto& rot = state.at("r");
    actor->shape.rot.x = rot[0].get<s16>();
    actor->shape.rot.y = rot[1].get<s16>();
    actor->shape.rot.z = rot[2].get<s16>();

    auto& scale = state.at("sc");
    actor->scale.x = scale[0].get<f32>();
    actor->scale.y = scale[1].get<f32>();
    actor->scale.z = scale[2].get<f32>();

    actor->shape.shadowAlpha = 255;
    actor->speedXZ = 0.0f;
    actor->velocity = { 0.0f, 0.0f, 0.0f };
    actor->colChkInfo.health = state.value("h", (u8)1);

    auto& colorFilter = state.at("cf");
    actor->colorFilterTimer = colorFilter[0].get<u8>();
    actor->colorFilterParams = colorFilter[1].get<u16>();

    actor->focus.pos = actor->world.pos;
    actor->focus.pos.y += 45.0f;

    SkelAnime* skelAnime = GaGetSkelAnime(actor, type);
    if (state.contains("j") && skelAnime->jointTable != nullptr) {
        GaBase64Decode(state["j"].get<std::string>(), (u8*)skelAnime->jointTable, skelAnime->limbCount * sizeof(Vec3s));
    }

    if (state.contains("x")) {
        auto& extra = state["x"];
        if (actor->id == ACTOR_EN_TEST && extra.size() >= 5) {
            EnTest* stalfos = (EnTest*)actor;
            stalfos->headRot.x = extra[0].get<s16>();
            stalfos->headRot.y = extra[1].get<s16>();
            stalfos->headRot.z = extra[2].get<s16>();
            stalfos->unk_7C8 = extra[3].get<u8>();
            stalfos->unk_7DE = extra[4].get<u8>();
        } else if (actor->id == ACTOR_EN_IK && extra.size() >= 4) {
            EnIk* ironKnuckle = (EnIk*)actor;
            ironKnuckle->drawArmorFlag = extra[0].get<u8>();
            ironKnuckle->armorStatusFlag = extra[1].get<u8>();
            ironKnuckle->unk_2F8 = extra[2].get<u8>();
            ironKnuckle->unk_2FE = extra[3].get<s8>();
        }
    }
}

// MARK: - Army game: claiming and controlling

void Anchor::SendPacket_GaClaim(uint32_t key) {
    AnchorClient* link = GetGanonsArmyLink();
    if (link == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = GA_CLAIM;
    payload["targetClientId"] = link->clientId;
    payload["k"] = key;
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_GaRelease(uint32_t key) {
    AnchorClient* link = GetGanonsArmyLink();
    if (link == nullptr) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = GA_RELEASE;
    payload["targetClientId"] = link->clientId;
    payload["k"] = key;
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_GaInput() {
    AnchorClient* link = GetGanonsArmyLink();
    if (link == nullptr || gaControlledKey == 0) {
        return;
    }

    Input* input = &gPlayState->state.input[0];
    f32 magnitude = 0.0f;
    s16 angle = 0;
    u16 held = 0;
    u16 pressed = 0;

    // No input while the pause menu or a text box is open
    if (gPlayState->pauseCtx.state == 0 && gPlayState->msgCtx.msgMode == MSGMODE_NONE) {
        func_80077D10(&magnitude, &angle, input);
        held = input->cur.button;
        pressed = input->press.button;
    }

    nlohmann::json payload;
    payload["type"] = GA_INPUT;
    payload["quiet"] = true;
    payload["targetClientId"] = link->clientId;
    payload["k"] = gaControlledKey;
    payload["y"] = (s16)(Camera_GetInputDirYaw(GET_ACTIVE_CAM(gPlayState)) + angle);
    payload["m"] = magnitude / 60.0f;
    payload["h"] = held;
    payload["p"] = pressed;
    SendJsonToRemote(payload);
}

static Actor* GaFindLocalEnemy(uint32_t key) {
    Actor* found = nullptr;
    GaForEachEnemy([&](Actor* actor, const GaEnemyType* type) {
        if (found == nullptr && GaKey(actor) == key) {
            found = actor;
        }
    });
    return found;
}

// MARK: - Solo test (nobody is playing Link)

void Anchor::GaEndSolo() {
    if (gaSoloMonster != nullptr) {
        Actor_Kill(gaSoloMonster);
        gaSoloMonster = nullptr;
    }
    gaSoloMonsterId = 0;
    gaSoloHasSafePos = false;

    if (IsSaveLoaded() && gaSavedPlayerDraw != nullptr) {
        Player* player = GET_PLAYER(gPlayState);
        if (player->actor.draw == nullptr) {
            player->actor.draw = (ActorFunc)gaSavedPlayerDraw;
        }
        player->actor.shape.shadowAlpha = 255;
    }
}

void Anchor::GaSoloFrame() {
    Player* player = GET_PLAYER(gPlayState);

    // Finish loading the room on the other side of a door
    if (gaRoomChangeRequested && gPlayState->roomCtx.status == 0) {
        Room_FinishRoomChange(gPlayState, &gPlayState->roomCtx);
        gaRoomChangeRequested = false;
    }

    s32 wanted = CVarGetInteger(CVAR_REMOTE_ANCHOR("GanonsArmy.Monster"), GANONS_ARMY_MONSTER_ANY);
    s16 wantedId = wanted == GANONS_ARMY_MONSTER_IRON_KNUCKLE ? ACTOR_EN_IK : ACTOR_EN_TEST;
    const char* wantedName = wantedId == ACTOR_EN_IK ? "Iron Knuckle" : "Stalfos";

    // Picked a different monster in the menu: swap
    if (gaSoloMonster != nullptr && gaSoloMonsterId != wantedId) {
        Vec3f pos = gaSoloMonster->world.pos;
        s16 yaw = gaSoloMonster->shape.rot.y;
        Actor_Kill(gaSoloMonster);
        gaSoloMonster = nullptr;
        player->actor.world.pos = pos;
        player->actor.shape.rot.y = yaw;
    }

    // Don't spawn during cutscenes, text or area changes
    bool busy = gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF ||
                gPlayState->msgCtx.msgMode != MSGMODE_NONE || Player_InBlockingCsMode(gPlayState, player);

    if (gaSoloMonster == nullptr) {
        if (busy) {
            return;
        }

        // Stalfos type 2 stands on the ground; Iron Knuckle type 2 is the standing black one.
        // 0x3F is an unused switch flag so the Iron Knuckle isn't removed on spawn.
        s16 params = wantedId == ACTOR_EN_IK ? 0x3F02 : STALFOS_TYPE_2;
        Vec3f spawnPos = gaSoloHasSafePos ? gaSoloSafePos : player->actor.world.pos;
        Actor* monster = Actor_Spawn(&gPlayState->actorCtx, gPlayState, wantedId, spawnPos.x, spawnPos.y, spawnPos.z, 0,
                                     player->actor.shape.rot.y, 0, params);
        if (monster == nullptr) {
            static u32 sLastFailFrame = 0;
            if (GaFrame() - sLastFailFrame > 200) {
                sLastFailFrame = GaFrame();
                GaNotify(std::string("Couldn't turn into a ") + wantedName + " here. Try another area.");
            }
            return;
        }

        monster->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
        monster->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED; // Can't lock on to yourself
        gaSoloMonster = monster;
        gaSoloMonsterId = wantedId;
        GaNotify(std::string("You are a ") + wantedName +
                 (wantedId == ACTOR_EN_IK ? "! B: Slam  A: Swing  R: Block  C-Down: Spin  Z: Camera"
                                          : "! B: Slash  A: Jump Attack  R: Block  C-Down: Dodge  Z: Camera"));
    }

    // Hide Link; the camera follows a point just in front of the monster so it stays on screen
    if (player->actor.draw != nullptr) {
        gaSavedPlayerDraw = (void*)player->actor.draw;
        player->actor.draw = nullptr;
    }
    player->actor.shape.shadowAlpha = 0;
    player->invincibilityTimer = 20;

    Actor* monster = gaSoloMonster;
    monster->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    monster->room = -1; // Keep the monster when moving between rooms

    if ((monster->bgCheckFlags & BGCHECKFLAG_GROUND) && monster->floorPoly != nullptr) {
        gaSoloSafePos = monster->world.pos;
        gaSoloHasSafePos = true;
    }

    if (!busy) {
        GaSoloExitsAndDoors(monster);
    }

    // Iron Knuckles start as a statue; wake ours up straight away
    if (monster->id == ACTOR_EN_IK && monster->init == nullptr) {
        EnIk* ironKnuckle = (EnIk*)monster;
        if (ironKnuckle->unk_2F8 == 3 && ironKnuckle->skelAnime.playSpeed == 0.0f) {
            ironKnuckle->skelAnime.playSpeed = 1.0f;
        }
    }
    s16 yaw = monster->shape.rot.y;
    // Keeping Link a little ahead of the monster also keeps the monster facing where it is going,
    // since its attacks turn it towards Link
    player->actor.world.pos.x = monster->world.pos.x + Math_SinS(yaw) * 60.0f;
    player->actor.world.pos.y = monster->world.pos.y;
    player->actor.world.pos.z = monster->world.pos.z + Math_CosS(yaw) * 60.0f;
    player->actor.prevPos = player->actor.world.pos;
    player->actor.shape.rot.y = player->actor.world.rot.y = yaw;
    player->actor.focus.pos = monster->world.pos;
    player->actor.focus.pos.y += 60.0f;
}

// MARK: - Solo test: loading zones and doors

// Walk into a loading zone (floor or wall) to go to the next area, like Link does
static bool GaTryExit(CollisionPoly* poly, s32 bgId) {
    if (poly == nullptr || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return false;
    }
    s32 exitIndex = SurfaceType_GetSceneExitIndex(&gPlayState->colCtx, poly, bgId);
    if (exitIndex == 0) {
        return false;
    }

    s16 next = gPlayState->setupExitList[exitIndex - 1];
    if (IS_RANDO) {
        next = Entrance_OverrideNextIndex(next);
    }

    if (next == ENTR_RETURN_GROTTO) {
        // Leaving a grotto: back to where we came in
        gSaveContext.respawnFlag = 2;
        gPlayState->nextEntranceIndex = gSaveContext.respawn[RESPAWN_MODE_RETURN].entranceIndex;
        gPlayState->transitionType = TRANS_TYPE_FADE_WHITE;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;
    } else if (next >= ENTR_RETURN_YOUSEI_IZUMI_YOKO) {
        return false; // Fairy fountain / shop "go back where you came from" exits aren't supported yet
    } else {
        gPlayState->nextEntranceIndex = next;
        gSaveContext.retainWeatherMode = 1;
        Scene_SetTransitionForNextEntrance(gPlayState);
    }
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    return true;
}

void Anchor::GaSoloExitsAndDoors(Actor* monster) {
    // Loading zones under the monster or in the wall it is walking into
    if (GaTryExit(monster->floorPoly, monster->floorBgId)) {
        return;
    }
    if ((monster->bgCheckFlags & BGCHECKFLAG_WALL) && GaTryExit(monster->wallPoly, monster->wallBgId)) {
        return;
    }

    if (gaSoloDoorCooldown > 0) {
        gaSoloDoorCooldown--;
        return;
    }

    // Doors: push the stick into a door to go through it
    GanonsArmyPuppetInput in;
    if (!GaGetPuppetInput(monster, &in) || in.mag < 0.5f) {
        return;
    }

    Actor* door = gPlayState->actorCtx.actorLists[ACTORCAT_DOOR].head;
    for (; door != nullptr; door = door->next) {
        f32 dx = door->world.pos.x - monster->world.pos.x;
        f32 dz = door->world.pos.z - monster->world.pos.z;
        f32 distXZ = sqrtf(dx * dx + dz * dz);
        if (distXZ > 75.0f || fabsf(door->world.pos.y - monster->world.pos.y) > 60.0f) {
            continue;
        }
        s16 towardDoor = Math_Vec3f_Yaw(&monster->world.pos, &door->world.pos);
        if (ABS((s16)(towardDoor - in.yaw)) > 0x2000) {
            continue; // Not pushing towards this door
        }

        // Which side of the door are we on? Go out the other side.
        s16 doorYaw = door->shape.rot.y;
        f32 frontX = Math_SinS(doorYaw);
        f32 frontZ = Math_CosS(doorYaw);
        f32 side =
            (monster->world.pos.x - door->world.pos.x) * frontX + (monster->world.pos.z - door->world.pos.z) * frontZ;
        f32 outDir = side >= 0.0f ? -1.0f : 1.0f;
        Vec3f through = door->world.pos;
        through.x += frontX * outDir * 70.0f;
        through.z += frontZ * outDir * 70.0f;

        // A door that leads to another area has a loading zone right behind it
        Vec3f from = monster->world.pos;
        from.y += 30.0f;
        Vec3f to = through;
        to.y += 30.0f;
        Vec3f hit;
        CollisionPoly* poly = nullptr;
        s32 bgId = BGCHECK_SCENE;
        if (BgCheck_EntityLineTest1(&gPlayState->colCtx, &from, &to, &hit, &poly, true, true, true, true, &bgId) &&
            GaTryExit(poly, bgId)) {
            return;
        }

        // A door between rooms of the same area: load the room on the other side
        u16 index = GET_TRANSITION_ACTOR_INDEX(door);
        if (index < gPlayState->transiActorCtx.numActors && gPlayState->transiActorCtx.list[index].id == door->id) {
            TransitionActorEntry* entry = &gPlayState->transiActorCtx.list[index];
            s8 curRoom = gPlayState->roomCtx.curRoom.num;
            s8 newRoom = entry->sides[0].room != curRoom ? entry->sides[0].room : entry->sides[1].room;
            if (newRoom >= 0 && newRoom != curRoom && gPlayState->roomCtx.status == 0) {
                if (Room_RequestNewRoom(gPlayState, &gPlayState->roomCtx, newRoom)) {
                    gaRoomChangeRequested = true;
                }
            }
        }

        monster->world.pos = through;
        monster->prevPos = through;
        monster->shape.rot.y = monster->world.rot.y = outDir > 0.0f ? doorYaw : (s16)(doorYaw + 0x8000);
        monster->speedXZ = 0.0f;
        gaSoloDoorCooldown = 30;
        return;
    }
}

// MARK: - Monster camera

// The monster this player is playing as, if any
Actor* Anchor::GaCameraTarget() {
    if (gaSoloMonster != nullptr && gaSoloMonster->update != nullptr) {
        return gaSoloMonster;
    }
    if (gaControlledKey != 0 && GetGanonsArmyRole() == GANONS_ARMY_ROLE_ARMY) {
        return GaFindLocalEnemy(gaControlledKey);
    }
    return nullptr;
}

void Anchor::GaUpdateCamera(Camera* camera) {
    static bool sActive = false;
    static s16 sYaw = 0;
    static Vec3f sEye;
    static Vec3f sAt;
    static Vec3f sLastPos;
    static s32 sSnapTimer = 0;

    Actor* monster = (camera != nullptr && camera->thisIdx == CAM_ID_MAIN && gPlayState != nullptr &&
                      gPlayState->transitionMode == TRANS_MODE_OFF && gPlayState->csCtx.state == CS_STATE_IDLE)
                         ? GaCameraTarget()
                         : nullptr;
    if (monster == nullptr || monster->world.pos.y < -30000.0f) {
        sActive = false;
        return;
    }

    // Bigger monsters get a camera further back
    bool ironKnuckle = monster->id == ACTOR_EN_IK;
    f32 distance = ironKnuckle ? 440.0f : 380.0f;
    f32 lookHeight = ironKnuckle ? 95.0f : 80.0f;
    f32 eyeHeight = ironKnuckle ? 120.0f : 100.0f;

    Input* input = &gPlayState->state.input[0];
    bool canControl = gPlayState->pauseCtx.state == 0 && gPlayState->msgCtx.msgMode == MSGMODE_NONE;

    Vec3f pos = monster->world.pos;
    bool justStarted = !sActive;
    if (!sActive) {
        sActive = true;
        sYaw = monster->shape.rot.y;
        sLastPos = pos;
        sSnapTimer = 0;
    }

    f32 moved = sqrtf(SQ(pos.x - sLastPos.x) + SQ(pos.z - sLastPos.z));
    sLastPos = pos;
    s16 behind = monster->shape.rot.y;
    s16 diff = behind - sYaw;

    if (canControl && CHECK_BTN_ALL(input->press.button, BTN_Z)) {
        sSnapTimer = 15; // Z: swing the camera behind the monster
    }

    if (sSnapTimer > 0 || (canControl && CHECK_BTN_ALL(input->cur.button, BTN_Z))) {
        sYaw += (s16)(diff * 0.3f);
        if (sSnapTimer > 0) {
            sSnapTimer--;
        }
    } else if (canControl && (input->cur.right_stick_x > 15 || input->cur.right_stick_x < -15)) {
        sYaw -= input->cur.right_stick_x * 12; // Right stick turns the camera
    } else if (moved > 0.5f && ABS(diff) < 0x6800) {
        // Swing round behind the monster as it walks, faster the further it has turned
        f32 rate = ABS(diff) > 0x2000 ? 0.09f : 0.05f;
        sYaw += (s16)(diff * rate);
    }

    // Where the camera wants to be
    Vec3f at = pos;
    at.y += lookHeight;
    Vec3f eye = at;
    eye.x -= Math_SinS(sYaw) * distance;
    eye.z -= Math_CosS(sYaw) * distance;
    eye.y += eyeHeight - lookHeight + 60.0f;

    // Don't go through walls
    Vec3f hit;
    CollisionPoly* poly = nullptr;
    s32 bgId = BGCHECK_SCENE;
    if (BgCheck_CameraLineTest1(&gPlayState->colCtx, &at, &eye, &hit, &poly, true, true, true, -1, &bgId)) {
        eye.x = hit.x + (at.x - hit.x) * 0.08f;
        eye.y = hit.y + (at.y - hit.y) * 0.08f;
        eye.z = hit.z + (at.z - hit.z) * 0.08f;
    }

    // Smooth it out (jump straight there when starting, or after the monster teleports through a door)
    if (justStarted || moved > 60.0f) {
        sAt = at;
        sEye = eye;
    }
    sAt.x += (at.x - sAt.x) * 0.5f;
    sAt.y += (at.y - sAt.y) * 0.3f;
    sAt.z += (at.z - sAt.z) * 0.5f;
    sEye.x += (eye.x - sEye.x) * 0.35f;
    sEye.y += (eye.y - sEye.y) * 0.25f;
    sEye.z += (eye.z - sEye.z) * 0.35f;

    camera->at = sAt;
    camera->eye = sEye;
    camera->eyeNext = sEye;
    camera->dist = distance;
    camera->inputDir.x = 0;
    camera->inputDir.y = sYaw; // So the stick moves the monster relative to this camera
    camera->inputDir.z = 0;
    camera->camDir.y = sYaw;
}

extern "C" void GanonsArmy_UpdateCamera(void* camera) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected) {
        return;
    }
    Anchor::Instance->GaUpdateCamera((Camera*)camera);
}

void Anchor::GaArmyFrame() {
    static u32 sClaimFrame = 0;
    static bool sClaimConfirmed = false;

    if (!IsSaveLoaded()) {
        return;
    }

    AnchorClient* link = GetGanonsArmyLink();
    if (link == nullptr) {
        // Nobody is playing Link: turn into the chosen monster right here to test it
        GaSoloFrame();
        return;
    }
    if (gaSoloMonster != nullptr) {
        GaEndSolo(); // A Link player joined, switch to the real game
    }

    // Follow Link to whatever area they're in
    if (link->sceneNum != gPlayState->sceneNum) {
        if (link->sceneNum < SCENE_ID_MAX && gaFollowEntrance != link->entranceIndex &&
            gPlayState->transitionTrigger == TRANS_TRIGGER_OFF && gPlayState->transitionMode == TRANS_MODE_OFF) {
            gaFollowEntrance = link->entranceIndex;
            gPlayState->nextEntranceIndex = link->entranceIndex;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
        }
        return;
    }
    gaFollowEntrance = -1;

    // Inside dungeons, load the same room Link is in (this player's Link isn't walking through doors)
    RoomContext* roomCtx = &gPlayState->roomCtx;
    if (gaRoomChangeRequested) {
        if (roomCtx->status == 0) {
            Room_FinishRoomChange(gPlayState, roomCtx);
            gaRoomChangeRequested = false;
        }
    } else if (gaLinkRoomNum >= 0 && gaLinkRoomNum < gPlayState->numRooms && gaLinkRoomNum != roomCtx->curRoom.num &&
               roomCtx->status == 0) {
        if (Room_RequestNewRoom(gPlayState, roomCtx, gaLinkRoomNum)) {
            gaRoomChangeRequested = true;
        }
    }

    // Hide this player's own Link; they play as a monster instead
    Player* player = GET_PLAYER(gPlayState);
    if (player->actor.draw != nullptr) {
        gaSavedPlayerDraw = (void*)player->actor.draw;
        player->actor.draw = nullptr;
    }
    player->actor.shape.shadowAlpha = 0;
    player->invincibilityTimer = 20;

    // Is the monster we control still around?
    Actor* controlled = nullptr;
    if (gaControlledKey != 0) {
        auto it = gaMirrors.find(gaControlledKey);
        bool fresh = it != gaMirrors.end() && (GaFrame() - it->second.receivedFrame) < 20;

        if (fresh && it->second.controllerClientId == ownClientId) {
            if (!sClaimConfirmed) {
                sClaimConfirmed = true;
                std::string name = "monster";
                const GaEnemyType* type = GaGetType(it->second.state.value("i", (s16)0));
                if (type != nullptr) {
                    name = type->name;
                }
                GaNotify("You are now a " + name + "! B / A: Attack  R: Block  C-Down: Dodge  Z: Camera");
            }
            controlled = GaFindLocalEnemy(gaControlledKey);
        } else if (sClaimConfirmed || (GaFrame() - sClaimFrame) > 40) {
            GaNotify(sClaimConfirmed ? (fresh ? "You lost control of your monster. Press L to take another."
                                              : "Your monster was defeated! Press L to take another.")
                                     : "Couldn't take that monster. Press L to try another.");
            gaControlledKey = 0;
            sClaimConfirmed = false;
        }
    }

    // L: take over the nearest free monster of the kind picked in the menu (press again for the next one)
    if (CHECK_BTN_ALL(gPlayState->state.input[0].press.button, BTN_L) && gPlayState->pauseCtx.state == 0) {
        s32 wanted = CVarGetInteger(CVAR_REMOTE_ANCHOR("GanonsArmy.Monster"), GANONS_ARMY_MONSTER_ANY);
        s16 wantedId = wanted == GANONS_ARMY_MONSTER_STALFOS        ? ACTOR_EN_TEST
                       : wanted == GANONS_ARMY_MONSTER_IRON_KNUCKLE ? ACTOR_EN_IK
                                                                    : (s16)0;
        const char* wantedName = wantedId == ACTOR_EN_TEST ? "Stalfos"
                                 : wantedId == ACTOR_EN_IK ? "Iron Knuckles"
                                                           : "monsters";

        Vec3f linkPos = link->player != nullptr ? link->player->actor.world.pos : link->posRot.pos;

        std::vector<std::pair<f32, uint32_t>> freeMonsters;
        for (auto& [key, mirror] : gaMirrors) {
            bool fresh = (GaFrame() - mirror.receivedFrame) < 20;
            bool free = mirror.controllerClientId == 0 || mirror.controllerClientId == ownClientId;
            if (!fresh || !free || mirror.state.value("h", 0) <= 0) {
                continue;
            }
            if (wantedId != 0 && mirror.state.value("i", (s16)0) != wantedId) {
                continue;
            }

            f32 dist = 1.0e9f;
            if (mirror.state.contains("p") && mirror.state["p"].is_array() && mirror.state["p"].size() == 3) {
                f32 dx = mirror.state["p"][0].get<f32>() - linkPos.x;
                f32 dy = mirror.state["p"][1].get<f32>() - linkPos.y;
                f32 dz = mirror.state["p"][2].get<f32>() - linkPos.z;
                dist = sqrtf(dx * dx + dy * dy + dz * dz);
            }
            freeMonsters.push_back({ dist, key });
        }
        std::sort(freeMonsters.begin(), freeMonsters.end());

        if (freeMonsters.empty()) {
            GaNotify(std::string("No free ") + wantedName + " near Link right now.");
        } else {
            // Nearest one first; if we already control one, move on to the next nearest
            size_t pick = 0;
            for (size_t i = 0; i < freeMonsters.size(); i++) {
                if (freeMonsters[i].second == gaControlledKey) {
                    pick = (i + 1) % freeMonsters.size();
                    break;
                }
            }
            uint32_t key = freeMonsters[pick].second;
            if (key != gaControlledKey) {
                if (gaControlledKey != 0) {
                    SendPacket_GaRelease(gaControlledKey);
                }
                gaControlledKey = key;
                sClaimFrame = GaFrame();
                sClaimConfirmed = false;
                SendPacket_GaClaim(key);
            }
        }
    }

    if (gaControlledKey != 0) {
        SendPacket_GaInput();
    }

    // Point the camera at our monster, or at Link when we don't have one
    Vec3f target;
    s16 targetYaw;
    if (controlled != nullptr && controlled->world.pos.y > -30000.0f) {
        target = controlled->world.pos;
        targetYaw = controlled->shape.rot.y;
    } else if (link->player != nullptr) {
        target = link->player->actor.world.pos;
        targetYaw = link->player->actor.shape.rot.y;
    } else {
        target = link->posRot.pos;
        targetYaw = link->posRot.rot.y;
    }

    player->actor.world.pos = target;
    player->actor.shape.rot.y = player->actor.world.rot.y = targetYaw;
    player->actor.focus.pos = target;
    player->actor.focus.pos.y += 60.0f;
}

// MARK: - Hooks

void Anchor::RegisterGanonsArmyHooks() {
    // Every frame: Link's game sends enemy state, army games handle controls and the camera
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        switch (GetGanonsArmyRole()) {
            case GANONS_ARMY_ROLE_LINK:
                GaLinkFrame();
                break;
            case GANONS_ARMY_ROLE_ARMY:
                GaArmyFrame();
                break;
        }
    });

    // Army games: their own Link stands still (hidden) while they play as a monster
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_PLAYER, isConnected, [&](void* actorRef, bool* should) {
        if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_ARMY || !IsSaveLoaded()) {
            return;
        }
        AnchorClient* link = GetGanonsArmyLink();
        bool solo = link == nullptr && gaSoloMonster != nullptr;
        if (!solo && (link == nullptr || link->sceneNum != gPlayState->sceneNum)) {
            return;
        }

        *should = false;

        // Things Anchor normally does during Link's update
        if (justLoadedSave) {
            justLoadedSave = false;
            SendPacket_RequestTeamState();
        }
        if (shouldRefreshActors) {
            shouldRefreshActors = false;
            RefreshClientActors();
        }
    });

    // Army games: shared enemies copy Link's game instead of thinking for themselves
    for (auto& type : sGaEnemyTypes) {
        s16 actorId = type.actorId;
        auto mirrorHook = [&, actorId](void* actorRef, bool* should) {
            if (GetGanonsArmyRole() != GANONS_ARMY_ROLE_ARMY || !IsSaveLoaded()) {
                return;
            }
            AnchorClient* link = GetGanonsArmyLink();
            if (link == nullptr || link->sceneNum != gPlayState->sceneNum) {
                return;
            }
            Actor* actor = (Actor*)actorRef;
            if (GaGetType(actor) == nullptr) {
                return;
            }
            *should = false;
            try {
                GaMirrorEnemy(actor);
            } catch (const std::exception& e) { SPDLOG_ERROR("[Anchor] Ganon's Army: bad enemy state {}", e.what()); }
        };

        // COND_ID_HOOK uses one static hook id per call site, so register each type by hand
        static std::unordered_map<s16, HOOK_ID> sMirrorHookIds;
        static std::unordered_map<s16, HOOK_ID> sDestroyHookIds;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::ShouldActorUpdate>(sMirrorHookIds[actorId]);
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnActorDestroy>(sDestroyHookIds[actorId]);
        sMirrorHookIds[actorId] = 0;
        sDestroyHookIds[actorId] = 0;
        if (isConnected) {
            sMirrorHookIds[actorId] =
                GameInteractor::Instance->RegisterGameHookForID<GameInteractor::ShouldActorUpdate>(actorId, mirrorHook);
            sDestroyHookIds[actorId] = GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnActorDestroy>(
                actorId, [&](void* actorRef) {
                    gaSpawnedKeys.erase((Actor*)actorRef);
                    if ((Actor*)actorRef == gaSoloMonster) {
                        gaSoloMonster = nullptr; // Respawned next frame
                    }
                });
        }
    }

    // New area: copies we spawned are gone, and Link's game will resend everything
    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        gaSpawnedKeys.clear();
        gaMirrors.clear();
        gaControlledKey = 0;
        gaLinkRoomNum = -1;
        gaRoomChangeRequested = false;
        gaSoloMonster = nullptr;  // The old area's actors are gone; a new monster spawns next frame
        gaSoloHasSafePos = false; // Start where Link enters the new area
        if (GetGanonsArmyRole() == GANONS_ARMY_ROLE_LINK) {
            gaPuppets.clear();
        }
    });
}
