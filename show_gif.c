#include <stdlib.h>

int main() {
    system("mpv --no-audio --fs --loop=inf --really-quiet gameover.gif & sleep 5; pkill -f 'mpv.*gameover.gif'");
    return 0;
}
