#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#ifndef EDEN_SERVO_SHELL_EXECUTABLE
#error EDEN_SERVO_SHELL_EXECUTABLE is required
#endif

int main(int argc, char *argv[]) {
    const std::string executable = EDEN_SERVO_SHELL_EXECUTABLE;
    if (!std::filesystem::is_regular_file(executable)) {
        std::cerr << "ServoShell executable not found at " << executable << '\n';
        return 1;
    }

    std::vector<std::string> arguments;
    arguments.emplace_back(executable);
    if (argc == 1) {
        arguments.emplace_back("https://browserbench.org/Speedometer3.1/");
    } else {
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
    }

    std::vector<char *> nativeArguments;
    nativeArguments.reserve(arguments.size() + 1);
    for (std::string &argument : arguments) {
        nativeArguments.push_back(argument.data());
    }
    nativeArguments.push_back(nullptr);
    execv(executable.c_str(), nativeArguments.data());
    std::cerr << "Unable to launch ServoShell: " << std::strerror(errno) << '\n';
    return 1;
}
