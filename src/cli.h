#pragma once

#include <QStringList>

// Commands that need no display. Each exits 0 or 1 and prints errors on
// stderr; `status --json` is for scripts and agents.
namespace cli {
// True when `args` (without the program name) is a command-line command.
bool handles(const QStringList &args);
int run(int argc, char *argv[]);
} // namespace cli
