#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

#include <stdint.h>

// Public API
int  ps2_mouse_init(void);
int  ps2_mouse_get_x(void);
int  ps2_mouse_get_y(void);
uint8_t ps2_mouse_get_buttons(void);
int  ps2_mouse_is_present(void);
void ps2_mouse_set_pos(int x, int y);

// ISR handler — call this from your IRQ12 handler
void ps2_mouse_irq_handler(void);

#endif
