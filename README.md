To compile and create an executable
gcc main_withgif.c -o giftest `pkg-config --cflags --libs gtk+3.0`
./giftest
