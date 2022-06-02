// Software CANbus implementation for rp2040
//
// Copyright (C) 2022  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uint32_t
#include <string.h> // memset
#include "RP2040.h" // hw_set_bits
#include "can2040.h" // can2040_setup
#include "hardware/regs/dreq.h" // DREQ_PIO0_RX1
#include "hardware/structs/dma.h" // dma_hw
#include "hardware/structs/iobank0.h" // iobank0_hw
#include "hardware/structs/padsbank0.h" // padsbank0_hw
#include "hardware/structs/pio.h" // pio0_hw
#include "hardware/structs/resets.h" // RESETS_RESET_PIO0_BITS


/****************************************************************
 * rp2040 and low-level helper functions
 ****************************************************************/

// Helper compiler definitions
#define barrier() __asm__ __volatile__("": : :"memory")
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

// Helper functions for writing to "io" memory
static inline void writel(void *addr, uint32_t val) {
    barrier();
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t readl(const void *addr) {
    uint32_t val = *(volatile const uint32_t *)addr;
    barrier();
    return val;
}
static inline uint8_t readb(const void *addr) {
    uint8_t val = *(volatile const uint8_t *)addr;
    barrier();
    return val;
}

// rp2040 helper function to clear a hardware reset bit
static void
rp2040_clear_reset(uint32_t reset_bit)
{
    if (resets_hw->reset & reset_bit) {
        resets_hw->reset &= reset_bit;
        hw_clear_bits(&resets_hw->reset, reset_bit);
        while (!(resets_hw->reset_done & reset_bit))
            ;
    }
}

// Helper to set the mode and extended function of a pin
static void
rp2040_gpio_peripheral(uint32_t gpio, int func, int pull_up)
{
    padsbank0_hw->io[gpio] = (
        PADS_BANK0_GPIO0_IE_BITS
        | (PADS_BANK0_GPIO0_DRIVE_VALUE_4MA << PADS_BANK0_GPIO0_DRIVE_MSB)
        | (pull_up > 0 ? PADS_BANK0_GPIO0_PUE_BITS : 0)
        | (pull_up < 0 ? PADS_BANK0_GPIO0_PDE_BITS : 0));
    iobank0_hw->io[gpio].ctrl = func << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
}


/****************************************************************
 * rp2040 PIO support
 ****************************************************************/

#define can2040_offset_sync_signal_start 4u
#define can2040_offset_sync_entry 6u
#define can2040_offset_sync_end 13u
#define can2040_offset_shared_rx_read 13u
#define can2040_offset_shared_rx_end 15u
#define can2040_offset_ack_no_match 18u
#define can2040_offset_ack_end 25u
#define can2040_offset_tx_start 26u

static const uint16_t can2040_program_instructions[] = {
    0x0085, //  0: jmp    y--, 5
    0x0048, //  1: jmp    x--, 8
    0xe03c, //  2: set    x, 28
    0x00cc, //  3: jmp    pin, 12
    0xc000, //  4: irq    nowait 0
    0x00c0, //  5: jmp    pin, 0
    0xc040, //  6: irq    clear 0
    0xe228, //  7: set    x, 8                   [2]
    0xe242, //  8: set    y, 2                   [2]
    0xc104, //  9: irq    nowait 4               [1]
    0x03c5, // 10: jmp    pin, 5                 [3]
    0x0307, // 11: jmp    7                      [3]
    0x0043, // 12: jmp    x--, 3
    0x20c4, // 13: wait   1 irq, 4
    0x4001, // 14: in     pins, 1
    0xa046, // 15: mov    y, isr
    0x00b2, // 16: jmp    x != y, 18
    0xc002, // 17: irq    nowait 2
    0x40eb, // 18: in     osr, 11
    0x4054, // 19: in     y, 20
    0xa047, // 20: mov    y, osr
    0x8080, // 21: pull   noblock
    0xa027, // 22: mov    x, osr
    0x0098, // 23: jmp    y--, 24
    0xa0e2, // 24: mov    osr, y
    0xa242, // 25: nop                           [2]
    0x6021, // 26: out    x, 1
    0xa001, // 27: mov    pins, x
    0x20c4, // 28: wait   1 irq, 4
    0x00d9, // 29: jmp    pin, 25
    0x023a, // 30: jmp    !x, 26                 [2]
    0xc023, // 31: irq    wait 3
};

static void
pio_sync_setup(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    struct pio_sm_hw *sm = &pio_hw->sm[0];
    sm->execctrl = (
        cd->gpio_rx << PIO_SM0_EXECCTRL_JMP_PIN_LSB
        | (can2040_offset_sync_end - 1) << PIO_SM0_EXECCTRL_WRAP_TOP_LSB
        | can2040_offset_sync_signal_start << PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB);
    sm->pinctrl = (
        1 << PIO_SM0_PINCTRL_SET_COUNT_LSB
        | cd->gpio_rx << PIO_SM0_PINCTRL_SET_BASE_LSB);
    sm->instr = 0xe080; // set pindirs, 0
    sm->pinctrl = 0;
    sm->instr = can2040_offset_sync_entry; // jmp sync_entry
}

static void
pio_rx_setup(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    struct pio_sm_hw *sm = &pio_hw->sm[1];
    sm->execctrl = (
        (can2040_offset_shared_rx_end - 1) << PIO_SM0_EXECCTRL_WRAP_TOP_LSB
        | can2040_offset_shared_rx_read << PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB);
    sm->pinctrl = cd->gpio_rx << PIO_SM0_PINCTRL_IN_BASE_LSB;
    sm->shiftctrl = (PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS
                     | 8 << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB
                     | PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS);
    sm->instr = can2040_offset_shared_rx_read; // jmp shared_rx_read
}

static void
pio_ack_setup(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    struct pio_sm_hw *sm = &pio_hw->sm[2];
    sm->execctrl = (
        (can2040_offset_ack_end - 1) << PIO_SM0_EXECCTRL_WRAP_TOP_LSB
        | can2040_offset_shared_rx_read << PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB);
    sm->pinctrl = cd->gpio_rx << PIO_SM0_PINCTRL_IN_BASE_LSB;
    sm->shiftctrl = 0;
    sm->instr = 0xe040; // set y, 0
    sm->instr = 0xa0e2; // mov osr, y
    sm->instr = 0xa02a, // mov x, !y
    sm->instr = can2040_offset_ack_no_match; // jmp ack_no_match
}

static void
pio_tx_setup(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    struct pio_sm_hw *sm = &pio_hw->sm[3];
    sm->execctrl = (cd->gpio_rx << PIO_SM0_EXECCTRL_JMP_PIN_LSB
                    | 0x1f << PIO_SM0_EXECCTRL_WRAP_TOP_LSB
                    | 0x00 << PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB);
    sm->shiftctrl = (PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS
                     | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS);
    sm->pinctrl = (1 << PIO_SM0_PINCTRL_SET_COUNT_LSB
                   | 1 << PIO_SM0_PINCTRL_OUT_COUNT_LSB
                   | cd->gpio_tx << PIO_SM0_PINCTRL_SET_BASE_LSB
                   | cd->gpio_tx << PIO_SM0_PINCTRL_OUT_BASE_LSB);
    sm->instr = 0xe001; // set pins, 1
    sm->instr = 0xe081; // set pindirs, 1
}

static void
pio_tx_reset(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->ctrl = ((0x07 << PIO_CTRL_SM_ENABLE_LSB)
                    | (0x08 << PIO_CTRL_SM_RESTART_LSB));
    if (pio_hw->flevel & PIO_FLEVEL_TX3_BITS) {
        struct pio_sm_hw *sm = &pio_hw->sm[3];
        sm->shiftctrl = 0;
        sm->shiftctrl = (PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS
                         | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS);
    }
}

static void
pio_tx_send(struct can2040 *cd, uint32_t *data, uint32_t count)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_tx_reset(cd);
    struct pio_sm_hw *sm = &pio_hw->sm[3];
    sm->instr = can2040_offset_tx_start; // jmp tx_start
    sm->instr = 0x20c0; // wait 1 irq, 0
    int i;
    for (i=0; i<count; i++)
        pio_hw->txf[3] = data[i];
    pio_hw->ctrl = 0x0f << PIO_CTRL_SM_ENABLE_LSB;
}

