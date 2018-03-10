#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <Include/ota.h>
#include <driverlib/flash.h>
#include <driverlib/vims.h>
#include <ti/sysbios/hal/Hwi.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/drivers/PWM.h>
#include <driverlib/sys_ctrl.h>

#include <../boards/_CC1350_LAUNCHXL/_Board.h>

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define _NEED_DISABLE_CACHE 1
#define _NEED_DISABLE_HWI 1

struct ota_region *OTA_REGION = (struct ota_region *) OTA_FLASH_BASE;

#define INVALID_GEN ((unsigned long) -1)

#if _NEED_DISABLE_CACHE == 1
static uint8_t cache_state(void) {
    return VIMSModeGet(VIMS_BASE);
}

static void set_cache_state(uint32_t mode) {
    VIMSModeSet(VIMS_BASE, mode);
}

static uint32_t disable_cache(void) {
    uint32_t state = cache_state();
    if (state != VIMS_MODE_OFF) {
        set_cache_state(VIMS_MODE_OFF);
        while (cache_state() != VIMS_MODE_OFF)
            ;
    }
    VIMSLineBufDisable(VIMS_BASE);
    return state;
}

static void enable_cache(uint32_t state) {
    if (state != VIMS_MODE_DISABLED) {
        set_cache_state(VIMS_MODE_ENABLED);
        while (cache_state() != VIMS_MODE_ENABLED)
            ;
    }
    VIMSLineBufEnable(VIMS_BASE);
}
#endif

#if _NEED_DISABLE_HWI == 1
#define DISABLE_HWI() uint32_t _hwi = Hwi_disable()
#define RESTORE_HWI() Hwi_restore(_hwi)
#else
#define DISABLE_HWI() ((void) 0)
#define RESTORE_HWI() ((void) 0)
#endif


#if _NEED_DISABLE_CACHE == 1
#define DISABLE_CACHE() uint32_t _cache = disable_cache()
#define RESTORE_CACHE() enable_cache(_cache)
#else
#define DISABLE_CACHE() ((void) 0)
#define RESTORE_CACHE() ((void) 0)
#endif

static uint32_t ota_FlashProgram(
        uint8_t *pui8DataBuffer,
        uint32_t ui32Address,
        uint32_t ui32Count)
{
    DISABLE_HWI();
    DISABLE_CACHE();

    uint32_t rc = FlashProgram(pui8DataBuffer, ui32Address, ui32Count);

    RESTORE_CACHE();
    RESTORE_HWI();
    return rc;
}

static void ota_FlashProtectionSet(uint32_t ui32SectorAddress,
                               uint32_t ui32ProtectMode) {
    DISABLE_HWI();
    DISABLE_CACHE();

    FlashProtectionSet(ui32SectorAddress, ui32ProtectMode);

    RESTORE_CACHE();
    RESTORE_HWI();
}


static uint32_t ota_FlashSectorErase(uint32_t ui32SectorAddress) {
    DISABLE_HWI();
    DISABLE_CACHE();

    uint32_t rc = FlashSectorErase(ui32SectorAddress);

    RESTORE_CACHE();
    RESTORE_HWI();

    return rc;
}

#define OTA_TASK_STACK_SIZE 1024
#define OTA_TASK_PRIORITY 4

static uint8_t ota_task_stack[OTA_TASK_STACK_SIZE];


static inline ota_entrypoint_t ota_zone_entrypoint(struct ota_zone *zone) {
    uintptr_t ptr = (uintptr_t) zone;
    ptr += (uintptr_t) zone->metadata.entrypoint;
    return (ota_entrypoint_t) ptr;
}

static void __ota_startup() {
    Task_Params task_params;
    Task_Struct task;    /* not static so you can see in ROV */

    /* Create the node task */
    Task_Params_init(&task_params);
    task_params.stackSize = OTA_TASK_STACK_SIZE;
    task_params.priority = OTA_TASK_PRIORITY;
    task_params.stack = &ota_task_stack;
    Task_construct(&task, ota_zone_entrypoint(&OTA_REGION->zones[OTA_ACTIVE_ZONE]), &task_params, NULL);
}

#define OTA_COPY_CHUNK 256

static void __ota_copy_zone(struct ota_zone *dst, struct ota_zone *src) {
    struct ota_dl_params p;
    struct ota_dl_state s;
    uint8_t buf[OTA_COPY_CHUNK];

    p.dl_size = src->metadata.size;
    p.entrypoint = src->metadata.entrypoint;

    ota_dl_init(&s, &p);
    s.target_zone = dst;
    s.target_gen = src->metadata.gen + 1;

    ota_dl_begin(&s);

    while (s.dl_done < s.dl_size) {
        size_t len = min(OTA_COPY_CHUNK, s.dl_size - s.dl_done);
        memcpy(buf, &src->payload[s.dl_done], len);
        ota_dl_process(&s, buf, len);
    }

    ota_dl_finish(&s);
}

