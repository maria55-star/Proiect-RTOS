/* Memoria pentru Netduino/STM32F4-like */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Varful stivei */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* Vector table la începutul flash-ului */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Cod și rodata */
    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* Date inițializate (.data) – în RAM, dar imaginea în FLASH */
    .data : AT(ADDR(.text) + SIZEOF(.text))
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM

    /* Adresa din FLASH de unde copiem .data */
    _sidata = LOADADDR(.data);

    /* Date neinițializate (.bss) */
    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM
}