static void
pio_tx_cancel(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->ctrl = 0x07 << PIO_CTRL_SM_ENABLE_LSB;
    struct pio_sm_hw *sm = &pio_hw->sm[3];
    sm->instr = 0xe001; // set pins, 1
}

static void
pio_ack_inject(struct can2040 *cd, uint32_t crc_bits, uint32_t rx_bit_pos)
{
    uint32_t key = (crc_bits & 0x1fffff) | ((-rx_bit_pos) << 21);
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_tx_reset(cd);
    struct pio_sm_hw *sm = &pio_hw->sm[3];
    sm->instr = can2040_offset_tx_start; // jmp tx_start
    sm->instr = 0xc042; // irq clear 2
    sm->instr = 0x20c2; // wait 1 irq, 2
    pio_hw->txf[3] = 0x7fffffff;
    pio_hw->ctrl = 0x0f << PIO_CTRL_SM_ENABLE_LSB;
    pio_hw->txf[2] = key;
}

static void
pio_ack_cancel(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->txf[2] = 0;
}

static int
pio_rx_check_stall(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    return pio_hw->fdebug & (1 << (PIO_FDEBUG_RXSTALL_LSB + 1));
}

static void
pio_sync_enable_idle_irq(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->inte0 = PIO_IRQ0_INTE_SM0_BITS;
}

