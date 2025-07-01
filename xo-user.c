#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("\n\nStopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("\n\nStopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

static void render_board(uint32_t frame)
{
    static const char symbol[4] = " XO?"; /* 0:空 1:X 2:O 3:? */
    /* 第3行第1欄；第1行 = time */
    printf("\033[9;1H\033[0J\n");
    fflush(stdout);

    for (int row = 0; row < 3; row++) {
        puts("+---+---+---+"); /* 橫線 */

        /* 一行三格 */
        printf("| %c | %c | %c |\n",
               symbol[(frame >> (row * 6 + 0)) & 0x3],  /* 第 row*3 + 0 格 */
               symbol[(frame >> (row * 6 + 2)) & 0x3],  /* 第 row*3 + 1 格 */
               symbol[(frame >> (row * 6 + 4)) & 0x3]); /* 第 row*3 + 2 格 */
    }
    puts("+---+---+---+"); /* 底線 */
}

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // char display_buf[DRAWBUFFER_SIZE];

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (FD_ISSET(device_fd, &readset)) {
            uint8_t buf[1024];
            ssize_t got = read(device_fd, &buf, sizeof(buf));

            if (got == 4) {
                uint32_t frame = le32toh(*(uint32_t *) buf);

                /* time set */
                time_t now = time(NULL);
                struct tm tm_info;
                localtime_r(&now, &tm_info);
                char tbuf[32];
                strftime(tbuf, sizeof(tbuf), "Time: %Y-%m-%d %H:%M:%S",
                         &tm_info);
                printf("\033[1;1H\033[2K%s", tbuf);  // 跑到(1,1),印時間
                printf("\033[0J");
                fflush(stdout);
                /* time set */

                /* bitmask轉ASCII  */
                render_board(frame);

                fflush(stdout);
            } else if (got > 0) { /* 任意長度文字 */
                fwrite(buf, 1, got, stdout); /* 直接印出 */
                fflush(stdout);              /* 立即顯示 */
            } else if (got == 0) {
                /* ctrl-Q 離開迴圈 */
                break;
            } else {
                /* read被打斷 */
            }
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
