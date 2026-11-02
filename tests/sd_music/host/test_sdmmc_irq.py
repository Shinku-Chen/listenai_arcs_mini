#!/usr/bin/env python3
"""Exercise production SD IRQ/reset code with injected register faults.

This host harness checks termination and wakeups, not hardware timing or W1C
register semantics. Physical rapid insert/remove testing is still required.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HAL = ROOT / "arcs-sdk/soc/arcs/hal/chip/arcs"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int BaseType_t;
#define CFG_RTOS 1
#define pdFALSE 0
#define ERR_CMD_TIMEOUT 1
#define sdc_dbg_print(...) ((void)0)
#define portYIELD_FROM_ISR(x) ((void)(x))
struct card {
    u32 CardInsert;
    bool already_init;
    u8 cmplMask;
    u16 ErrorSts, autoErr;
};
static struct card card;
static struct {
    struct card *Card;
    u32 min_clk;
    u8 sdcard_init_complete, dma_transfer_data, reset_flag;
    void *sdio_dma_semaphore;
    void (*sdio_card_int)(u16);
} SDHost[1];
static volatile struct {
    u16 IntrSts, ErrSts, AutoCMDErr;
    u8 SoftRst;
} regs;
static bool stuck_reset;
static unsigned wakeups, accesses;
static void *registers(u8 index) {
    assert(index == 0);
    accesses++;
    if (!stuck_reset) regs.SoftRst = 0;
    return (void *)&regs;
}
#define gpRegSDC(i) ((__typeof__(&regs))registers(i))
static void xSemaphoreGiveFromISR(void *s, BaseType_t *w) { wakeups++; *w = 1; }
static void ftsdc021_SetSDClock(u8 i, u32 c) {}
static void ftsdc021_SetPower(u8 i, int p) {}
static int ftsdc021_CheckCardInsert(u8 i) { return 0; }
static void ftsdc021_print_err_msg(u8 i, u16 e) {}
static void ftsdc021_execute_tuning(u8 i, int n) {}

PRODUCTION_CODE

static void setup(u16 irq, u8 wait, bool in_flight, u16 error, bool stuck) {
    memset(&card, 0, sizeof(card));
    memset(SDHost, 0, sizeof(SDHost));
    memset((void *)&regs, 0, sizeof(regs));
    SDHost[0].Card = &card;
    SDHost[0].dma_transfer_data = in_flight;
    card.cmplMask = wait;
    regs.IntrSts = irq;
    regs.ErrSts = error;
    stuck_reset = stuck;
    wakeups = accesses = 0;
}
int main(void) {
    /* Late TX completion must return and preserve another command's wait. */
    setup(SDHCI_INTR_STS_TXR_COMPLETE, WAIT_CMD_COMPLETE, true, 0, false);
    ftsdc021_0_IntrHandler();
    assert(wakeups == 0 && card.cmplMask == WAIT_CMD_COMPLETE);
    assert(SDHost[0].dma_transfer_data == 1);

    /* Normal completion, and simultaneous completion/error, wake only once. */
    for (int error = 0; error < 2; error++) {
        setup(SDHCI_INTR_STS_TXR_COMPLETE | SDHCI_INTR_STS_DMA |
              (error ? SDHCI_INTR_STS_ERR : 0),
              WAIT_TRANS_COMPLETE | WAIT_DMA_INTR, true,
              error ? SDHCI_INTR_ERR_DATA_CRC : 0, false);
        ftsdc021_0_IntrHandler();
        assert(wakeups == 1 && card.cmplMask == 0);
        assert(SDHost[0].dma_transfer_data == 0);
        assert(card.ErrorSts == regs.ErrSts);
    }

    /* Both healthy and permanently stuck reset hardware; no tick advances. */
    const u16 errors[] = {
        SDHCI_INTR_ERR_CMD_TIMEOUT, SDHCI_INTR_ERR_DATA_TIMEOUT,
        SDHCI_INTR_ERR_CMD_TIMEOUT | SDHCI_INTR_ERR_DATA_CRC,
        SDHCI_INTR_ERR_ADMA, SDHCI_INTR_ERR_AutoCMD
    };
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        for (int stuck = 0; stuck < 2; stuck++) {
            setup(SDHCI_INTR_STS_ERR, WAIT_TRANS_COMPLETE, true, errors[i], stuck);
            ftsdc021_0_IntrHandler();
            assert(wakeups == 1 && SDHost[0].dma_transfer_data == 0);
            assert(card.ErrorSts == errors[i]);
            assert(accesses < 200100);
            if (stuck && (errors[i] & (SDHCI_INTR_ERR_CMD_LINE | SDHCI_INTR_ERR_DAT_LINE)))
                assert(SDHost[0].reset_flag == 1);
        }
    }
    setup(SDHCI_INTR_STS_ERR, 0, false, SDHCI_INTR_ERR_DATA_TIMEOUT, true);
    ftsdc021_0_IntrHandler();
    assert(wakeups == 0);
    puts("SDMMC IRQ fault injection passed (14 scenarios)");
}
"""


def main():
    source = (HAL / "driver/sdmmc/ftsdc021.c").read_text()
    header = (HAL / "include/register/ftsdc021.h").read_text()
    # Use the actual register bits and production function bodies.
    defines = "\n".join(re.findall(
        r"^#define (?:SDHCI_INTR_[A-Za-z0-9_]+|SDHCI_SOFTRST_\w+|WAIT_\w+)\s+[^\n]+",
        header, re.MULTILINE))
    code = function(source, "static u32\nftsdc021_reset_lines(")
    code += "\n" + function(source, "void\nftsdc021_0_IntrHandler()")
    with tempfile.TemporaryDirectory(prefix="sdmmc-irq-") as temp:
        path = Path(temp)
        (path / "test.c").write_text(defines + "\n" + HARNESS.replace("PRODUCTION_CODE", code))
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=5)


if __name__ == "__main__":
    main()
