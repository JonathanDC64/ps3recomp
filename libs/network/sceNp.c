/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv (NP_FIRE_CB experiment) */
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, username, SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

/* Build a fake NP ID from the current username */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, s_fake_username);

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    *region = 0x5553;

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    *age = 25; /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> %d\n", userId, *age);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    langs->language1 = SCE_NP_LANG_ENGLISH;
    langs->language2 = 0;
    langs->language3 = 0;

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;
static u32                  s_npmgr_cb_opd = 0;   /* raw guest OPD of the callback */
static u32                  s_npmgr_cb_arg_ea = 0;/* raw guest EA of the user arg */

/* On real NP, after a game registers its manager callback the firmware delivers
 * an initial status event so the game's NP state machine leaves "initializing"
 * and settles (offline, signed-in-but-disconnected, etc.). We never fired it, so
 * event-driven titles (DeS polls sceNpManagerGetNpId/GetStatus every frame but
 * only ADVANCES on the callback event) stall before the title screen. Fire the
 * callback once with the OFFLINE status. Flag-gated (NP_FIRE_CB) so it can't
 * regress the working boot until proven. Invoked from GetStatus (the game's NP
 * poll thread = a valid guest context), one-shot, re-entrancy guarded. */
static void np_fire_manager_cb_once(void)
{
    static int fired = 0, in_fire = 0;
    if (fired || in_fire) return;
    if (!s_npmgr_cb_opd) return;
    if (getenv("NP_NO_FIRE_CB")) return;   /* fire by default; env disables */
    extern uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
    in_fire = 1; fired = 1;
    fprintf(stderr, "[sceNp] firing manager callback opd=0x%08X event=OFFLINE arg=0x%08X\n",
            s_npmgr_cb_opd, s_npmgr_cb_arg_ea);
    ppu_guest_call(s_npmgr_cb_opd, (uint64_t)(int64_t)SCE_NP_MANAGER_STATUS_OFFLINE,
                   0, (uint64_t)s_npmgr_cb_arg_ea, 0);
    in_fire = 0;
}

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    *status = SCE_NP_MANAGER_STATUS_OFFLINE;   /* -1, endian-safe */
    printf("[sceNp] ManagerGetStatus() -> OFFLINE\n");
    np_fire_manager_cb_once();
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    /* Stored for bookkeeping; we never transition online so never fire it. */
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    s_npmgr_cb_opd    = (u32)(size_t)callback;   /* raw guest OPD */
    s_npmgr_cb_arg_ea = (u32)(size_t)arg;
    printf("[sceNp] ManagerRegisterCallback(opd=0x%08X arg=0x%08X)\n",
           s_npmgr_cb_opd, s_npmgr_cb_arg_ea);
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
/* The engine's NexusRevolution Main thread polls GetNpId/GetOnlineName while the
 * NP state machine settles. On real HW the sceNpManager status callback fires
 * (via the sysutil callback pump) with the connection result; without it the
 * poll never concludes and the boot never advances to the NP2 teardown / save-
 * data (auto-save dialog) phase. Deliver the (offline) status the first time the
 * engine polls, so its state machine concludes and proceeds. */
s32 sceNpManagerGetNpId(SceNpId* npId)               { np_fire_manager_cb_once(); return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { np_fire_manager_cb_once(); return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }
