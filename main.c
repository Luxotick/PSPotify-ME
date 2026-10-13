#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspaudiocodec.h>
#include <psptypes.h>
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspsysmem.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_avmodules.h>
#include <psputility_modules.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include "spotify/config.h"
#include "spotify/http.h"
#include "spotify/log.h"
#include "spotify/memory.h"
#include "spotify/device_policy.h"
#include "spotify/player.h"
#include "spotify/ui.h"
#include <me-safe-task/me-stask.h>
#include <me-safe-task/me-stask-kcall.h>
#include "common.h"

PSP_MODULE_INFO("PSPotify ME", 0, 2, 0);
/* Heap = all free user memory except 8 MB. A fixed 24 MB heap only fit
 * with ARK's "highmem:forcemax"; on a default 24 MB user partition it
 * crashed before main. The 8 MB covers what is allocated outside the heap
 * after start-up (net/AV modules ~0.6 MB measured, thread stacks, kcall).
 * The SFO asks for extended memory (MEMSIZE 1) where the firmware allows.
 * libcglue ignores the magnitude of a negative heap size: any negative
 * value means "max free memory minus the threshold", and the threshold
 * defaults to 512 KB (which left 82 KB after network start on hardware). */
PSP_HEAP_SIZE_KB(-1);
PSP_HEAP_THRESHOLD_SIZE_KB(8192);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
#define TIMEOUT_US 2000000u
#define FAIL_TARGET -1001
#define FAIL_MEMORY -1002
#define FAIL_PATH -1008
_Static_assert(ME_CORE_T2_IMG_TABLE == 2 && ME_CORE_IMG_TABLE == 3,
               "Device policy requires the audited pinned table IDs");

// Static application system RAM survives every asynchronous job.
static SharedTask shared_task;
static unsigned long codec_data[64] __attribute__((aligned(64)));
static int dispatcher_ready, av_loaded, network_ready, unsafe_to_exit, power_locked;
static volatile int exit_requested;
typedef struct DeviceInfo {
    int model;
    uint32_t firmware;
    int table;
    uint32_t witness;
    int witness_available, table_available;
    /* Original ME image words at the patch sites and mapped functions,
     * read before any patching, to compare firmware versions from logs. */
    uint32_t probe_addr[16], probe_word[16][2];
    int probe_count;
} DeviceInfo;
static DeviceInfo device = { .model = -1, .table = -1 };
static void report_return(const char *operation, int ret);

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    exit_requested = 1; // HOME uses the same checked teardown as START.
    return 0;
}

/* Absolute application directory, e.g. ms0:/PSP/GAME/PSPOTIFY. */
static char app_directory[256];

static int set_application_directory(int argc, char **argv)
{
    char *directory = app_directory;
    if (argc < 1 || argv == NULL || argv[0] == NULL) return FAIL_PATH;
    const char *slash = strrchr(argv[0], '/');
    if (slash == NULL) return FAIL_PATH;
    const size_t length = (size_t)(slash - argv[0]);
    if (length == 0 || length >= sizeof app_directory) return FAIL_PATH;
    memcpy(directory, argv[0], length);
    directory[length] = '\0';
    // The embedded bridge writes ./kcall.prx; never depend on launcher cwd.
    return sceIoChdir(directory);
}

static int network_probe(void)
{
    int ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (ret < 0 && ret != (int)0x80110801) return ret;
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (ret < 0 && ret != (int)0x80110801) return ret;
    ret = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetInetInit();
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetResolverInit();
    if (ret < 0 && ret != (int)0x80410201) return ret;
    ret = sceNetApctlInit(0x1800, 0x30);
    if (ret < 0 && ret != (int)0x80410201) return ret;
    network_ready = 1;
    memory_log("post-modules");

    if (sceWlanGetSwitchState() == 0) {
        log_printf("WARNING: WLAN switch is physically OFF!\n");
    }

    int valid_profiles[16];
    int valid_count = 0;

    for (int i = 1; i <= 10; ++i) {
        if (sceUtilityCheckNetParam(i) == 0) {
            valid_profiles[valid_count++] = i;
        }
    }
    if (valid_count == 0 && sceUtilityCheckNetParam(0) == 0) {
        valid_profiles[valid_count++] = 0;
    }

    if (valid_count == 0) {
        log_printf("Wi-Fi: No profile found in PSP settings!\n");
        return -112;
    }

    int connected_profile = -1;
    for (int p = 0; p < valid_count; ++p) {
        int prof = valid_profiles[p];
        ret = sceNetApctlConnect(prof);
        if (ret == 0) {
            connected_profile = prof;
            break;
        }
    }

    if (connected_profile < 0) {
        log_printf("Wi-Fi: Connect failed!\n");
        return ret;
    }

    log_printf("Wi-Fi: Connecting (Profile %d)...", connected_profile);

    for (unsigned int attempt = 0; attempt < 300u; ++attempt) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        ret = sceNetApctlGetState(&state);
        if (ret < 0) return ret;

        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof info);
            if (sceNetApctlGetInfo(8, &info) >= 0) {
                log_printf(" OK! (IP: %s)\n", info.ip);
            } else {
                log_printf(" OK!\n");
            }
            return 0;
        }
        if (state == PSP_NET_APCTL_STATE_DISCONNECTED && attempt > 40u) {
            log_printf(" Disconnected\n");
            return -111;
        }
        sceKernelDelayThread(100000);
    }
    log_printf(" Timeout\n");
    return -110;
}

// These callbacks run on SC in kernel mode through the embedded bridge PRX.
/* Classic init, dispatch, decode and restoration; no table selection
 * or writes during this check. See the pinned-source audit. */
static const unsigned int required_mappings[] = {43, 44, 48, 66, 67};
/* Patch sites hardcoded in the pinned safe-task (me-stask.c init()). */
static const uint32_t t2_patch_sites[] = {0x8832162cu, 0x8838c780u, 0x8834f3b0u};
static const uint32_t og_patch_sites[] = {0x88312f4cu, 0x8837b1c0u, 0x88365038u};
#define SYSCALL_ROUTINE_PATCH_SITE 0x883004bcu
static void probe_me_word(DeviceInfo *info, uint32_t addr)
{
    if (info->probe_count >= (int)(sizeof info->probe_addr / sizeof info->probe_addr[0])) return;
    info->probe_addr[info->probe_count] = addr;
    info->probe_word[info->probe_count][0] = hw(addr);
    info->probe_word[info->probe_count][1] = hw(addr + 4u);
    info->probe_count++;
}
static int inspect_device(void *param)
{
    DeviceInfo *info = param;
    info->firmware = (uint32_t)sceKernelDevkitVersion();
    info->witness = hw(ME_CORE_KERNEL_ADDR | 0x18u);
    info->witness_available = 1;
    info->table = meCoreGetTableIdFromWitnessWord();
    info->table_available = 1;
    const uint32_t *sites = NULL;
    const unsigned int *mapping = NULL;
    if (info->table == ME_CORE_T2_IMG_TABLE) { sites = t2_patch_sites; mapping = t2ImgTable; }
    else if (info->table == ME_CORE_IMG_TABLE) { sites = og_patch_sites; mapping = ogImgTable; }
    if (sites == NULL) return 0;
    probe_me_word(info, SYSCALL_ROUTINE_PATCH_SITE);
    for (int i = 0; i < 3; ++i) probe_me_word(info, sites[i]);
    for (size_t i = 0; i < sizeof required_mappings / sizeof required_mappings[0]; ++i) {
        unsigned int offset = mapping[required_mappings[i]];
        if (offset && !(offset & 3u) && offset < 0x00100000u)
            probe_me_word(info, ME_CORE_KERNEL_ADDR + offset);
    }
    return 0;
}
static int reboot_device(void *unused)
{
    (void)unused;
    return scePowerRequestColdReset(0);
}
static const char *generation(int model)
{
    switch (model) {
    case 0: return "01g";
    case 1: return "02g";
    case 2: return "03g";
    case 3: return "04g";
    case 4: return "05g";
    case 6: return "07g";
    case 8: return "09g";
    default: return "unknown";
    }
}
static const char *table_name(int table)
{
    switch (table) {
    case ME_CORE_BL_IMG_TABLE: return "ME_CORE_BL_IMG_TABLE";
    case ME_CORE_SD_IMG_TABLE: return "ME_CORE_SD_IMG_TABLE";
    case ME_CORE_T2_IMG_TABLE: return "ME_CORE_T2_IMG_TABLE";
    case ME_CORE_IMG_TABLE: return "ME_CORE_IMG_TABLE";
    default: return "UNKNOWN";
    }
}
static int mappings_complete(int table)
{
    const unsigned int *mapping;
    if (table == ME_CORE_T2_IMG_TABLE) mapping = t2ImgTable;
    else if (table == ME_CORE_IMG_TABLE) mapping = ogImgTable;
    else return 0;
    for (size_t i = 0; i < sizeof required_mappings / sizeof required_mappings[0]; ++i) {
        unsigned int offset = mapping[required_mappings[i]];
        if (!offset || (offset & 3u) || offset >= 0x00100000u) return 0;
    }
    return 1;
}
static void report_return(const char *operation, int ret)
{
    log_printf("%s: 0x%08X (%d)\n", operation, (unsigned int)ret, ret);
}
static int initialize(void)
{
    sceKernelDcacheWritebackAll();
    /* Firmware can still be logged if loading the model bridge fails. */
    device.firmware = (uint32_t)sceKernelDevkitVersion();
    int ret = meSafeTaskGetModel();
    if (ret < 0) report_return("inspect model/bridge", ret);
    else {
        device.model = ret;
        spotify_device_model = ret;
        ret = meSafeTaskCall(inspect_device, &device);
        if (ret < 0) report_return("inspect witness/table", ret);
    }
    log_printf("DEVICE: model=%d generation=%s firmware=0x%08X\n", device.model,
               generation(device.model), (unsigned int)device.firmware);
    if (device.witness_available)
        log_printf("DEVICE: me_witness=0x%08X\n", (unsigned int)device.witness);
    else log_printf("DEVICE: me_witness=unavailable\n");
    if (device.table_available)
        log_printf("DEVICE: me_table=%d me_table_name=%s\n", device.table, table_name(device.table));
    else log_printf("DEVICE: me_table=unavailable me_table_name=unavailable\n");
    for (int i = 0; i < device.probe_count; ++i)
        log_printf("DEVICE: me_probe 0x%08X = %08X %08X\n", (unsigned int)device.probe_addr[i],
                   (unsigned int)device.probe_word[i][0], (unsigned int)device.probe_word[i][1]);
    memory_log("post-inspection");
    if (ret < 0) {
        log_printf("REFUSED: device inspection failed; no ME patching performed\n");
        return ret;
    }
    if (!device_policy_allows(device.model, device.firmware, device.table,
                              device.witness, mappings_complete(device.table))) {
        if (device.firmware != 0x06060110u)
            log_printf("REFUSED: unsupported firmware 0x%08X on model %d\n",
                       (unsigned int)device.firmware, device.model);
        else log_printf("REFUSED: unsupported ME image table %d on model %d (or unsupported model)\n",
                        device.table, device.model);
        log_printf("No ME patching performed; use START to exit safely\n");
        return FAIL_TARGET;
    }
    log_printf("DEVICE: decision=recognized\n");
    const uintptr_t data_addr = (uintptr_t)&shared_task;
    const uintptr_t code_addr = (uintptr_t)me_loop;
    if ((data_addr & 63u) != 0 || data_addr < 0x08800000u ||
        data_addr + sizeof shared_task > 0x0A000000u ||
        code_addr < 0x08800000u || code_addr >= 0x0A000000u) return FAIL_MEMORY;
    ret = scePowerLock(0);
    if (ret < 0) return ret;
    power_locked = 1;
    ret = meSafeTaskInitDispatcher();
    report_return("ME dispatcher init", ret);
    if (ret < 0) return ret;
    dispatcher_ready = 1;
    unsafe_to_exit = 1;
    ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    report_return("ME load AVCODEC", ret);
    if (ret < 0) return ret;
    av_loaded = 1;
    sceKernelDcacheWritebackAll();
    ret = sceAudiocodecGetEDRAM(codec_data, 0x1000);
    report_return("ME get EDRAM", ret);
    if (ret < 0) return ret;
    ret = sceAudiocodecReleaseEDRAM(codec_data);
    report_return("ME release EDRAM", ret);
    if (ret < 0) return ret;
    unsafe_to_exit = 0;
    memory_log("post-ME");
    log_printf("%s (model %d) ME OK\n",
               device.model == 0 ? "PSP-1000" : device.model == 1 ? "PSP-2000"
               : device.model == 4 ? "PSP Go" : "PSP-3000", device.model);
    return 0;
}
int main(int argc, char **argv)
{
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    int init_result = set_application_directory(argc, argv);
    if (init_result >= 0) {
        log_init(app_directory);
        spotify_config_set_dir(app_directory);
    }
    memory_log("startup");
    if (init_result >= 0) {
        init_result = sceKernelCreateCallback("ME proof exit", exit_callback, NULL);
        if (init_result >= 0) {
            init_result = sceKernelRegisterExitCallback(init_result);
        }
    }
    if (init_result >= 0) init_result = initialize();
    int quit_now = 0;
    if (init_result < 0) {
        log_printf("INIT FAIL: 0x%08X\n", (unsigned int)init_result);
    } else if (network_probe() < 0) {
        log_printf("NETWORK FAILED\n");
    } else {
        memory_log("post-network");
        http_init();
        if (spotify_ui_run(&exit_requested) == PLAYER_ERR_ME_TIMEOUT) {
            /* A decode may still be running on the ME: never unload under it. */
            unsafe_to_exit = 1;
        } else {
            quit_now = 1;
        }
        pspDebugScreenInit();
        log_set_echo(1);
        if (unsafe_to_exit) log_printf("Media Engine stopped responding.\n");
    }
    uint32_t previous_buttons = 0;
controls:
    for (;;) {
        if (quit_now && !unsafe_to_exit) break;
        pspDebugScreenSetXY(0, 30);
        if (unsafe_to_exit)
            pspDebugScreenPrintf("START=reboot PSP                           ");
        else
            pspDebugScreenPrintf("START=exit                                 ");
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        const uint32_t pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;
        if ((pressed & PSP_CTRL_START) || (exit_requested && !unsafe_to_exit)) {
            if (unsafe_to_exit) {
                pspDebugScreenSetXY(0, 25);
                const int ret = meSafeTaskCall(reboot_device, NULL);
                report_return("reboot", ret);
            } else break;
        }
        scePowerTick(PSP_POWER_TICK_ALL);
        sceKernelDelayThreadCB(20000);
    }
    if (dispatcher_ready) {
        pspDebugScreenSetXY(0, 25);
        const int ret = meSafeTaskShutdownDispatcher(TIMEOUT_US);
        if (ret < 0) {
            log_printf("ME shutdown failed. START=reboot.\n");
            unsafe_to_exit = 1;
            exit_requested = 0;
            quit_now = 0;
            goto controls;
        }
    }
    if (av_loaded) sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
    if (network_ready) {
        http_term();
        sceNetApctlDisconnect();
        sceNetApctlTerm();
        sceNetResolverTerm();
        sceNetInetTerm();
        sceNetTerm();
        sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    }
    if (power_locked) scePowerUnlock(0);
    log_printf("Exiting safely\n");
    sceKernelExitGame();
    return init_result < 0 ? 1 : 0;
}