void ota_startup(void) {
    // Check if inactive image is newer, if it is, copy over active and reset
    struct ota_zone *act = &OTA_REGION->zones[OTA_ACTIVE_ZONE];
    struct ota_zone *inact = &OTA_REGION->zones[OTA_INACTIVE_ZONE];

    if (inact->metadata.done == OTA_DONE_MAGIC) {

        if (act->metadata.done != OTA_DONE_MAGIC ||
            inact->metadata.gen > act->metadata.gen
        ) {
            __ota_copy_zone(act, inact);
            SysCtrlSystemReset();
        }
    }

    if (act->metadata.done == OTA_DONE_MAGIC)
        __ota_startup();
}

#define OTA_ENTRYPOINT(zone) \
    ((ota_entrypoint_t) (((uintptr_t) zone->payload) + ((zone->metadata.entrypoint)))

void ota_dl_init(struct ota_dl_state *state, struct ota_dl_params *params) {
    state->target_zone = &OTA_REGION->zones[OTA_INACTIVE_ZONE];
    if (OTA_REGION->zones[OTA_ACTIVE_ZONE].metadata.done == OTA_DONE_MAGIC)
        state->target_gen = OTA_REGION->zones[OTA_ACTIVE_ZONE].metadata.gen + 1;
    else
        state->target_gen = 0;

    state->dl_done = 0;
    state->dl_size = params->dl_size;
    state->entrypoint = params->entrypoint;
    state->sector_size = FlashSectorSizeGet();
    state->nr_sectors = sizeof (struct ota_zone) / state->sector_size;
}

#define _first_sector(state)    \
    (((uint32_t) state->target_zone) / state->sector_size)

#define _last_sector(state)     \
    (_first_sector(state) + state->nr_sectors)

#define FOREACH_SECTOR(state, idx)                  \
    for (int idx = _first_sector(state); idx < _last_sector(state); i++)

int ota_dl_begin(struct ota_dl_state *state) {
    FOREACH_SECTOR(state, i) {
        uint32_t rc;

        ota_FlashProtectionSet(i * state->sector_size, FLASH_NO_PROTECT);

        rc = ota_FlashSectorErase(i * state->sector_size);
        if (rc != FAPI_STATUS_SUCCESS)
            return (int) rc;
    }
    return 0;
}

int ota_dl_process(struct ota_dl_state *state, uint8_t *buf, size_t len)  {
        int rc = ota_FlashProgram(
                buf,
                (uint32_t) &state->target_zone->payload[state->dl_done],
                len);

        if (rc != FAPI_STATUS_SUCCESS)
            return rc;

        state->dl_done += len;
        return 0;
}

int ota_dl_finish(struct ota_dl_state *state) {
    unsigned long magic = OTA_DONE_MAGIC;

    int rc = ota_FlashProgram(
            (uint8_t *) &state->dl_size,
            (uint32_t) &state->target_zone->metadata.size,
            sizeof (size_t));

    if (rc != FAPI_STATUS_SUCCESS)
        return (int) rc;

    rc = ota_FlashProgram(
            (uint8_t *) &state->entrypoint,
            (uint32_t) &state->target_zone->metadata.entrypoint,
            sizeof (ota_entrypoint_t));

    if (rc != FAPI_STATUS_SUCCESS)
        return (int) rc;

    rc = ota_FlashProgram(
            (uint8_t *) &state->target_gen,
            (uint32_t) &state->target_zone->metadata.gen,
            sizeof (unsigned long));

    if (rc != FAPI_STATUS_SUCCESS)
        return (int) rc;

    rc = ota_FlashProgram(
            (uint8_t *) &magic,
            (uint32_t) &state->target_zone->metadata.done,
            sizeof (unsigned long));

    if (rc != FAPI_STATUS_SUCCESS)
        return (int) rc;

    FOREACH_SECTOR(state, i) {
        ota_FlashProtectionSet(i * state->sector_size, FLASH_WRITE_PROTECT);
    }
    return 0;
}

extern void payload_test_app(UArg arg1, UArg arg2);
extern int payload_test_app_end(void);

#define _UINT(x) ((uintptr_t) x)

void test_ota(void) {
    PWM_init();
    PWM_Params pwm_p;
    PWM_Params_init(&pwm_p);
    pwm_p.dutyUnits = PWM_DUTY_US;
    pwm_p.dutyValue = 100;
    pwm_p.periodUnits = PWM_PERIOD_US;
    pwm_p.periodValue = 100;

    PWM_Handle pwm = PWM_open(Board_PWM1, &pwm_p);
    while (pwm == NULL)
        ;
    PWM_start(pwm);

    size_t func_size = _UINT(payload_test_app_end) - _UINT(payload_test_app);
    uint8_t *buf = malloc(func_size);
    memcpy(buf, (uint8_t *) 0x17000, func_size);

    struct ota_dl_params p;
    p.dl_size = func_size;
    p.entrypoint = 0;

    struct ota_dl_state s;
    ota_dl_init(&s, &p);
    ota_dl_begin(&s);
    ota_dl_process(&s, buf, func_size);
    ota_dl_finish(&s);
    __ota_copy_zone(&OTA_REGION->zones[OTA_ACTIVE_ZONE],
                    &OTA_REGION->zones[OTA_INACTIVE_ZONE]);
    payload_test_app(0, 0);
}
