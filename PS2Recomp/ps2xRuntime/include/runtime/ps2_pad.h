#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

// Runtime menus (save slot picker): hide the pad from the game while shown (it is given back once
// every button is released), and read the host's pressed buttons (active-high, PS2 bit order).
void ps2PadSetBlocked(bool blocked);
uint16_t ps2PadHostPressed();

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
};

#endif
