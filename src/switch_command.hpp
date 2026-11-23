#pragma once

#include <span>

// `ultraviolent switch SOCKET [--tap IFNAME]`: a virtual Ethernet switch for machines on
// one host (ORCHESTRATION.adoc "O2"). `arguments` follow the word "switch". Returns the
// process's exit status.
int switch_command(std::span<char*> arguments);