static void
pio_sync_disable_idle_irq(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->inte0 = 0;
}

static uint32_t
pio_sync_check_idle(struct can2040 *cd)
{
    pio_hw_t *pio_hw = cd->pio_hw;
    return pio_hw->intr & PIO_IRQ0_INTE_SM0_BITS;
}

static void
pio_sm_setup(struct can2040 *cd)
{
    // Reset state machines
    pio_hw_t *pio_hw = cd->pio_hw;
    pio_hw->ctrl = PIO_CTRL_SM_RESTART_BITS | PIO_CTRL_CLKDIV_RESTART_BITS;
    pio_hw->fdebug = 0xffffffff;

    // Load pio program
    int i;
    for (i=0; i<ARRAY_SIZE(can2040_program_instructions); i++)
        pio_hw->instr_mem[i] = can2040_program_instructions[i];

    // Set initial state machine state
    pio_sync_setup(cd);
    pio_rx_setup(cd);
    pio_ack_setup(cd);
    pio_tx_setup(cd);

    // Start state machines
    pio_hw->ctrl = 0x07 << PIO_CTRL_SM_ENABLE_LSB;
}

#define PIO_FUNC 6

static void
pio_setup(struct can2040 *cd, uint32_t sys_clock, uint32_t bitrate)
{
    // Configure pio0 clock
    uint32_t rb = cd->pio_num ? RESETS_RESET_PIO1_BITS : RESETS_RESET_PIO0_BITS;
    rp2040_clear_reset(rb);

    // Setup and sync pio state machine clocks
    pio_hw_t *pio_hw = cd->pio_hw;
    uint32_t div = (256 / 16) * sys_clock / bitrate;
    int i;
    for (i=0; i<4; i++)
        pio_hw->sm[i].clkdiv = div << PIO_SM0_CLKDIV_FRAC_LSB;

    // Configure state machines
    pio_sm_setup(cd);

    // Map Rx/Tx gpios
    rp2040_gpio_peripheral(cd->gpio_rx, PIO_FUNC, 1);
    rp2040_gpio_peripheral(cd->gpio_tx, PIO_FUNC, 0);
}


