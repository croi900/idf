#include <hpx/hpx_init.hpp>
#include <hpx/include/parallel_for_loop.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/async.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/include/threads.hpp>
#include <hpx/semaphore.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <iomanip>
#include <thread>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <memory>

#include "src/actions.h"
#include "src/cli.h"
#include "src/dirtree.hpp"
#include "src/file_processor.hpp"
#include "src/tokenizer.hpp"
#include "src/sharding.hpp"
#include "src/db_manager.hpp"
#include "src/gui.h"
#include "src/index.h"
#include "src/search.h"


int main(int argc, char *argv[]) {

    int selection;
    std::cout << "Please select what you want to do:\n";
    std::cout << "0: Reindex DB\n";
    std::cout << "1: CLI\n";
    std::cout << "2: GUI\n";

    std::cin >> selection;
    idf::action action = static_cast<idf::action>(selection);
    idf::cli();
    switch (action) {
        case idf::action::reindex:
            idf::reindex_db();
            break;
        case idf::action::cli:
            idf::cli();
            break;
        case idf::action::interface:
            idf::gui();
        default:
            break;
    }




    return 0;
}
