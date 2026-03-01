// main.cpp — Entry point for Talos
//
// This is where the program starts. It creates the engine,
// initializes everything, runs the game loop, and handles errors.

#include "Engine.h"
#include "Logger.h"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        Talos::Engine engine;
        engine.init();
        engine.run();
    } catch (const std::exception& e) {
        // If anything goes wrong, print the error
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