/****************************************************************
 * CRC calculation
 ****************************************************************/

static uint32_t
crcbits(uint32_t crc, uint32_t data, uint32_t count)
{
    int i;
    for (i=count-1; i>=0; i--) {
        uint32_t bit = (data >> i) & 1;
        crc = ((crc >> 14) & 1) ^ bit ? (crc << 1) ^ 0x4599 : (crc << 1);
    }
    return crc;
}


/****************************************************************
 * Bit unstuffing
 ****************************************************************/

static void
unstuf_add_bits(struct can2040_bitunstuffer *bu, uint32_t data, uint32_t count)
{
    uint32_t mask = (1 << count) - 1;
    bu->stuffed_bits = (bu->stuffed_bits << count) | (data & mask);
    bu->count_stuff = count;
}

static void
unstuf_set_count(struct can2040_bitunstuffer *bu, uint32_t count)
{
    bu->unstuffed_bits = 0;
    bu->count_unstuff = count;
}

static void
unstuf_clear_state(struct can2040_bitunstuffer *bu)
{
    uint32_t sb = bu->stuffed_bits, edges = sb ^ (sb >> 1);
    uint32_t cs = bu->count_stuff, re = edges >> cs;
    if (!(re & 1) && (re & 0xf))
        bu->stuffed_bits ^= 1 << cs;
}

static int
unstuf_pull_bits(struct can2040_bitunstuffer *bu)
{
    uint32_t sb = bu->stuffed_bits, edges = sb ^ (sb >> 1);
    uint32_t ub = bu->unstuffed_bits;
    uint32_t cs = bu->count_stuff, cu = bu->count_unstuff;
    for (;;) {
        if (!cu) {
            // Extracted desired bits
            bu->unstuffed_bits = ub;
            bu->count_stuff = cs;
            bu->count_unstuff = cu;
            return 0;
        }
        if (!cs) {
            // Need more data
            bu->unstuffed_bits = ub;
            bu->count_stuff = cs;
            bu->count_unstuff = cu;
            return 1;
        }
        cs--;
        if ((edges >> (cs+1)) & 0xf) {
            // Normal data
            cu--;
            ub |= ((sb >> cs) & 1) << cu;
        } else if (((edges >> cs) & 0x1f) == 0x00) {
            // Six consecutive bits - a bitstuff error
            bu->unstuffed_bits = ub;
            bu->count_stuff = cs;
            bu->count_unstuff = cu;
            if ((sb >> cs) & 1)
                return -1;
            return -2;
        }
    }
}


/****************************************************************
 * Bit stuffing
 ****************************************************************/

static uint32_t
bitstuff(uint32_t *pb, uint32_t num_bits)
{
    uint32_t b = *pb, edges = b ^ (b >> 1), count = num_bits;
    int i;
    for (i=num_bits-1; i>=0; i--) {
        if (!((edges >> i) & 0xf)) {
            uint32_t mask = (1 << (i + 1)) - 1;
            uint32_t low = b & mask, high = (b & ~(mask >> 1)) << 1;
            b = high ^ low ^ (1 << i);
            i -= 3;
            count++;
            edges = b ^ (b >> 1);
        }
    }
    *pb = b;
    return count;
}

struct bitstuffer_s {
    uint32_t prev_stuffed, bitpos, *buf, crc;
};

static void
bs_pushraw(struct bitstuffer_s *bs, uint32_t data, uint32_t count)
{
    uint32_t bitpos = bs->bitpos;
    uint32_t wp = bitpos / 32, bitused = bitpos % 32, bitavail = 32 - bitused;
    uint32_t *fb = &bs->buf[wp];
    if (bitavail >= count) {
        fb[0] |= data << (bitavail - count);
    } else {
        fb[0] |= data >> (count - bitavail);
        fb[1] |= data << (32 - (count - bitavail));
    }
    bs->bitpos = bitpos + count;
}

