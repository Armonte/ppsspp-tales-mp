// Runtime verification for the ppsspp-tales-mp extra-pads MMIO window.
//
// Reads pads 0..3 from 0x0E000000 + i*16 each frame and prints them.
// Pad 0 is also read via standard sceCtrlReadBufferPositive for sanity.
// Build with the pspdev toolchain; run inside ppsspp-tales-mp v0.1.0+ with
// "Enable virtual pads 2-4" turned on. Will fault or read zeros on stock PPSSPP.

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

PSP_MODULE_INFO("ExtraPadsTest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define EXTRA_PADS_BASE 0x0E000000
#define NUM_PADS 8

// Layout matches PPSSPP's internal CtrlData and what sceCtrlReadBufferPositive writes.
typedef struct {
    uint32_t frame;       // microsecond timestamp
    uint32_t buttons;     // PSP_CTRL_* bitmask
    uint8_t  lx, ly;
    uint8_t  rx, ry;
    uint8_t  reserved[4];
} ExtraPad;

#define pspDebugScreenPrintf pspDebugScreenPrintf

static int exit_callback(int arg1, int arg2, void *common) {
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("ExitCallback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
    return thid;
}

static void print_pad(int idx, const ExtraPad *p) {
    pspDebugScreenPrintf("P%d frame=%08lx buttons=%08lx Lx=%3u Ly=%3u Rx=%3u Ry=%3u\n",
        idx, p->frame, p->buttons, p->lx, p->ly, p->rx, p->ry);
}

int main(int argc, char *argv[]) {
    pspDebugScreenInit();
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    // The MMIO window. On stock PPSSPP these reads will fault.
    volatile const ExtraPad *pads = (const ExtraPad *)EXTRA_PADS_BASE;

    int sample = 0;
    while (sample < 60) {  // ~1 second worth of samples
        // Snapshot all MMIO entries.
        ExtraPad snap[NUM_PADS];
        for (int i = 0; i < NUM_PADS; ++i) snap[i] = pads[i];

        // Cross-check pad 0 against sceCtrl.
        SceCtrlData sce;
        sceCtrlReadBufferPositive(&sce, 1);

        // On-screen for interactive runs.
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("extra-pads test sample=%d\n", sample);
        for (int i = 0; i < NUM_PADS; ++i) print_pad(i, &snap[i]);
        pspDebugScreenPrintf("--------------------------------------------------\n");
        pspDebugScreenPrintf("sceCtrl  frame=%08x buttons=%08lx Lx=%3u Ly=%3u\n",
            sce.TimeStamp, sce.Buttons, sce.Lx, sce.Ly);

        // Stdout for headless runs (printf goes via Kprintf in pspsdk).
        printf("SAMPLE %d\n", sample);
        for (int i = 0; i < NUM_PADS; ++i) {
            printf("  PAD%d frame=%08lx buttons=%08lx Lx=%3u Ly=%3u Rx=%3u Ry=%3u\n",
                i, snap[i].frame, snap[i].buttons, snap[i].lx, snap[i].ly, snap[i].rx, snap[i].ry);
        }
        printf("  sceCtrl: frame=%08x buttons=%08lx Lx=%3u Ly=%3u\n",
            sce.TimeStamp, sce.Buttons, sce.Lx, sce.Ly);

        sceDisplayWaitVblankStart();
        ++sample;
    }
    printf("DONE\n");

    sceKernelExitGame();
    return 0;
}
