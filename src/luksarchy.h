#ifndef LUKSARCHY_H
#define LUKSARCHY_H

/* Feature macros must come before any system headers */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <ctype.h>

#define CHROOT_PATH "/mnt"
#define MAX_CMD_SIZE 4096
#define LOG_PATH "/tmp/luksarchy-install.log"

/* ANSI helpers used by the TUI */
#define ANSI_RESET      "\033[0m"
#define ANSI_WHITE      "\033[37m"
#define ANSI_GRAY       "\033[90m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE_BOLD  "\033[1;34m"
#define ANSI_CURSOR_POS "\033[%d;%dH"

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} Log_Level;

#define LOG_DEBUG(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_ERROR, __VA_ARGS__)

#define CHECK_OR_FAIL(cond, msg) do { \
    if (!(cond)) { \
        LOG_ERROR("%s", (msg)); \
        show_message((msg)); \
        return 0; \
    } \
} while (0)

typedef struct {
    const char *label;
    const char *value;
    const char *default_display;
    int is_password;
} Tui_Field;

typedef enum {
    INPUT_TEXT = 0,
    INPUT_PASSWORD,
    INPUT_FZF_KEYMAP,
    INPUT_FZF_TIMEZONE
} Input_Type;

typedef struct {
    char *dest;
    const char *default_val;
    Input_Type type;
    int cursor_offset;
    const char *error_msg;
} Form_Field;

typedef struct {
    const char *filename;
    const char *content;
    mode_t permissions;
} Dotfile;

typedef struct {
    const char *key;
    const char *value;
} Config_Entry;

typedef struct {
    const char *unit;           /* e.g. "getty@tty1.service" */
    const char *drop_in_dir;    /* e.g. "getty@tty1.service.d" */
    const char *drop_in_file;   /* e.g. "autologin.conf" */
    Config_Entry *entries;
    size_t entry_count;
} Systemd_Override;

/* Logging */
void logger_init(const char *log_path);
void logger_close(void);
void log_msg(Log_Level level, const char *fmt, ...);

/* File / dir helpers */
int write_file(const char *path, const char *content);
int write_file_fmt(const char *path, const char *fmt, ...);
int set_file_perms(const char *path, mode_t mode, const char *owner, const char *group);
int create_directory(const char *path, mode_t mode);

/* Chroot helpers */
int chroot_exec(const char *cmd);
int chroot_exec_fmt(const char *fmt, ...);
int chroot_exec_as_user(const char *username, const char *cmd);
int chroot_exec_as_user_fmt(const char *username, const char *fmt, ...);
int git_clone_as_user(const char *username, const char *repo_url, const char *dest_path);
int make_clean_install(const char *build_dir);

/* Dotfiles / systemd */
int create_user_dotfile(const char *username, const Dotfile *dotfile);
int setup_systemd_override(const Systemd_Override *override);

/* TUI */
void show_message(const char *message);

#endif /* LUKSARCHY_H */