static void
bs_push(struct bitstuffer_s *bs, uint32_t data, uint32_t count)
{
    data &= (1 << count) - 1;
    bs->crc = crcbits(bs->crc, data, count);
    uint32_t stuf = (bs->prev_stuffed << count) | data;
    uint32_t newcount = bitstuff(&stuf, count);
    bs_pushraw(bs, stuf, newcount);
    bs->prev_stuffed = stuf;
}

static uint32_t
bs_finalize(struct bitstuffer_s *bs)
{
    uint32_t bitpos = bs->bitpos;
    uint32_t words = DIV_ROUND_UP(bitpos, 32);
    uint32_t extra = words * 32 - bitpos;
    if (extra)
        bs->buf[words - 1] |= (1 << extra) - 1;
    return words;
}


/****************************************************************
 * Notification callbacks
 ****************************************************************/

static void
report_error(struct can2040 *cd, uint32_t error_code)
{
    struct can2040_msg msg = {};
    cd->rx_cb(cd, CAN2040_ID_ERROR | error_code, &msg);
}

static void
report_rx_msg(struct can2040 *cd)
{
    cd->rx_cb(cd, CAN2040_ID_RX, &cd->parse_msg);
}

static void
report_tx_msg(struct can2040 *cd, struct can2040_msg *msg)
{
    cd->rx_cb(cd, CAN2040_ID_TX, msg);
}

static void
report_tx_fail(struct can2040 *cd, struct can2040_msg *msg)
{
    cd->rx_cb(cd, CAN2040_ID_TX_FAIL, msg);
}


/****************************************************************
 * Transmit
 ****************************************************************/

static uint32_t
tx_qpos(struct can2040 *cd, uint32_t pos)
{
    return pos % ARRAY_SIZE(cd->tx_queue);
}

static void
tx_do_schedule(struct can2040 *cd)
{
    if (cd->in_transmit || cd->tx_push_pos == cd->tx_pull_pos)
        return;
    if (cd->cancel_count > 32) { // XXX
        cd->cancel_count = 0;
        uint32_t tx_pull_pos = cd->tx_pull_pos;
        cd->tx_pull_pos++;
        report_tx_fail(cd, &cd->tx_queue[tx_qpos(cd, tx_pull_pos)].msg);
    }
    cd->in_transmit = 1;
    struct can2040_transmit *qt = &cd->tx_queue[tx_qpos(cd, cd->tx_pull_pos)];
    pio_tx_send(cd, qt->stuffed_data, qt->stuffed_words);
}

static void
tx_cancel(struct can2040 *cd)
{
    cd->in_transmit = 0;
    cd->cancel_count++;
    pio_tx_cancel(cd);
}

static int
tx_check_self_transmit(struct can2040 *cd)
{
    if (!cd->in_transmit)
        return 0;
    struct can2040_transmit *qt = &cd->tx_queue[tx_qpos(cd, cd->tx_pull_pos)];
    struct can2040_msg *pm = &cd->parse_msg;
    if (qt->crc == cd->parse_crc
        && qt->msg.addr == pm->addr && qt->msg.data_len == pm->data_len
        && qt->msg.d4[0] == pm->d4[0] && qt->msg.d4[1] == pm->d4[1]) {
        return 1;
    }
    tx_cancel(cd);
    return 0;
}

static void
tx_finalize(struct can2040 *cd)
{
    tx_cancel(cd);
    cd->cancel_count = 0;
    uint32_t tx_pull_pos = cd->tx_pull_pos;
    cd->tx_pull_pos++;
    report_tx_msg(cd, &cd->tx_queue[tx_qpos(cd, tx_pull_pos)].msg);
}


/****************************************************************
 * Input state tracking
 ****************************************************************/

enum {
    MS_START, MS_DATA, MS_CRC, MS_ACK, MS_EOF, MS_DISCARD
};

