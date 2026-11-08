#pragma once

#include <Arduino.h>

// Runtime health diagnostics, sport-agnostic.
//
// resetReasonName() reports why the board last restarted ("panic",
// "task-watchdog", "brownout", ...). It's in /status and on the portal's
// diagnostics page so a board that rebooted on its own can be spotted
// without a USB cable (pair it with uptimeSec).
//
// lastCrashJson() summarizes the crash stored in the coredump flash
// partition (the panic handler writes one on every crash; it survives
// reboots until the next crash overwrites it): task, PC, exception cause
// and the on-device backtrace, plus whether it came from THIS firmware
// build. Decode the addresses with xtensa-esp32s3-elf-addr2line against
// the ELF of the build whose hash matches "elf".
//
// serviceHealthHeartbeat() prints a heap + task-stack line every
// HEALTH_HEARTBEAT_MS in SB_DEBUG builds and compiles to nothing otherwise.
// Call it from a core-0 task loop (it paces itself) — never the render
// loop, where a ~25 ms Serial.printf stalls the displays.
namespace health {

const char* resetReasonName();

// JSON object, or "null" when no crash is stored. The flash read happens
// once (first call); call it from setup() so later calls never touch flash.
String lastCrashJson();

void serviceHealthHeartbeat();

}  // namespace health
