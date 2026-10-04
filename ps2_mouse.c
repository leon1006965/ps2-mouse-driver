// PS/2 Mouse Driver — Standalone, freestanding, zero-dependency
// Tested on: QEMU, Dell Latitude (Synaptics touchpad), HP Pavilion dv5
//
// This driver works on real hardware. Key features:
// - Remainder accumulators for tiny delta movements (±1 px still counts)
// - EOI always sent in IRQ handler (prevents IRQ12 lockup)
// - Controller wait before every PS/2 write (prevents dropped bytes)
// - Config byte re-read before enabling IRQ12
// - ACK timeouts prevent hangs on missing mouse
// - Mouse reset before initialization
// - Keyboard clock bit cleared in config
// - Polling fallback when IRQ12 doesn't fire
//
// Build with: -ffreestanding -nostdlib -mno-red-zone -fno-stack-protector
//
// Usage:
//   1. Call ps2_mouse_init() once during kernel boot
//   2. Register ps2_mouse_irq_handler() as your IRQ12 handler (vector 0x2C)
//   3. Unmask IRQ2 (cascade) on master PIC and IRQ12 on slave PIC
//   4. Call ps2_mouse_get_x/y/buttons() to read positions

#include "ps2_mouse.h"

// --- Port I/O (replace with your own inline asm or kernel primitives) ---
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; asm volatile("inb %1,%0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void io_wait(void) { outb(0x80, 0); }

#define PS2_TIMEOUT 100000

// --- PS/2 controller primitives ---

static int ps2_wait_write(void) {
    for (int i = 0; i < PS2_TIMEOUT; i++)
        if (!(inb(0x64) & 2)) return 0;
    return -1;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < PS2_TIMEOUT; i++)
        if (inb(0x64) & 1) return 0;
    return -1;
}

static int ps2_read(void) {
    if (ps2_wait_read() != 0) return -1;
    return inb(0x60);
}

static void ps2_write(uint16_t port, uint8_t val) {
    ps2_wait_write();
    outb(port, val);
}

static void ps2_flush(void) {
    for (int i = 0; i < 16 && (inb(0x64) & 1); i++) inb(0x60);
}

// Send command to mouse, wait for ACK (0xFA). Returns 0 on success.
static int mouse_cmd(uint8_t val) {
    for (int retry = 0; retry < 3; retry++) {
        ps2_write(0x64, 0xD4);   // tell controller: next byte goes to mouse
        io_wait();
        ps2_write(0x60, val);
        int r = ps2_read();
        if (r == 0xFA) return 0;  // ACK
        if (r != 0xFE) return -1; // not a resend request
    }
    return -1;
}

// --- Mouse state ---

static volatile int mouse_x = 40;
static volatile int mouse_y = 12;
static volatile uint8_t mouse_buttons = 0;
static volatile uint8_t packet[3];
static volatile int packet_idx = 0;
static int acc_x = 0, acc_y = 0;
static int mouse_present = 0;

// --- Initialization ---

int ps2_mouse_init(void) {
    // Flush any stale bytes from PS/2 controller
    ps2_flush();

    // Disable keyboard during setup (command 0xAD)
    ps2_write(0x64, 0xAD);

    // Enable auxiliary device (command 0xA8)
    ps2_write(0x64, 0xA8);
    ps2_flush();

    // Read controller config byte
    ps2_write(0x64, 0x20);
    int c = ps2_read();
    if (c < 0) {
        ps2_write(0x64, 0xAE); // re-enable keyboard
        return -1;
    }
    uint8_t config = (uint8_t)c;

    // Modify config:
    //   bit 1 = 1  → enable IRQ12 (mouse interrupt)
    //   bit 4 = 0  → enable keyboard clock
    //   bit 5 = 0  → enable mouse clock
    config |= 0x02;
    config &= ~0x10;
    config &= ~0x20;

    ps2_write(0x64, 0x60);   // tell controller: next byte is config
    ps2_write(0x60, config);

    // Reset mouse (0xFF) — optional but recommended
    // Returns FA (ACK) then AA (self-test pass) then device ID
    if (mouse_cmd(0xFF) == 0) {
        int st = -1;
        for (int i = 0; i < 20 && st < 0; i++) st = ps2_read();
        if (st == 0xAA) ps2_read(); // consume device ID byte
    }
    ps2_flush();

    // Set Defaults (0xF6)
    mouse_cmd(0xF6);

    // Enable Data Reporting (0xF4) — this is the critical step
    if (mouse_cmd(0xF4) != 0) {
        ps2_write(0x64, 0xAE); // re-enable keyboard
        return -1;
    }
    ps2_flush();

    // Re-read config byte (keyboard driver may have changed it)
    ps2_write(0x64, 0x20);
    c = ps2_read();
    if (c >= 0) {
        config = (uint8_t)c;
        config |= 0x02;  // ensure IRQ12 is enabled
        ps2_write(0x64, 0x60);
        ps2_write(0x60, config);
    }

    // Re-enable keyboard (command 0xAE)
    ps2_write(0x64, 0xAE);

    packet_idx = 0;
    mouse_present = 1;
    return 0;
}