static void
data_state_go_discard(struct can2040 *cd)
{
    cd->parse_state = MS_DISCARD;
    unstuf_set_count(&cd->unstuf, 8);
    tx_cancel(cd);
    pio_sync_enable_idle_irq(cd);
}

static void
data_state_go_error(struct can2040 *cd)
{
    data_state_go_discard(cd);
}

static void
data_state_go_idle(struct can2040 *cd)
{
    if (cd->parse_state == MS_START) {
        if (!cd->unstuf.count_stuff && cd->unstuf.stuffed_bits == 0xffffffff) {
            // Counter overflow in "sync" state machine - reset it
            pio_sync_setup(cd);
            cd->unstuf.stuffed_bits = 0;
            data_state_go_discard(cd);
            return;
        }
        unstuf_set_count(&cd->unstuf, 18);
        return;
    }
    pio_sync_disable_idle_irq(cd);
    if (cd->parse_state == MS_EOF) {
        // Check if final eof bits are all 1s
        uint32_t ub = cd->unstuf.unstuffed_bits, cu = cd->unstuf.count_unstuff;
        if ((ub >> cu) + 1 == (1 << (6 - cu))) {
            // Success - send notification
            if (tx_check_self_transmit(cd))
                tx_finalize(cd);
            else
                report_rx_msg(cd);
        }
    }
    pio_ack_cancel(cd);
    tx_do_schedule(cd);
    cd->parse_state = MS_START;
    unstuf_set_count(&cd->unstuf, 18);
}

static void
data_state_go_crc(struct can2040 *cd)
{
    cd->parse_state = MS_CRC;
    unstuf_set_count(&cd->unstuf, 15);
    cd->parse_crc &= 0x7fff;

    // Check if the message was a self transmission
    if (tx_check_self_transmit(cd))
        return;

    // Inject ack
    uint32_t cs = cd->unstuf.count_stuff;
    uint32_t last = (cd->unstuf.stuffed_bits >> cs) << 15;
    last |= cd->parse_crc;
    int count = bitstuff(&last, 15 + 1) - 1;
    last = (last << 1) | 1;
    uint32_t pos = cd->raw_bit_count - cs - 1;
    pio_ack_inject(cd, last, pos + count + 1);
}

static void
data_state_update_start(struct can2040 *cd, uint32_t data)
{
    if ((data & ((1<<18) | (7<<4))) != 0) {
        // Not a supported header
        data_state_go_discard(cd);
        return;
    }
    cd->parse_hdr = data;
    cd->parse_crc = crcbits(0, data, 18);
    uint32_t rdlc = data & 0xf, dlc = rdlc > 8 ? 8 : rdlc;
    cd->parse_msg.addr = (data >> 7) & 0x7ff;
    cd->parse_msg.data_len = dlc;
    cd->parse_msg.d4[0] = cd->parse_msg.d4[1] = 0;
    cd->parse_datapos = 0;
    if (cd->parse_datapos >= dlc) {
        data_state_go_crc(cd);
    } else {
        cd->parse_state = MS_DATA;
        unstuf_set_count(&cd->unstuf, 8);
    }
    pio_sync_enable_idle_irq(cd);
}

static void
data_state_update_data(struct can2040 *cd, uint32_t data)
{
    cd->parse_crc = crcbits(cd->parse_crc, data, 8);
    cd->parse_msg.d1[cd->parse_datapos++] = data;
    if (cd->parse_datapos >= cd->parse_msg.data_len) {
        data_state_go_crc(cd);
    } else {
        unstuf_set_count(&cd->unstuf, 8);
    }
}

static void
data_state_update_crc(struct can2040 *cd, uint32_t data)
{
    if (cd->parse_crc != data) {
        pio_ack_cancel(cd);
        data_state_go_discard(cd);
        return;
    }

    cd->parse_state = MS_ACK;
    unstuf_clear_state(&cd->unstuf);
    unstuf_set_count(&cd->unstuf, 2);
}

