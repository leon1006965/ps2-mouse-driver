# PS/2 Mouse Driver for Hobby Operating Systems

A standalone, freestanding PS/2 mouse driver that works on **real hardware** — not just QEMU. Tested on Dell Latitude (Synaptics touchpad) and HP Pavilion dv5.

## What makes this different

Most PS/2 mouse drivers fail on real hardware because they miss one of these critical details:

| Problem | This driver's solution |
|---------|----------------------|
| Tiny movements (±1px) get lost to integer division | Remainder accumulators preserve every pixel |
| IRQ12 locks up after first packet | EOI always sent, no early returns |
| Dropped bytes on slow i8042 controllers | Wait for input buffer before every write |
| Blind config byte reads corrupt controller | Re-read config before modifying |
| No ACK on missing mouse hangs boot | Every command has a timeout |
| Stale controller state causes random behavior | Mouse reset (0xFF) before init |
| Keyboard bytes consumed as mouse data | Aux bit check before reading port 0x60 |
| Polling breaks interrupt state | `pushfl`/`popfl` saves/restores EFLAGS |

## Files

| File | Purpose |
|------|---------|
| `ps2_mouse.h` | Public API header |
| `ps2_mouse.c` | Driver implementation |

## Quick Start

### 1. Include the driver

```c
#include "ps2_mouse.h"
```

### 2. Initialize during kernel boot

Call `ps2_mouse_init()` after your IDT and PIC are set up, but **before** enabling interrupts (`sti`):

```c
void kernel_main(void) {
    idt_install();       // your IDT setup
    pic_init();          // your PIC setup (remap to 0x20/0x28)

    ps2_mouse_init();    // initialize PS/2 mouse

    // Unmask IRQ12 (mouse) on the slave PIC
    // IRQ12 is bit 4 on port 0xA1
    outb(0xA1, inb(0xA1) & ~(1 << 4));

    // Unmask IRQ2 (cascade) on the master PIC
    // IRQ12 goes through the cascade pin (IRQ2) on the master PIC
    outb(0x21, inb(0x21) & ~(1 << 2));

    sti_enable();        // enable interrupts
    shell_run();         // your main loop
}
```

### 3. Register the IRQ handler

Your IRQ12 handler (vector 0x2C after PIC remap) must call `ps2_mouse_irq_handler()`:

```asm
global isr12
isr12:
    pusha
    call ps2_mouse_irq_handler
    popa
    mov al, 0x20
    out 0xA0, al    ; EOI to slave PIC
    out 0x20, al    ; EOI to master PIC
    iret
```

**Important:** The handler must send EOI to BOTH PICs. Missing EOI on the slave PIC blocks IRQ12 permanently.

### 4. Read mouse position in your main loop

```c
void shell_run(void) {
    while (1) {
        int x = ps2_mouse_get_x();
        int y = ps2_mouse_get_y();
        uint8_t btns = ps2_mouse_get_buttons();

        // Draw cursor at (x, y)
        draw_cursor(x, y, btns);

        // Also poll (for when IRQ12 doesn't fire)
        ps2_mouse_poll();

        // Process keyboard input...
    }
}
```

### 5. Build flags

```
-ffreestanding -nostdlib -mno-red-zone -fno-stack-protector
```

The `-mno-red-zone` flag is required because `ps2_mouse_poll()` uses `pushfl`/`popfl` which touch the stack.

## Architecture

```
Kernel Boot
    │
    ├── idt_install()     ← Set up IDT, remap PIC to 0x20/0x28
    ├── pic_init()        ← Configure PIC (mask all initially)
    ├── ps2_mouse_init()  ← Detect and initialize mouse
    │   ├── Flush stale bytes from output buffer
    │   ├── Disable keyboard (0xAD) during setup
    │   ├── Enable auxiliary device (0xA8)
    │   ├── Read and modify config byte
    │   │   ├── bit 1 = 1 (enable IRQ12)
    │   │   ├── bit 4 = 0 (enable keyboard clock)
    │   │   └── bit 5 = 0 (enable mouse clock)
    │   ├── Reset mouse (0xFF) → ACK + self-test
    │   ├── Set Defaults (0xF6)
    │   ├── Enable Data Reporting (0xF4) → must ACK
    │   ├── Re-read config byte, enable IRQ12
    │   └── Re-enable keyboard (0xAE)
    ├── pic_unmask_irq12() ← Unmask IRQ12 on slave PIC
    ├── pic_unmask_irq2()  ← Unmask cascade on master PIC
    ├── sti_enable()       ← Enable interrupts
    │
    └── Main Loop
        ├── ps2_mouse_poll() ← Drain pending bytes (polling fallback)
        ├── ps2_mouse_get_x()  ← Returns mouse X position
        ├── ps2_mouse_get_y()  ← Returns mouse Y position
        ├── ps2_mouse_get_buttons() ← Returns button state
        │
        └── IRQ12 fires → ps2_mouse_irq_handler()
            ├── drain() ← Read all pending bytes from port 0x60
            ├── process_packet() ← Decode 3-byte packets
            └── EOI to slave PIC + master PIC
```