// --- Packet Processing ---
// PS/2 mouse sends 3-byte packets:
//   Byte 0: [yo xo ys xs 1 bm br bl]
//     yo = Y overflow, xo = X overflow
//     ys = Y sign bit (9th bit), xs = X sign bit (9th bit)
//     1   = always set (sync bit)
//     bm  = middle button, br = right, bl = left
//   Byte 1: X-Axis movement (8-bit)
//   Byte 2: Y-Axis movement (8-bit)
//
// The 9-bit signed value is computed as:
//   rel_x = byte1 - ((byte0 << 4) & 0x100)
//   rel_y = byte2 - ((byte0 << 3) & 0x100)

static void process_packet(void) {
    uint8_t s = packet[0];

    // Discard packets with overflow flags set
    if (s & 0xC0) return;

    // Compute 9-bit signed deltas
    int dx = packet[1] - ((s << 4) & 0x100);
    int dy = -(packet[2] - ((s << 3) & 0x100));

    mouse_buttons = s & 0x07;

    // Scale to text cells using remainder accumulators.
    // This preserves small movements (±1) that integer division would lose.
    acc_x += dx;
    mouse_x += acc_x / 8;
    acc_x %= 8;

    acc_y += dy;
    mouse_y += acc_y / 16;
    acc_y %= 16;

    // Clamp to screen bounds
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x > 79) mouse_x = 79;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y > 24) mouse_y = 24;
}

// Feed a single byte into the packet state machine
static void feed(uint8_t data) {
    if (packet_idx == 0) {
        // Byte 0 must have bit 3 set (sync bit) to start a new packet
        if (!(data & 0x08)) return;
        packet[0] = data;
        packet_idx = 1;
    } else if (packet_idx == 1) {
        packet[1] = data;
        packet_idx = 2;
    } else {
        packet[2] = data;
        packet_idx = 0;
        process_packet();
    }
}

// Drain all pending mouse bytes from the PS/2 controller
static void drain(void) {
    for (int guard = 0; guard < 64; guard++) {
        if ((inb(0x64) & 0x21) != 0x21) break;  // OBF=1 AND aux=1
        feed(inb(0x60));
    }
}

// --- IRQ Handler ---
// Call this from your IRQ12 handler (vector 0x2C after PIC remap).
// IMPORTANT: Must always send EOI to BOTH PICs, even if no data was read.
// Missing EOI on the slave PIC blocks IRQ12 permanently.

void ps2_mouse_irq_handler(void) {
    drain();
    outb(0xA0, 0x20);  // EOI to slave PIC
    outb(0x20, 0x20);  // EOI to master PIC
}

// --- Polling Fallback ---
// Call this from your main loop or before reading mouse position.
// Drains any pending bytes when IRQ12 doesn't fire (common on some hardware).
// Saves/restores interrupt flags to avoid breaking critical sections.

void ps2_mouse_poll(void) {
    uint32_t flags;
    asm volatile("pushfl; pop %0; cli" : "=r"(flags) :: "memory");
    drain();
    asm volatile("push %0; popfl" : : "r"(flags) : "memory", "cc");
}

// --- Public API ---

int ps2_mouse_get_x(void) { ps2_mouse_poll(); return mouse_x; }
int ps2_mouse_get_y(void) { ps2_mouse_poll(); return mouse_y; }
uint8_t ps2_mouse_get_buttons(void) { ps2_mouse_poll(); return mouse_buttons; }
int ps2_mouse_is_present(void) { return mouse_present; }
void ps2_mouse_set_pos(int x, int y) { mouse_x = x; mouse_y = y; }