static void
data_state_update_ack(struct can2040 *cd, uint32_t data)
{
    pio_ack_cancel(cd);
    if (data != 0x02) {
        data_state_go_discard(cd);

        // If cpu couldn't keep up for some read data then reset the pio state
        if (pio_rx_check_stall(cd)) {
            pio_sm_setup(cd);
            report_error(cd, 0);
        }
        return;
    }
    cd->parse_state = MS_EOF;
    unstuf_set_count(&cd->unstuf, 6);
}

static void
data_state_update_eof(struct can2040 *cd, uint32_t data)
{
    // The end-of-frame should have raised a bitstuff condition..
    data_state_go_discard(cd);
}

static void
data_state_update_discard(struct can2040 *cd, uint32_t data)
{
    data_state_go_discard(cd);
}

static void
data_state_update(struct can2040 *cd, uint32_t data)
{
    switch (cd->parse_state) {
    case MS_START: data_state_update_start(cd, data); break;
    case MS_DATA: data_state_update_data(cd, data); break;
    case MS_CRC: data_state_update_crc(cd, data); break;
    case MS_ACK: data_state_update_ack(cd, data); break;
    case MS_EOF: data_state_update_eof(cd, data); break;
    case MS_DISCARD: data_state_update_discard(cd, data); break;
    }
}


/****************************************************************
 * Input processing
 ****************************************************************/

static void
process_rx(struct can2040 *cd, uint32_t rx_byte)
{
    unstuf_add_bits(&cd->unstuf, rx_byte, 8);
    cd->raw_bit_count += 8;

    // undo bit stuffing
    for (;;) {
        int ret = unstuf_pull_bits(&cd->unstuf);
        if (!ret) {
            // Pulled the next field - process it
            data_state_update(cd, cd->unstuf.unstuffed_bits);
        } else if (ret > 0) {
            // Need more data
            break;
        } else {
            if (ret == -1) {
                // 6 consecutive high bits
                data_state_go_idle(cd);
            } else {
                // 6 consecutive low bits
                data_state_go_error(cd);
            }
        }
    }
}

void
can2040_dma_irq_handler(struct can2040 *cd)
{
    uint32_t dma_chan = cd->dma_chan;
    io_rw_32 *inte = cd->dma_inte, *intf = &inte[1], *ints = &inte[2];
    if (*intf & (1 << dma_chan)) {
        // Forced irq from pio irq handler
        hw_clear_bits(intf, 1 << cd->dma_chan);
        if (cd->parse_state != MS_START && pio_sync_check_idle(cd))
            data_state_go_idle(cd);
    }

    dma_channel_hw_t *ch = &dma_hw->ch[dma_chan];
    while (*ints & (1 << dma_chan)) {
        uint8_t rx_byte = readb(&cd->latest_rx);
        *ints = 1 << dma_chan;
        ch->al1_transfer_count_trig = 1;
        process_rx(cd, rx_byte);
    }
}

void
can2040_pio_irq_handler(struct can2040 *cd)
{
    uint32_t parse_state = cd->parse_state;
    if (parse_state != MS_START && pio_sync_check_idle(cd)) {
        // Force dma irq to force idle state
        io_rw_32 *inte = cd->dma_inte, *intf = &inte[1];
        hw_set_bits(intf, 1 << cd->dma_chan);
    }
}

static void
dma_setup(struct can2040 *cd)
{
    rp2040_clear_reset(RESETS_RESET_DMA_BITS);

    // Enable irqs
    io_rw_32 *inte = cd->dma_inte;
    hw_set_bits(inte, 1 << cd->dma_chan);

    // Configure dma channel
    dma_channel_hw_t *ch = &dma_hw->ch[cd->dma_chan];
    pio_hw_t *pio_hw = cd->pio_hw;
    ch->read_addr = (uint32_t)&pio_hw->rxf[1];
    ch->write_addr = (uint32_t)&cd->latest_rx;
    ch->transfer_count = 1;
    uint32_t dreq_pio = cd->pio_num ? DREQ_PIO1_RX1 : DREQ_PIO0_RX1;
    ch->ctrl_trig = (dreq_pio << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB
                     | cd->dma_chan << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB
                     | DMA_CH0_CTRL_TRIG_EN_BITS);
}


