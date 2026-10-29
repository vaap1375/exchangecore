#include "allocation_counter.hpp"

#include <iostream>

int main(int argc, char** argv) {
    return exchangecore::run_benchmark_cli(argc - 1, argv + 1, std::cout, std::cerr,
                                         exchangecore::standalone_allocation_probe());
}
