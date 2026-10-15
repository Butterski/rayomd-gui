// SPDX-License-Identifier: Apache-2.0
// The command line: `rayomd-gui [--version | --snapshot <out.png>] [file]`.
#pragma once

#include <string>
#include <vector>

namespace rgui {

struct Args {
    enum class Mode {
        Run,       // open the window, with `file` when given
        Version,   // print `rayomd-gui <VERSION>`, exit 0
        Snapshot,  // render `file` once, write `snapshotPath`, exit
        Usage,     // bad arguments: print kUsage to stderr, exit 2
    };
    static constexpr const char* kUsage = "Usage: rayomd-gui [--version | --snapshot <out.png>] [file]";
    static constexpr int kUsageExitCode = 2;

    Mode mode = Mode::Run;
    std::string snapshotPath;  // UTF-8
    std::string file;          // UTF-8; empty for none
};

// Parses the arguments after the program name (UTF-8). `--version` stands alone; `--snapshot`
// takes an output path and an optional file; otherwise one optional path. Any other argument
// beginning with `-`, a missing snapshot path or a second file is Usage.
Args ParseArgs(const std::vector<std::string>& arguments);

} // namespace rgui