/****************************************************************
 * Transmit queuing
 ****************************************************************/

int
can2040_check_transmit(struct can2040 *cd)
{
    uint32_t tx_pull_pos = readl(&cd->tx_pull_pos);
    uint32_t tx_push_pos = cd->tx_push_pos;
    uint32_t pending = tx_push_pos - tx_pull_pos;
    return pending < ARRAY_SIZE(cd->tx_queue);
}

int
can2040_transmit(struct can2040 *cd, struct can2040_msg msg)
{
    uint32_t tx_pull_pos = readl(&cd->tx_pull_pos);
    uint32_t tx_push_pos = cd->tx_push_pos;
    uint32_t pending = tx_push_pos - tx_pull_pos;
    if (pending >= ARRAY_SIZE(cd->tx_queue))
        // Tx queue full
        return -1;

    // Copy msg into qt->msg
    struct can2040_transmit *qt = &cd->tx_queue[tx_qpos(cd, tx_push_pos)];
    qt->msg.addr = msg.addr & 0x7ff;
    uint32_t rl=msg.data_len, len = rl > 8 ? 8 : rl;
    qt->msg.data_len = len;
    qt->msg.d4[0] = qt->msg.d4[1] = 0;
    memcpy(qt->msg.d1, msg.d1, len);

    // Calculate crc and stuff bits
    memset(qt->stuffed_data, 0, sizeof(qt->stuffed_data));
    struct bitstuffer_s bs = { 1, 0, qt->stuffed_data, 0 };
    uint32_t hdr = (qt->msg.addr << 7) | len;
    bs_push(&bs, hdr, 19);
    int i;
    for (i=0; i<len; i++)
        bs_push(&bs, qt->msg.d1[i], 8);
    qt->crc = bs.crc & 0x7fff;
    bs_push(&bs, qt->crc, 15);
    bs_pushraw(&bs, 1, 1);
    qt->stuffed_words = bs_finalize(&bs);

    // Submit
    writel(&cd->tx_push_pos, tx_push_pos + 1);

    // Kick transmitter
    __disable_irq();
    if (cd->parse_state == MS_START)
        // XXX - not a good way to start tx
        tx_do_schedule(cd);
    __enable_irq();

    return 0;
}


/****************************************************************
 * Setup
 ****************************************************************/

void
can2040_setup(struct can2040 *cd, uint32_t pio_num, uint32_t dma_chan
              , uint32_t dma_irq)
{
    memset(cd, 0, sizeof(*cd));
    cd->pio_num = !!pio_num;
    cd->dma_chan = dma_chan >= NUM_DMA_CHANNELS ? NUM_DMA_CHANNELS-1 : dma_chan;
    cd->dma_irq = !!dma_irq;
    cd->dma_inte = (void*)(dma_irq ? &dma_hw->inte1 : &dma_hw->inte0);
    cd->pio_hw = cd->pio_num ? pio1_hw : pio0_hw;
}

void
can2040_callback_config(struct can2040 *cd, can2040_rx_cb rx_cb)
{
    cd->rx_cb = rx_cb;
}

void
can2040_start(struct can2040 *cd, uint32_t sys_clock, uint32_t bitrate
              , uint32_t gpio_rx, uint32_t gpio_tx)
{
    cd->gpio_rx = gpio_rx;
    cd->gpio_tx = gpio_tx;
    pio_setup(cd, sys_clock, bitrate);
    data_state_go_discard(cd);
    dma_setup(cd);
}

void
can2040_shutdown(struct can2040 *cd)
{
    // XXX
}