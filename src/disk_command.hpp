#pragma once

#include <span>

// `ultraviolent disk ...`: copy-on-write overlays (ORCHESTRATION.adoc "O1"). `arguments`
// follow the word "disk". Returns the process's exit status.
int disk_command(std::span<char*> arguments);
