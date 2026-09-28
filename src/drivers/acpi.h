#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// Корневой указатель ACPI
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed)) rsdp_t;

// Стандартный заголовок любой таблицы ACPI
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oemtableid[8];
    uint32_t oemrevision;
    uint32_t creatorid;
    uint32_t creatorrevision;
} __attribute__((packed)) acpi_header_t;

// FADT (Fixed ACPI Description Table) - содержит порты питания
typedef struct {
    acpi_header_t h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_power_management;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block; // <--- Порт, который мы ищем
    uint32_t pm1b_control_block;
} __attribute__((packed)) fadt_t;

void acpi_power_off(void);

#endif