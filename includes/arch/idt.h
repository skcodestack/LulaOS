#ifndef __IDT_H__
#define __IDT_H__

#include <stdint.h>
#include <linkage.h>

#define IDT_RESERVED(x) (x)              // reserved must be set 0
#define IDT_GATE_TYPE(x) (x << 8)        // gate type
#define IDT_RESERVED2(x) (x << 12)       // reserved must be set 0
#define IDT_DPL(x) (x << 13)             // descriptor privilege level 0-3
#define IDT_PRESENT(x) (x << 15)         // present must be set 1
#define IDT_OFFSET_H(x) (x & 0xFFFF0000) // interrupt service routine address

#define IDT_OFFSET_L(x) (x & 0x0000FFFF)
#define IDT_SEGMENT_SELECTOR(x) ((x & 0x0000FFFF) << 16) // segment selector

#define IDT_TYPE_TASK 0x05           // task gate, ofsset is must be 0
#define IDT_TYPE_INTERRUPT_16BIT 0x6 // 16bit interrupt gate
#define IDT_TYPE_TRAP_16BIT 0x7      // 16bit trap gate
#define IDT_TYPE_INTERRUPT_32BIT 0xE // 32bit interrupt gate
#define IDT_TYPE_TRAP_32BIT 0xF      // 32bit trap gate

#define IDT_TYPE_ATTR_TASK_GATE IDT_RESERVED(0) | IDT_GATE_TYPE(IDT_TYPE_TASK) | IDT_RESERVED2(0) | IDT_DPL(0) | IDT_PRESENT(1)
#define IDT_TYPE_ATTR_INTERRUPT_GATE_32BIT IDT_RESERVED(0) | IDT_GATE_TYPE(IDT_TYPE_INTERRUPT_32BIT) | IDT_RESERVED2(0) | IDT_DPL(0) | IDT_PRESENT(1)
#define IDT_TYPE_ATTR_TRAP_GATE_32BIT IDT_RESERVED(0) | IDT_GATE_TYPE(IDT_TYPE_TRAP_32BIT) | IDT_RESERVED2(0) | IDT_DPL(0) | IDT_PRESENT(1)
#define IDT_TYPE_ATTR_SYSTEM_GATE_32BIT IDT_RESERVED(0) | IDT_GATE_TYPE(IDT_TYPE_TRAP_32BIT) | IDT_RESERVED2(0) | IDT_DPL(3) | IDT_PRESENT(1)


extern uint64_t _idt[]; 

void _set_task_gate_entry(uint8_t index, uint16_t selector);

void _set_interrupt_gate_entry(uint8_t index, uint16_t selector, uint32_t offset);

void _set_trap_gate_entry(uint8_t index, uint16_t selector, uint32_t offset);

void _set_system_gate_entry(uint8_t index, uint16_t selector, uint32_t offset);

void _init_idt();
#endif