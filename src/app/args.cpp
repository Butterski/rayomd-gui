// SPDX-License-Identifier: Apache-2.0
#include "app/args.h"

namespace rgui {
namespace {

bool IsOption(const std::string& argument) {
    return !argument.empty() && argument[0] == '-';
}

Args Usage() {
    Args args;
    args.mode = Args::Mode::Usage;
    return args;
}

} // namespace

Args ParseArgs(const std::vector<std::string>& arguments) {
    Args args;
    size_t i = 0;
    if (i < arguments.size() && arguments[i] == "--version") {
        if (arguments.size() != 1) return Usage();
        args.mode = Args::Mode::Version;
        return args;
    }
    if (i < arguments.size() && arguments[i] == "--snapshot") {
        i++;
        if (i >= arguments.size() || arguments[i].empty() || IsOption(arguments[i])) return Usage();
        args.mode = Args::Mode::Snapshot;
        args.snapshotPath = arguments[i++];
    }
    if (i < arguments.size()) {
        if (arguments[i].empty() || IsOption(arguments[i])) return Usage();
        args.file = arguments[i++];
    }
    if (i < arguments.size()) return Usage();
    return args;
}

} // namespace rgui
