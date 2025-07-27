#include "Shell.h"
#include <iostream>

int main() {
    try {
        Shell app_shell;
        app_shell.run();
    } catch (const std::exception& e) {
        std::cerr << "A critical error occurred during initialization: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}