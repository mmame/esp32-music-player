/*
 * freertos_psram_task.c
 *
 * Provides xTaskCreateRestrictedPinnedToCore() for IDF v5.4+ where the
 * ADF FreeRTOS patch (idf_v5.x_freertos.patch) has not been applied.
 *
 * ADF's audio_thread.c declares the function __attribute__((weak)), so
 * this strong definition silently overrides the stub that always fails.
 *
 * Implementation: allocate the TCB from internal RAM via heap_caps_malloc,
 * then delegate to xTaskCreateStaticPinnedToCore().  The PSRAM stack buffer
 * was already allocated by ADF's audio_thread_create() via audio_calloc().
 * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y (already set in sdkconfig)
 * enables the FreeRTOS kernel to schedule tasks whose stacks live in PSRAM.
 *
 * Note: the TCB allocation is intentionally not freed on task deletion
 * because ADF audio-pipeline element tasks are long-lived and ADF itself
 * does not provide a deletion hook here.  The leak is bounded and benign.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

/* portPRIVILEGE_BIT is defined as 0 on non-MPU Xtensa targets; guard just
   in case a future port changes this. */
#ifndef portPRIVILEGE_BIT
#define portPRIVILEGE_BIT  ((UBaseType_t)0)
#endif

BaseType_t xTaskCreateRestrictedPinnedToCore(
    const TaskParameters_t * const pxTaskDefinition,
    TaskHandle_t *pxCreatedTask,
    const BaseType_t xCoreID)
{
    if (!pxTaskDefinition || !pxTaskDefinition->puxStackBuffer) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    /* TCB must reside in internal RAM. */
    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tcb) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    /* Strip the MPU privilege bit; it is not meaningful on Xtensa. */
    UBaseType_t prio = pxTaskDefinition->uxPriority & ~portPRIVILEGE_BIT;

    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        pxTaskDefinition->pvTaskCode,
        pxTaskDefinition->pcName,
        (uint32_t)pxTaskDefinition->usStackDepth,
        pxTaskDefinition->pvParameters,
        prio,
        pxTaskDefinition->puxStackBuffer,   /* stack: already in PSRAM */
        tcb,                                /* TCB:   internal RAM     */
        xCoreID);

    if (handle != NULL) {
        if (pxCreatedTask) {
            *pxCreatedTask = handle;
        }
        return pdPASS;
    }

    heap_caps_free(tcb);
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
}
