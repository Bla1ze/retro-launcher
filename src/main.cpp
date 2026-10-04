#include "Launcher.h"
#include "Player.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

// One binary, two modes, handed off by exec so the process ID the firmware's
// launcher waits on stays alive the whole time:
//   (no args)                                          menu, app dir = working directory
//   --menu <appDir> <system> <index> <message>          menu, resuming after a game
//   --play <appDir> <system> <rom> <screen> <index> [<returnTo>]
//                                                       one game, then back to the menu
int main(int argc, char** argv)
{
    if (argc >= 7 && std::strcmp(argv[1], "--play") == 0) {
        return runPlayer(argv[2], argv[3], argv[4], argv[5], std::atoi(argv[6]), argc >= 8 ? argv[7] : argv[3]);
    }
    if (argc >= 6 && std::strcmp(argv[1], "--menu") == 0) {
        return runMenu(argv[2], argv[3], std::atoi(argv[4]), argv[5]);
    }
    char cwd[1024] = {0};
    if (!getcwd(cwd, sizeof(cwd))) std::strcpy(cwd, ".");
    return runMenu(cwd, "", 0, "");
}