## PS/2 Protocol Reference

### Initialization Sequence

```
Host → Mouse:  0xFF        Reset
Mouse → Host:  0xFA        ACK
Mouse → Host:  0xAA        Self-test passed
Mouse → Host:  0x00        Device ID (standard mouse)

Host → Mouse:  0xF6        Set Defaults
Mouse → Host:  0xFA        ACK

Host → Mouse:  0xF4        Enable Data Reporting
Mouse → Host:  0xFA        ACK
```

### Packet Format (3-byte standard mouse)

```
Byte 0:  [yo] [xo] [ys] [xs] [1] [bm] [br] [bl]
           │    │    │    │   │    │    │    │
           │    │    │    │   │    │    │    └── Left button
           │    │    │    │   │    │    └── Right button
           │    │    │    │   │    └── Middle button
           │    │    │    │   └── Always 1 (sync bit)
           │    │    │    └── X sign bit (9th bit)
           │    │    └── Y sign bit (9th bit)
           │    └── X overflow
           └── Y overflow

Byte 1:  X-Axis movement (8-bit unsigned)
Byte 2:  Y-Axis movement (8-bit unsigned)
```

### Signed Delta Computation

```c
// The movement values are 9-bit signed, not 8-bit:
int dx = byte1 - ((byte0 << 4) & 0x100);  // X sign = bit 4
int dy = byte2 - ((byte0 << 3) & 0x100);  // Y sign = bit 5

// When sign bit is set, subtract 256 to get negative value:
//   byte1=0x05, bit4=0 → dx = 5 - 0 = +5
//   byte1=0x05, bit4=1 → dx = 5 - 256 = -251
```

### PIC Remap (required)

```
Master PIC (port 0x20/0x21):
  IRQ0-7  → INT 0x20-0x27

Slave PIC (port 0xA0/0xA1):
  IRQ8-15 → INT 0x28-0x2F

IRQ12 (mouse) → INT 0x2C (0x28 + 4)
IRQ2 (cascade) → must be unmasked on master PIC
```

### Config Byte (port 0x20, read via 0x20 command)

```
Bit 0: IRQ0 (timer) enabled
Bit 1: IRQ1 (keyboard) enabled
Bit 2: IRQ2 (cascade) enabled
Bit 3: IRQ3 enabled
Bit 4: IRQ4 enabled
Bit 5: IRQ5 enabled
Bit 6: IRQ6 enabled
Bit 7: IRQ7 enabled
```

For mouse init, we want:
- Bit 1 = 1 (enable IRQ12)
- Bit 4 = 0 (enable keyboard clock)
- Bit 5 = 0 (enable mouse clock)

## How to tune for your hardware

| Symptom | Fix |
|---------|-----|
| Cursor too fast | Increase divisor (change `/ 8` to `/ 12` for X, `/ 16` to `/ 24` for Y) |
| Cursor too slow | Decrease divisor |
| Cursor doesn't move at all | Check if IRQ12 is unmasked; try `ps2_mouse_poll()` in a loop |
| Cursor moves erratically | Check for PS/2 buffer overflow; add `ps2_flush()` calls |
| System reboots on boot | Check that EOI is sent to BOTH PICs in the IRQ handler |
| Keyboard stops working | Config byte may have disabled keyboard clock; check bit 4 |

## Integration Checklist

- [ ] PS/2 controller ports: `inb`/`outb` for ports 0x60, 0x64, 0x80
- [ ] IDT set up with vector 0x2C (44) pointing to your IRQ handler
- [ ] PIC remapped: master → 0x20, slave → 0x28
- [ ] IRQ12 unmasked on slave PIC (port 0xA1, bit 4)
- [ ] IRQ2 unmasked on master PIC (port 0x21, bit 2)
- [ ] `sti` called after all initialization
- [ ] `ps2_mouse_poll()` called in your main loop (polling fallback)
- [ ] Build with `-mno-red-zone`

## License

MIT

## Credits

Based on the [OSDev Wiki PS/2 Mouse Interface](https://wiki.osdev.org/PS/2_Mouse_Interface) and hardened through real-hardware testing on Dell and HP laptops.
