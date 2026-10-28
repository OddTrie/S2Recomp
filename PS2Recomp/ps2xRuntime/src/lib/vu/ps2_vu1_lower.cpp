#include "ps2_vu1_lower.inl"

void VU1Core::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    execLowerInline(instr, vuData, dataSize, gs, memory, upperInstr);
}
