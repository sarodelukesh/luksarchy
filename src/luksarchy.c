#include "luksarchy.h"
#include <string.h>
#include <ctype.h>

static FILE *log_file = NULL;
static const char *level_strings[] = {"DEBUG", "INFO", "WARN", "ERROR"};
static struct termios orig_termios;

static void part_path(char *out, size_t size, const char *disk, int part) {
    if (isdigit((unsigned char)disk[strlen(disk) - 1])) {
        snprintf(out, size, "/dev/%sp%d", disk, part);
    } else {
        snprintf(out, size, "/dev/%s%d", disk, part);
    }
}



enum Terminal_Size {
    TERM_SMALL = 0,
    TERM_MEDIUM = 1,
    TERM_LARGE = 2
};

enum Install_Mode {
    INSTALL_WHOLE_DISK = 0,
    INSTALL_FREE_SPACE = 1
};

typedef struct {
    enum Install_Mode mode;
    char disk[64];             /* "sda" / "nvme0n1" (no /dev/ prefix) */
    char free_start[32];       /* raw 512-byte sector number - only for FREE_SPACE */
    char free_end[32];         /* raw 512-byte sector number - only for FREE_SPACE */
    int  reuse_efi;            /* 1 if using existing ESP (FREE_SPACE w/ UEFI) */
    char efi_part[64];         /* e.g. "/dev/sda1" or "/dev/nvme0n1p1" */
    char root_part[64];        /* filled in by partition_disk() */
} Partition_Plan;

/*
 * Single Hyprland stack. No XFCE / OXWM / picom / Xorg.
 *
 * grub + efibootmgr are included so pacstrap installs them from the offline
 * local repo (/repo) during installation. They must NOT be downloaded at
 * runtime inside install_bootloader().
 *
 * ly is intentionally NOT listed here: it is not reliably present in the
 * offline /repo, and a missing package in pacstrap aborts the ENTIRE install
 * (all-or-nothing). We install ly opportunistically in install_ly() and treat
 * its absence as a soft warning.
 */
static const char *HYPRLAND_PACKAGES =
    "base base-devel linux linux-firmware linux-headers "
    "networkmanager git vim neovim curl wget htop btop man-db man-pages "
    "openssh sudo "
    "grub efibootmgr "
    "hyprland hyprpaper hypridle hyprlock xdg-desktop-portal-hyprland "
    "waybar wofi dunst "
    "pipewire pipewire-pulse pipewire-alsa wireplumber "
    "pavucontrol alsa-utils "
    "firefox alacritty thunar thunar-archive-plugin file-roller "
    "mpv yt-dlp zathura zathura-pdf-poppler eog "
    "fastfetch rofi-wayland ripgrep fd "
    "ttf-jetbrains-mono-nerd "
    "polkit-gnome qt5-wayland qt6-wayland "
    "grim slurp wl-clipboard brightnessctl playerctl "
    "noto-fonts noto-fonts-emoji";

static int is_uefi_system(void) {
    struct stat st;
    return stat("/sys/firmware/efi", &st) == 0;
}

void logger_init(const char *log_path) {
    log_file = fopen(log_path, "a");
    if (log_file) {
        time_t now = time(NULL);
        char *timestamp = ctime(&now);
        timestamp[strlen(timestamp) - 1] = '\0';
        fprintf(log_file, "\n=== Luksarchy Installation Log - %s ===\n", timestamp);
        fflush(log_file);
    }
}

void logger_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

void log_msg(Log_Level level, const char *fmt, ...) {
    if (!log_file) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    fprintf(
        log_file,
        "[%02d:%02d:%02d] [%s] ",
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        level_strings[level]
    );

    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);
}

int write_file(const char *path, const char *content) {
    LOG_INFO("Writing file: %s", path);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("Failed to open file for writing: %s", path);
        return 0;
    }

    if (fprintf(fp, "%s", content) < 0) {
        LOG_ERROR("Failed to write to file: %s", path);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    LOG_DEBUG("Successfully wrote file: %s", path);
    return 1;
}

int write_file_fmt(const char *path, const char *fmt, ...) {
    char content[MAX_CMD_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(content, sizeof(content), fmt, args);
    va_end(args);

    return write_file(path, content);
}

int set_file_perms(const char *path, mode_t mode, const char *owner, const char *group) {
    LOG_INFO("Setting permissions for %s: mode=%o owner=%s group=%s", path, mode, owner, group);

    if (chmod(path, mode) != 0) {
        LOG_ERROR("Failed to chmod %s", path);
        return 0;
    }

    char chown_cmd[512];
    if (strncmp(path, CHROOT_PATH, strlen(CHROOT_PATH)) == 0) {
        const char *chroot_path = path + strlen(CHROOT_PATH);
        snprintf(chown_cmd, sizeof(chown_cmd),
            "arch-chroot %s chown %s:%s %s 2>> %s",
            CHROOT_PATH, owner, group, chroot_path, LOG_PATH);
    } else {
        snprintf(chown_cmd, sizeof(chown_cmd), "chown %s:%s %s", owner, group, path);
    }

    if (system(chown_cmd) != 0) {
        LOG_ERROR("Failed to chown %s", path);
        return 0;
    }

    return 1;
}

int create_directory(const char *path, mode_t mode) {
    LOG_INFO("Creating directory: %s", path);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);

    if (system(cmd) != 0) {
        LOG_ERROR("Failed to create directory: %s", path);
        return 0;
    }

    if (chmod(path, mode) != 0) {
        LOG_WARN("Failed to set permissions on directory: %s", path);
    }

    return 1;
}

int chroot_exec(const char *cmd) {
    char full_cmd[MAX_CMD_SIZE];
    snprintf(full_cmd, sizeof(full_cmd),
        "arch-chroot %s /bin/bash -c '%s' >> %s 2>&1",
        CHROOT_PATH, cmd, LOG_PATH);

    LOG_INFO("Executing in chroot: %s", cmd);
    LOG_DEBUG("Full command: %s", full_cmd);

    int result = system(full_cmd);
    if (result != 0) {
        LOG_ERROR("Chroot command failed (exit %d): %s", result, cmd);
        return 0;
    }

    LOG_DEBUG("Chroot command succeeded: %s", cmd);
    return 1;
}

int chroot_exec_fmt(const char *fmt, ...) {
    char cmd[MAX_CMD_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, args);
    va_end(args);

    return chroot_exec(cmd);
}

int chroot_exec_as_user(const char *username, const char *cmd) {
    char full_cmd[MAX_CMD_SIZE * 2];
    snprintf(full_cmd, sizeof(full_cmd), "sudo -u %s %s", username, cmd);
    return chroot_exec(full_cmd);
}

int chroot_exec_as_user_fmt(const char *username, const char *fmt, ...) {
    char cmd[MAX_CMD_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, args);
    va_end(args);

    return chroot_exec_as_user(username, cmd);
}

int git_clone_as_user(const char *username, const char *repo_url, const char *dest_path) {
    LOG_INFO("Cloning %s to %s as user %s", repo_url, dest_path, username);
    return chroot_exec_as_user_fmt(username, "git clone %s %s", repo_url, dest_path);
}

int make_clean_install(const char *build_dir) {
    LOG_INFO("Building and installing from %s", build_dir);
    return chroot_exec_fmt("cd %s && make clean install", build_dir);
}

int create_user_dotfile(const char *username, const Dotfile *dotfile) {
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/home/%s/%s", CHROOT_PATH, username, dotfile->filename);

    LOG_INFO("Creating dotfile %s for user %s", dotfile->filename, username);

    if (!write_file(full_path, dotfile->content)) {
        return 0;
    }

    if (!set_file_perms(full_path, dotfile->permissions, username, username)) {
        return 0;
    }

    return 1;
}

int setup_systemd_override(const Systemd_Override *override) {
    char dir_path[1024];
    char file_path[2048];

    snprintf(dir_path, sizeof(dir_path), "%s/etc/systemd/system/%s", CHROOT_PATH, override->drop_in_dir);
    snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, override->drop_in_file);

    LOG_INFO("Setting up systemd override: %s/%s", override->drop_in_dir, override->drop_in_file);

    if (!create_directory(dir_path, 0755)) {
        return 0;
    }

    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        LOG_ERROR("Failed to create systemd override file: %s", file_path);
        return 0;
    }

    for (size_t i = 0; i < override->entry_count; i++) {
        if (override->entries[i].value[0] == '\0') {
            fprintf(fp, "%s\n", override->entries[i].key);
        } else {
            fprintf(fp, "%s=%s\n", override->entries[i].key, override->entries[i].value);
        }
    }

    fclose(fp);
    LOG_INFO("Successfully created systemd override");
    return 1;
}

static int raw_mode_active = 0;

static void disable_raw_mode(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = 0;
    }
}

static void enable_raw_mode(void) {
    if (raw_mode_active) return;   /* already enabled; don't re-register atexit */

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);     /* registered exactly once */

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;
}

static void get_terminal_size(int *rows, int *cols) {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    *rows = ws.ws_row;
    *cols = ws.ws_col;
}

static enum Terminal_Size get_terminal_size_category(int cols) {
    if (cols < 45) return TERM_SMALL;
    if (cols < 75) return TERM_MEDIUM;
    return TERM_LARGE;
}

static int get_logo_width(int cols) {
    enum Terminal_Size size = get_terminal_size_category(cols);
    /*
     * Block-drawing chars (█ etc.) are double-width in most terminals
     * (wcwidth == 2).  Measure the widest line in display columns, not
     * byte/codepoint count.
     *
     * Large logo: widest line is 69 Unicode chars, all double-width → 138 cols.
     * Medium logo: ASCII-range box chars, single-width → 31 cols.
     * Small logo: "LUKSARCHY" → 9 cols.
     */
    if (size == TERM_LARGE)  return 138;
    if (size == TERM_MEDIUM) return 31;
    return 9;
}

static int get_logo_start(int cols) {
    int width = get_logo_width(cols);
    int start = (cols - width) / 2;
    return (start < 1) ? 1 : start;
}

static int get_menu_start_row(int cols) {
    enum Terminal_Size size = get_terminal_size_category(cols);
    if (size == TERM_LARGE) return 10;
    if (size == TERM_MEDIUM) return 7;
    return 4;
}

static void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

static void draw_logo(int cols) {
    const char *logo_large[] = {
        "███     ███   ████  ██████████ ██████ ███████  ████████  ████   ███",
        "███     ███   ████ ██▀████▀▀▀▀▀████▀▀██▄██▀▀██▄██▀▀▀▀██  ███▄▄███ ███▀",
        "███     ███   ██████▀ ██████▄ █████████████▄█████████▀ ▐██████▀ ▀███▄ ",
        "███     ███   ████▀██▄ ▀▀▀▀██ ███▀▀██ ███▀▀██ ███     ██▀▀██▄  ▀██▄  ",
        "███████▀███████▀███  ██ ██████ ██  ██  ██  ██ ▀███████  ███   ███   ",
        "▀▀▀▀▀▀ ▀▀▀▀▀▀ ▀▀▀▀▀▀ ▀▀ ▀▀▀▀▀▀ ▀▀  ▀▀  ▀▀  ▀▀ ▀▀▀▀▀▀ ▀▀▀   ▀▀▀   "
    };

    const char *logo_medium[] = {
        "╢  ╢ ╢╢╔═╗╔═╗╢╔═╗╔╢╔═╗╢ ╢╢ ╢",
        "║  ║ ║║╠╣╔╗║╠╣║╣║╢╝║  ╠╣╚╢╣╝",
        "╚═╝╚═╝╚╚═╝╚ ╚╚═╚═╝╚ ╚ ╚ ╚ "
    };

    const char *logo_small[] = {
        "LUKSARCHY"
    };

    enum Terminal_Size size = get_terminal_size_category(cols);
    const char **logo;
    int logo_height;

    switch (size) {
        case TERM_LARGE:
            logo = logo_large;
            logo_height = 6;
            break;
        case TERM_MEDIUM:
            logo = logo_medium;
            logo_height = 3;
            break;
        default:
            logo = logo_small;
            logo_height = 1;
            break;
    }

    /*
     * logo_width here is in *display columns* (double-width chars already
     * accounted for in get_logo_width).  If the terminal is narrower than
     * the logo just start at column 1 — the logo will wrap but at least it
     * won't appear off-centre to the right.
     */
    int display_width = get_logo_width(cols);
    int logo_start = (cols > display_width) ? (cols - display_width) / 2 : 1;
    if (logo_start < 1) logo_start = 1;

    printf("\033[1;32m");
    for (int i = 0; i < logo_height; i++) {
        printf("\033[%d;%dH%s", i + 2, logo_start, logo[i]);
    }
    printf("\033[0m");
}

static int draw_menu(const char **items, int count, int selected) {
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    int menu_start_row = get_menu_start_row(cols);

    for (int i = 0; i < count; i++) {
        printf("\033[%d;%dH", menu_start_row + i, logo_start + 2);
        if (i == selected) {
            printf("\033[1;34m> %s\033[0m", items[i]);
        } else {
            printf("\033[37m  %s\033[0m", items[i]);
        }
    }

    printf("\033[%d;%dH", menu_start_row + count + 2, logo_start);
    printf("\033[33mj/k Navigate  Enter Select\033[0m");

    fflush(stdout);
    return 0;
}

static int select_from_menu(const char **items, int count) {
    int selected = 0;

    enable_raw_mode();
    draw_menu(items, count, selected);

    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 27) {
            /* Read the rest of a possible escape sequence ("\033[A" / "\033[B"). */
            char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                read(STDIN_FILENO, &seq[1], 1);
                if (seq[1] == 'B') goto move_down;   /* arrow down */
                if (seq[1] == 'A') goto move_up;     /* arrow up   */
            }
            /* Bare ESC = quit */
            disable_raw_mode();
            return -1;
        }

        if (c == 'q') {
            disable_raw_mode();
            return -1;
        }

        if (c == 'j') {
move_down:
            if (selected < count - 1) {
                selected++;
                draw_menu(items, count, selected);
            }
            continue;
        }

        if (c == 'k') {
move_up:
            if (selected > 0) {
                selected--;
                draw_menu(items, count, selected);
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            disable_raw_mode();
            return selected;
        }
    }

    disable_raw_mode();
    return -1;
}

void show_message(const char *message) {
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    int msg_row = get_menu_start_row(cols);

    printf("\033[%d;%dH", msg_row, logo_start);
    printf("\033[37m%s\033[0m", message);
    fflush(stdout);

    sleep(2);
}

/*
 * check_internet_connection() / WiFi flow intentionally absent for offline
 * operation. All packages come from /repo (file:///repo).
 */

static void draw_form(
        const char *username,
        const char *password,
        const char *confirmed_password,
        const char *hostname,
        const char *keyboard,
        const char *timezone,
        int current_field
    ) {
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    int form_row = 10;

    printf(ANSI_CURSOR_POS ANSI_WHITE "Setup your system:" ANSI_RESET, form_row, logo_start);
    form_row += 2;

    Tui_Field fields[] = {
        {"Username",         username,           NULL,         0},
        {"Password",         password,           NULL,         1},
        {"Confirm Password", confirmed_password, NULL,         1},
        {"Hostname",         hostname,           "luksarchy",  0},
        {"Keyboard",         keyboard,           "us",         0},
        {"Timezone",         timezone,           NULL,         0},
    };
    int num_fields = (int)(sizeof(fields) / sizeof(fields[0]));

    for (int i = 0; i < num_fields; i++) {
        printf(ANSI_CURSOR_POS, form_row + i, logo_start);

        if (current_field == i) {
            printf(ANSI_BLUE_BOLD ">" ANSI_RESET " ");
        } else {
            printf("  ");
        }

        printf(ANSI_WHITE "%s: " ANSI_RESET, fields[i].label);

        if (strlen(fields[i].value) > 0) {
            printf(ANSI_GREEN "%s" ANSI_RESET, fields[i].is_password ? "********" : fields[i].value);
        } else if (current_field != i) {
            if (fields[i].default_display) {
                printf(ANSI_GRAY "%s" ANSI_RESET, fields[i].default_display);
            } else {
                printf(ANSI_GRAY "[not set]" ANSI_RESET);
            }
        }
    }

    fflush(stdout);
}

static int validate_alphanumeric(const char *s) {
    for (int i = 0; s[i]; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_')
            return 0;
    }
    return 1;
}

static int read_line(char *buf, int size, int echo) {
    struct termios old_term;
    tcgetattr(STDIN_FILENO, &old_term);
    struct termios new_term = old_term;
    if (echo)
        new_term.c_lflag |= ECHO;
    else
        new_term.c_lflag &= ~ECHO;
    new_term.c_lflag |= ICANON;
    new_term.c_lflag &= ~ISIG;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);

    int result = (fgets(buf, size, stdin) != NULL);
    if (result)
        buf[strcspn(buf, "\n")] = '\0';

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    return result;
}

/* dest must be at least 256 bytes (matches the callers' 256-byte arrays). */
static int fzf_select(char *dest, size_t dest_size, const char *cmd, const char *default_val) {
    clear_screen();
    FILE *fp = popen(cmd, "r");
    if (fp == NULL)
        return 0;

    char buf[256];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        buf[strcspn(buf, "\n")] = '\0';
        if (buf[0] != '\0')
            snprintf(dest, dest_size, "%s", buf);
    }
    pclose(fp);

    if (dest[0] == '\0' && default_val)
        snprintf(dest, dest_size, "%s", default_val);

    return 1;
}

static int handle_password_entry(
        char *password,
        char *confirmed_password,
        int form_row,
        int logo_start,
        char *username,
        char *hostname,
        char *keyboard,
        char *timezone
    ) {
    char temp_input[256];
    char password_confirm[256];

    printf(ANSI_CURSOR_POS, form_row + 1, logo_start + 13);
    fflush(stdout);

    if (!read_line(temp_input, sizeof(temp_input), 0))
        return -1;

    if (strlen(temp_input) == 0) {
        show_message("Password cannot be empty");
        return 0;
    }

    snprintf(password, 256, "%s", temp_input);

    draw_form(username, password, confirmed_password, hostname, keyboard, timezone, 2);
    printf(ANSI_CURSOR_POS, form_row + 2, logo_start + 20);
    fflush(stdout);

    if (!read_line(password_confirm, sizeof(password_confirm), 0))
        return -1;

    if (strcmp(password, password_confirm) == 0) {
        snprintf(confirmed_password, 256, "%s", password_confirm);
        return 1;
    } else {
        show_message("Passwords do not match");
        return 0;
    }
}

static int get_form_input(
        char *username,
        char *password,
        char *confirmed_password,
        char *hostname,
        char *keyboard,
        char *timezone
    ) {
    char temp_input[256];
    int rows, cols;
    get_terminal_size(&rows, &cols);
    int logo_start = get_logo_start(cols);
    int form_row = 12;

    Form_Field fields[] = {
        {username, NULL,        INPUT_TEXT,         13, "Username must be alphanumeric"},
        {password, NULL,        INPUT_PASSWORD,     13, NULL},
        {confirmed_password, NULL, INPUT_PASSWORD,  20, NULL},
        {hostname, "luksarchy", INPUT_TEXT,         13, "Hostname must be alphanumeric"},
        {keyboard, "us",        INPUT_FZF_KEYMAP,   0,  NULL},
        {timezone, NULL,        INPUT_FZF_TIMEZONE, 0,  "Timezone is required"},
    };
    int num_fields = (int)(sizeof(fields) / sizeof(fields[0]));

    int current_field = 0;
    while (current_field < num_fields) {
        draw_form(username, password, confirmed_password, hostname, keyboard, timezone, current_field);
        Form_Field *f = &fields[current_field];

        if (f->type == INPUT_FZF_KEYMAP) {
            fzf_select(keyboard, 256,
                "localectl list-keymaps | fzf --height=40% --reverse --prompt='Keyboard: ' "
                "--header='Start typing to filter, Enter to select' --query='us'", "us");
            current_field++;
        } else if (f->type == INPUT_FZF_TIMEZONE) {
            fzf_select(timezone, 256,
                "timedatectl list-timezones | fzf --height=40% --reverse --prompt='Timezone: ' "
                "--header='Type your city/timezone, Enter to select'", NULL);
            if (strlen(timezone) == 0) {
                show_message("Timezone is required");
            } else {
                current_field++;
            }
        } else if (current_field == 1) {
            int result = handle_password_entry(
                password,
                confirmed_password,
                form_row,
                logo_start,
                username,
                hostname,
                keyboard,
                timezone
            );
            if (result == -1) return 0;
            if (result == 1) current_field = 3;
        } else if (current_field == 2) {
            current_field = 1;
        } else {
            printf(ANSI_CURSOR_POS, form_row + current_field, logo_start + f->cursor_offset);
            fflush(stdout);

            if (!read_line(temp_input, sizeof(temp_input), 1))
                return 0;

            if (strlen(temp_input) == 0) {
                if (f->default_val) {
                    snprintf(f->dest, 256, "%s", f->default_val);
                    current_field++;
                }
            } else if (validate_alphanumeric(temp_input)) {
                snprintf(f->dest, 256, "%s", temp_input);
                current_field++;
            } else {
                show_message(f->error_msg);
            }
        }
    }

    while (1) {
        draw_form(username, password, confirmed_password, hostname, keyboard, timezone, 6);

        get_terminal_size(&rows, &cols);
        logo_start = get_logo_start(cols);

        printf(ANSI_CURSOR_POS ANSI_YELLOW
               "Press Enter to continue, or field number to edit (0-5)" ANSI_RESET,
               20, logo_start);
        fflush(stdout);

        enable_raw_mode();
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == '\r' || c == '\n') {
                disable_raw_mode();
                return 1;
            }
            if (c == 'q' || c == 27) {
                disable_raw_mode();
                return 0;
            }
            if (c >= '0' && c <= '5') {
                disable_raw_mode();
                int edit_field = c - '0';
                Form_Field *f = &fields[edit_field];

                if (f->type == INPUT_FZF_KEYMAP) {
                    fzf_select(keyboard, 256,
                        "localectl list-keymaps | fzf --height=40% --reverse --prompt='Keyboard: ' "
                        "--header='Start typing to filter, Enter to select' --query='us'", "us");
                } else if (f->type == INPUT_FZF_TIMEZONE) {
                    fzf_select(timezone, 256,
                        "timedatectl list-timezones | fzf --height=40% --reverse --prompt='Timezone: ' "
                        "--header='Type your city/timezone, Enter to select'", NULL);
                    if (strlen(timezone) == 0)
                        show_message("Timezone is required");
                } else if (edit_field == 1 || edit_field == 2) {
                    draw_form(username, password, confirmed_password, hostname, keyboard, timezone, 1);
                    handle_password_entry(password, confirmed_password,
                                          form_row, logo_start,
                                          username, hostname, keyboard, timezone);
                } else {
                    draw_form(username, password, confirmed_password, hostname, keyboard, timezone, edit_field);
                    printf(ANSI_CURSOR_POS, form_row + edit_field, logo_start + f->cursor_offset);
                    fflush(stdout);

                    if (read_line(temp_input, sizeof(temp_input), 1)) {
                        if (strlen(temp_input) == 0 && f->default_val) {
                            snprintf(f->dest, 256, "%s", f->default_val);
                        } else if (strlen(temp_input) > 0) {
                            if (validate_alphanumeric(temp_input))
                                snprintf(f->dest, 256, "%s", temp_input);
                            else
                                show_message(f->error_msg);
                        }
                    }
                }
                continue;
            }
        }
        disable_raw_mode();
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Disk selection                                                     */
/* ------------------------------------------------------------------ */

static int select_disk(char *disk_name) {
    clear_screen();

    FILE *fp = popen(
        "lsblk -d -n -o NAME,SIZE,MODEL,TYPE | grep -E '(disk|nvme)' | "
        "grep -v -E '(loop|rom|airoot)' | "
        "awk '{printf \"%s (%s) %s\\n\", $1, $2, substr($0, index($0,$3))}'",
        "r");
    if (fp == NULL) {
        show_message("Failed to list disks");
        return 0;
    }

    char disks[32][256];
    char names[32][64];
    int disk_count = 0;

    while (disk_count < 32 && fgets(disks[disk_count], sizeof(disks[0]), fp) != NULL) {
        disks[disk_count][strcspn(disks[disk_count], "\n")] = '\0';
        sscanf(disks[disk_count], "%s", names[disk_count]);
        disk_count++;
    }
    pclose(fp);

    if (disk_count == 0) {
        show_message("No disks found");
        return 0;
    }

    const char *disk_ptrs[32];
    for (int i = 0; i < disk_count; i++) {
        disk_ptrs[i] = disks[i];
    }

    int selected = select_from_menu(disk_ptrs, disk_count);
    if (selected < 0) {
        return 0;
    }

    snprintf(disk_name, 64, "%s", names[selected]);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Install mode + free-space helpers                                  */
/* ------------------------------------------------------------------ */

static int select_install_mode(void) {
    const char *items[] = {
        "Erase disk and install Luksarchy (destroys all data)",
        "Install alongside existing OS (use free space)"
    };
    int sel = select_from_menu(items, 2);
    if (sel < 0) return -1;
    return sel; /* 0 = WHOLE_DISK, 1 = FREE_SPACE */
}

/*
 * Return free regions of /dev/<disk> as "start:end" strings.
 * Output format from parted -ms:  "<num>:<start>:<end>:<size>:free;<fs>;"
 * We print "<start>:<end>" (units as reported by parted, typically MiB/GiB or %).
 */
/*
 * list_free_space() - find free regions on /dev/<disk>.
 *
 * Works entirely in 512-byte sectors to avoid any unit-suffix ambiguity.
 * Parses sgdisk -p output for partition table + the "First usable sector"
 * and "Last usable sector" lines, so gaps never overlap the GPT header,
 * partition entries, or backup GPT at the disk's end.
 *
 * Returns region strings as "startSector:endSector" (raw 512-byte sectors).
 */
static int list_free_space(const char *disk, char regions[][128], int max_regions) {
    char cmd[512];
    int count = 0;
    unsigned long long first_usable = 34;   /* conservative GPT default */
    unsigned long long last_usable  = 0;

    /* First pass: parse "First usable sector" and "Last usable sector" from sgdisk -p. */
    snprintf(cmd, sizeof(cmd), "sgdisk -p /dev/%s 2>/dev/null", disk);
    {
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            LOG_ERROR("list_free_space: sgdisk -p failed for /dev/%s", disk);
            return 0;
        }
        char line[256];
        while (fgets(line, sizeof(line), fp) != NULL) {
            unsigned long long val;
            if (sscanf(line, " First usable sector: %llu", &val) == 1)
                first_usable = val;
            else if (sscanf(line, " Last usable sector: %llu", &val) == 1)
                last_usable = val;
        }
        pclose(fp);
    }

    /* Fallback: if sgdisk didn't report usable range, use blockdev size minus backup GPT. */
    if (last_usable == 0) {
        snprintf(cmd, sizeof(cmd), "blockdev --getsz /dev/%s 2>/dev/null", disk);
        FILE *fp2 = popen(cmd, "r");
        if (fp2) {
            char buf[64] = "";
            if (fgets(buf, sizeof(buf), fp2) != NULL) {
                unsigned long long total = strtoull(buf, NULL, 10);
                /* Reserve 33 sectors for backup GPT + 1 for alignment. */
                last_usable = (total > 34) ? total - 34 : 0;
            }
            pclose(fp2);
        }
    }

    char line[256];
    unsigned long long prev_end = first_usable - 1;

    /* Second pass: list partitions and compute gaps. */
    snprintf(cmd, sizeof(cmd),
        "sgdisk -p /dev/%s 2>/dev/null | awk 'NF>=3 && $1 ~ /^[0-9]+$/ { print $2, $3 }'", disk);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERROR("list_free_space: sgdisk -p failed for /dev/%s", disk);
        return 0;
    }

    while (count < max_regions && fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long start = 0, end = 0;
        if (sscanf(line, "%llu %llu", &start, &end) == 2) {
            /* Free gap between prev_end+1 and start-1. */
            if (start > prev_end + 1) {
                unsigned long long gap_start = prev_end + 1;
                unsigned long long gap_end   = start - 1;
                if (gap_start < first_usable) gap_start = first_usable;
                if (gap_end > last_usable)    gap_end   = last_usable;
                if (gap_end > gap_start && (gap_end - gap_start) >= 4096) {
                    snprintf(regions[count], sizeof(regions[count]), "%llu:%llu",
                             gap_start, gap_end);
                    count++;
                }
            }
            if (end > prev_end) prev_end = end;
        }
    }
    pclose(fp);

    /* Free space from after the last partition to the last usable sector. */
    {
        if (count < max_regions && last_usable > prev_end + 1) {
            unsigned long long gap_start = prev_end + 1;
            unsigned long long gap_end   = last_usable;
            if (gap_start < first_usable) gap_start = first_usable;
            if (gap_end > gap_start && (gap_end - gap_start) >= 4096) { /* >= 2 MiB */
                snprintf(regions[count], sizeof(regions[count]), "%llu:%llu",
                         gap_start, gap_end);
                count++;
            }
        }
    }

    LOG_INFO("list_free_space: found %d free region(s) on /dev/%s (usable %llu..%llu)",
             count, disk, first_usable, last_usable);
    return count;
}



/*
 * Detect an existing EFI System Partition on the given disk.
 * Used in free-space/alongside mode to reuse the ESP instead of formatting
 * a new one.
 */
static int detect_esp(const char *disk, char *esp_out, size_t esp_out_size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "lsblk -n -o NAME,PARTTYPENAME /dev/%s 2>/dev/null | "
        "awk '$2 ~ /EFI/ {print \"/dev/\" $1; exit}'", disk);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char line[128] = "";
    if (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';
    }
    pclose(fp);

    if (strlen(line) == 0) return 0;
    snprintf(esp_out, esp_out_size, "%s", line);
    LOG_INFO("Detected existing ESP: %s", esp_out);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Partitioning                                                       */
/* ------------------------------------------------------------------ */

/*
 * Non-destructive alongside-install:
 *   - Do NOT wipefs, do NOT zap-all
 *   - Create one new partition for / inside the chosen free region
 *   - Reuse existing ESP if UEFI and detect_esp() found one
 *   - Put swap in a swapfile on the root (avoids needing a 2nd region)
 */
/*
 * partition_disk_free_space() - non-destructive alongside-install.
 *
 *   - Do NOT wipefs, do NOT zap-all
 *   - Create one new root partition inside the chosen free region
 *   - Reuse existing ESP if UEFI and detect_esp() found one
 *   - Put swap in a swapfile on the root (avoids needing a 2nd region)
 *
 * plan->free_start / free_end are raw 512-byte sector numbers (e.g. "2048:41943006").
 */
static int partition_disk_free_space(Partition_Plan *plan) {
    char cmd[2048];
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);
    int logo_start = get_logo_start(cols);

    int uefi = is_uefi_system();
    printf("\033[%d;%dH\033[37mCreating root partition in free space on /dev/%s...\033[0m",
           10, logo_start, plan->disk);
    fflush(stdout);

    unsigned long long free_s = strtoull(plan->free_start, NULL, 10);
    unsigned long long free_e = strtoull(plan->free_end,   NULL, 10);

    /* Sanity: refuse if free region is < 15 GiB.
     * 15 GiB = 15 * 1024 * 2048 = 31457280 sectors (512-byte). */
    if (free_e - free_s < 31457280ULL) {
        show_message("Free space < 15 GiB, refusing alongside install");
        return 0;
    }

    /*
     * Bootloader partition handling:
     *
     * UEFI + no existing ESP: carve a 512 MiB ESP (ef00) from the start of
     *   the free region.  root starts immediately after.
     *
     * BIOS + GPT: GRUB's i386-pc target requires a BIOS Boot Partition (ef02,
     *   1 MiB) to embed its core image — without one it refuses to install.
     *   Carve it from the start of the free region before root.
     *
     * UEFI + existing ESP / BIOS + MBR: nothing extra needed here.
     */
    unsigned long long root_s = free_s;   /* start sector for root partition */

    if (!uefi) {
        /*
         * BIOS systems: check if the disk uses GPT.  If so we need a BIOS
         * Boot Partition.  MBR disks embed GRUB in the MBR gap — no extra
         * partition required.
         */
        char gpt_check[512];
        snprintf(gpt_check, sizeof(gpt_check),
            "sgdisk -p /dev/%s 2>/dev/null | grep -q 'GPT'", plan->disk);
        int is_gpt = (system(gpt_check) == 0);

        if (is_gpt) {
            /* 1 MiB BIOS Boot Partition: 2048 sectors of 512 bytes */
            unsigned long long bbp_end = free_s + 2047ULL;
            if (bbp_end >= free_e) {
                show_message("Free region too small to fit BIOS Boot Partition + root");
                return 0;
            }
            LOG_INFO("GPT + BIOS: creating 1 MiB BIOS Boot Partition (ef02)");
            snprintf(cmd, sizeof(cmd),
                "sgdisk --new=0:%llu:%llu --typecode=0:ef02 --change-name=0:BIOS /dev/%s 2>> %s",
                free_s, bbp_end, plan->disk, LOG_PATH);
            if (system(cmd) != 0) {
                LOG_ERROR("sgdisk --new BIOS Boot Partition failed on /dev/%s", plan->disk);
                show_message("Failed to create BIOS Boot Partition");
                return 0;
            }
            system("partprobe 2>/dev/null || true");
            sleep(1);
            root_s = bbp_end + 1;
            LOG_INFO("BIOS Boot Partition created; root will start at sector %llu", root_s);
        }
    }

    if (uefi && !plan->reuse_efi) {
        /* 512 MiB ESP: sectors [free_s, free_s + 1048575] */
        unsigned long long esp_end = free_s + 1048575ULL;
        if (esp_end >= free_e) {
            show_message("Free region too small to fit ESP + root");
            return 0;
        }

        LOG_INFO("No existing ESP — creating new 512 MiB ESP in free region");
        snprintf(cmd, sizeof(cmd),
            "sgdisk --new=0:%llu:%llu --typecode=0:ef00 --change-name=0:EFI /dev/%s 2>> %s",
            free_s, esp_end, plan->disk, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("sgdisk --new ESP failed on /dev/%s", plan->disk);
            show_message("Failed to create EFI partition");
            return 0;
        }

        /* Re-read so the kernel sees the new ESP before we query its number. */
        system("partprobe 2>/dev/null || true");
        sleep(1);

        /* Find the partition number we just created (highest number). */
        {
            char q[512];
            snprintf(q, sizeof(q),
                "sgdisk -p /dev/%s 2>/dev/null"
                " | awk 'NF>=3 && $1 ~ /^[0-9]+$/ { print $1 }'"
                " | sort -n | tail -1",
                plan->disk);
            FILE *fp = popen(q, "r");
            if (!fp) { show_message("Failed to query ESP partition number"); return 0; }
            char nbuf[16] = "";
            if (fgets(nbuf, sizeof(nbuf), fp) != NULL)
                nbuf[strcspn(nbuf, "\n")] = '\0';
            pclose(fp);
            if (strlen(nbuf) == 0) {
                show_message("Could not determine ESP partition number");
                return 0;
            }
            part_path(plan->efi_part, sizeof(plan->efi_part), plan->disk, atoi(nbuf));
        }
        LOG_INFO("New ESP partition: %s", plan->efi_part);

        /* Format the new ESP. */
        snprintf(cmd, sizeof(cmd), "mkfs.fat -F32 %s 2>> %s", plan->efi_part, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("mkfs.fat failed on %s", plan->efi_part);
            show_message("Failed to format EFI partition");
            return 0;
        }

        plan->reuse_efi = 0;   /* fresh ESP — install_bootloader will use bootctl/grub fresh */
        root_s = esp_end + 1;  /* root starts right after the ESP */
    }

    /* Create root partition from root_s to free_e. */
    snprintf(cmd, sizeof(cmd),
        "sgdisk --new=0:%llu:%llu --typecode=0:8300 /dev/%s 2>> %s",
        root_s, free_e, plan->disk, LOG_PATH);
    if (system(cmd) != 0) {
        LOG_ERROR("sgdisk --new root failed on /dev/%s (start=%llu end=%llu)",
                  plan->disk, root_s, free_e);
        show_message("Failed to create root partition");
        return 0;
    }

    /* Re-read partition table. */
    system("partprobe 2>/dev/null || true");
    sleep(1);

    /* Figure out which partition number we just created — the highest one. */
    {
        char q[512];
        snprintf(q, sizeof(q),
            "sgdisk -p /dev/%s 2>/dev/null"
            " | awk 'NF>=3 && $1 ~ /^[0-9]+$/ { print $1 }'"
            " | sort -n | tail -1",
            plan->disk);
        FILE *fp = popen(q, "r");
        if (!fp) { show_message("Failed to query root partition number"); return 0; }
        char numbuf[16] = "";
        if (fgets(numbuf, sizeof(numbuf), fp) != NULL)
            numbuf[strcspn(numbuf, "\n")] = '\0';
        pclose(fp);
        if (strlen(numbuf) == 0) {
            show_message("Could not determine root partition number");
            return 0;
        }
        part_path(plan->root_part, sizeof(plan->root_part), plan->disk, atoi(numbuf));
    }
    LOG_INFO("New root partition: %s", plan->root_part);

    /* mkfs.ext4 on the new root partition. */
    snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F %s 2>> %s", plan->root_part, LOG_PATH);
    if (system(cmd) != 0) {
        LOG_ERROR("mkfs.ext4 failed on %s", plan->root_part);
        show_message("Failed to format root partition");
        return 0;
    }

    /* Mount root at /mnt. */
    snprintf(cmd, sizeof(cmd), "mount %s /mnt 2>> %s", plan->root_part, LOG_PATH);
    if (system(cmd) != 0) {
        LOG_ERROR("mount %s /mnt failed", plan->root_part);
        show_message("Failed to mount root partition");
        return 0;
    }

    /* Mount ESP at /mnt/boot (reused or freshly created). */
    if (uefi) {
        system("mkdir -p /mnt/boot 2>> " LOG_PATH);
        snprintf(cmd, sizeof(cmd), "mount %s /mnt/boot 2>> %s", plan->efi_part, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to mount ESP %s at /mnt/boot", plan->efi_part);
            show_message("Failed to mount EFI partition");
            return 0;
        }
        LOG_INFO("ESP %s mounted at /mnt/boot", plan->efi_part);
    }

    show_message("Disk prepared successfully (alongside)!");
    return 1;
}

/*
 * Destructive whole-disk install (existing behavior).
 */
static int partition_disk_whole(Partition_Plan *plan) {
    char cmd[1024];
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);
    int logo_start = get_logo_start(cols);
    int uefi = is_uefi_system();

    char part1[64], part2[64], part3[64];
    part_path(part1, sizeof(part1), plan->disk, 1);
    part_path(part2, sizeof(part2), plan->disk, 2);
    part_path(part3, sizeof(part3), plan->disk, 3);

    printf("\033[%d;%dH\033[37mPartitioning /dev/%s (%s mode)...\033[0m",
           10, logo_start, plan->disk, uefi ? "UEFI" : "BIOS");
    fflush(stdout);

    LOG_INFO("Starting whole-disk partitioning: /dev/%s (mode: %s)",
             plan->disk, uefi ? "UEFI" : "BIOS");

    snprintf(cmd, sizeof(cmd), "wipefs -af /dev/%s 2>> %s", plan->disk, LOG_PATH);
    if (system(cmd) != 0) {
        LOG_ERROR("Failed to wipe disk: /dev/%s", plan->disk);
        show_message("Failed to wipe disk");
        return 0;
    }

    if (uefi) {
        snprintf(cmd, sizeof(cmd), "sgdisk --zap-all /dev/%s 2>> %s", plan->disk, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to zap disk: /dev/%s", plan->disk);
            show_message("Failed to zap disk");
            return 0;
        }

        snprintf(cmd, sizeof(cmd),
            "sgdisk --clear "
            "--new=1:0:+1G --typecode=1:ef00 --change-name=1:EFI "
            "--new=2:0:+4G --typecode=2:8200 --change-name=2:swap "
            "--new=3:0:0 --typecode=3:8300 --change-name=3:root "
            "/dev/%s 2>> %s", plan->disk, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to create partitions on /dev/%s", plan->disk);
            show_message("Failed to create partitions");
            return 0;
        }
    } else {
        snprintf(cmd, sizeof(cmd),
            "parted -s /dev/%s mklabel msdos "
            "mkpart primary linux-swap 1MiB 4GiB "
            "mkpart primary ext4 4GiB 100%% "
            "set 2 boot on 2>> %s", plan->disk, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to create MBR partitions on /dev/%s", plan->disk);
            show_message("Failed to create partitions");
            return 0;
        }
    }

    if (uefi) {
        snprintf(cmd, sizeof(cmd), "mkfs.fat -F32 %s 2>> %s", part1, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to format EFI partition: %s", part1);
            show_message("Failed to format EFI partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mkswap %s 2>> %s", part2, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to format swap: %s", part2);
            show_message("Failed to format swap partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F %s 2>> %s", part3, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to format root: %s", part3);
            show_message("Failed to format root partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mount %s /mnt 2>> %s", part3, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to mount root: %s", part3);
            show_message("Failed to mount root partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mkdir -p /mnt/boot 2>> %s", LOG_PATH);
        system(cmd);

        snprintf(cmd, sizeof(cmd), "mount %s /mnt/boot 2>> %s", part1, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to mount EFI: %s", part1);
            show_message("Failed to mount EFI partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "swapon %s 2>> %s", part2, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to enable swap: %s", part2);
            show_message("Failed to enable swap");
            return 0;
        }

        snprintf(plan->root_part, sizeof(plan->root_part), "%s", part3);
        snprintf(plan->efi_part,  sizeof(plan->efi_part),  "%s", part1);
        plan->reuse_efi = 0;
    } else {
        snprintf(cmd, sizeof(cmd), "mkswap %s 2>> %s", part1, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to format swap: %s", part1);
            show_message("Failed to format swap partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mkfs.ext4 -F %s 2>> %s", part2, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to format root: %s", part2);
            show_message("Failed to format root partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "mount %s /mnt 2>> %s", part2, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to mount root: %s", part2);
            show_message("Failed to mount root partition");
            return 0;
        }

        snprintf(cmd, sizeof(cmd), "swapon %s 2>> %s", part1, LOG_PATH);
        if (system(cmd) != 0) {
            LOG_ERROR("Failed to enable swap: %s", part1);
            show_message("Failed to enable swap");
            return 0;
        }

        snprintf(plan->root_part, sizeof(plan->root_part), "%s", part2);
        plan->efi_part[0] = '\0';
    }

    show_message("Disk prepared successfully!");
    return 1;
}

static int partition_disk_plan(Partition_Plan *plan) {
    if (plan->mode == INSTALL_FREE_SPACE) {
        return partition_disk_free_space(plan);
    }
    return partition_disk_whole(plan);
}

/* ------------------------------------------------------------------ */
/* Package installation                                               */
/* ------------------------------------------------------------------ */

static int install_packages_impl(const char *package_list) {
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    printf("\033[%d;%dH\033[37mInstalling system packages (offline)...\033[0m", 10, logo_start);
    printf("\033[%d;%dH\033[37mThis will take several minutes.\033[0m", 11, logo_start);
    fflush(stdout);

    LOG_INFO("Starting offline package installation");
    LOG_INFO("Packages: %s", package_list);

    /* Pre-flight checks so we fail fast with a clear message. */
    if (stat("/usr/bin/pacstrap", &(struct stat){0}) != 0) {
        LOG_ERROR("pacstrap not found at /usr/bin/pacstrap");
        show_message("pacstrap not found in live environment");
        return 0;
    }
    if (stat("/repo", &(struct stat){0}) != 0) {
        LOG_ERROR("/repo not found - local package repository missing");
        show_message("Package repository /repo not found");
        return 0;
    }
    if (stat("/mnt", &(struct stat){0}) != 0) {
        LOG_ERROR("/mnt not found - root partition not mounted");
        show_message("Root partition not mounted at /mnt");
        return 0;
    }

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "pacstrap -C /etc/pacman.conf -G /mnt %s 2>> %s",
        package_list, LOG_PATH);

    int result = system(cmd);
    if (result != 0) {
        LOG_ERROR("pacstrap failed with exit code %d — see %s for details", result, LOG_PATH);
        show_message("Failed to install packages (see log)");
        return 0;
    }

    LOG_INFO("Package installation completed successfully");
    show_message("Packages installed successfully!");
    return 1;
}

/*
 * Opportunistically install ly:
 *   1. Try the offline repo first (if the package is present there).
 *   2. Fall back to a vendored .pkg.tar.zst under /usr/share/luksarchy/ly/.
 *   3. If neither works, log a warning and return 0 — the install continues,
 *      and autologin on tty1 will be used instead.
 */
static int install_ly(void) {
    int rows, cols;
    get_terminal_size(&rows, &cols);
    clear_screen();
    draw_logo(cols);
    int logo_start = get_logo_start(cols);

    printf("\033[%d;%dH\033[37mInstalling ly display manager...\033[0m", 10, logo_start);
    fflush(stdout);

    /* 1. Offline repo */
    if (chroot_exec("pacman -C /etc/pacman.conf -G -S --noconfirm ly 2>> " LOG_PATH)) {
        LOG_INFO("ly installed from offline repo");
        return 1;
    }

    /* 2. Vendored package */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "ls /usr/share/luksarchy/ly/ly-*.pkg.tar.zst 2>/dev/null | head -1");
    FILE *fp = popen(cmd, "r");
    char pkg[512] = "";
    if (fp) {
        if (fgets(pkg, sizeof(pkg), fp)) {
            pkg[strcspn(pkg, "\n")] = '\0';
        }
        pclose(fp);
    }

    if (strlen(pkg) > 0) {
        char base[256];
        const char *slash = strrchr(pkg, '/');
        snprintf(base, sizeof(base), "%s", slash ? slash + 1 : pkg);

        /* Copy into chroot and install there */
        char cp_cmd[1024];
        snprintf(cp_cmd, sizeof(cp_cmd),
            "mkdir -p /mnt/tmp/ly && cp %s /mnt/tmp/ly/ 2>> %s", pkg, LOG_PATH);
        if (system(cp_cmd) == 0) {
            if (chroot_exec_fmt(
                    "pacman -U --noconfirm /tmp/ly/%s 2>> %s", base, LOG_PATH)) {
                LOG_INFO("ly installed from vendored package %s", base);
                return 1;
            }
        }
    }

    LOG_WARN("ly could not be installed (not in /repo and no vendored package). "
             "Falling back to getty@tty1 autologin.");
    return 0;
}

/* ------------------------------------------------------------------ */
/* System configuration                                               */
/* ------------------------------------------------------------------ */

static int configure_system_impl(
        const char *username,
        const char *password,
        const char *hostname,
        const char *keyboard,
        const char *timezone,
        const char *disk
    ) {
    (void)disk;
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    printf("\033[%d;%dH\033[37mConfiguring system...\033[0m", 10, logo_start);
    printf("\033[%d;%dH\033[90m(Logging to %s)\033[0m", 11, logo_start, LOG_PATH);
    fflush(stdout);

    LOG_INFO("Starting system configuration");

    CHECK_OR_FAIL(
        system("genfstab -U /mnt >> /mnt/etc/fstab 2>> " LOG_PATH) == 0,
        "Failed to generate fstab - check " LOG_PATH
    );

    CHECK_OR_FAIL(
        chroot_exec_fmt("ln -sf /usr/share/zoneinfo/%s /etc/localtime", timezone),
        "Failed to configure timezone"
    );

    if (!chroot_exec("hwclock --systohc")) {
        LOG_WARN("Failed to set hardware clock");
    }

    CHECK_OR_FAIL(
        write_file("/mnt/etc/locale.gen", "en_US.UTF-8 UTF-8\n"),
        "Failed to write locale.gen"
    );

    CHECK_OR_FAIL(
        chroot_exec("locale-gen"),
        "Failed to generate locales"
    );

    CHECK_OR_FAIL(
        write_file("/mnt/etc/locale.conf", "LANG=en_US.UTF-8\n"),
        "Failed to write locale.conf"
    );

    CHECK_OR_FAIL(
        write_file_fmt("/mnt/etc/vconsole.conf", "KEYMAP=%s\n", keyboard),
        "Failed to write vconsole.conf"
    );

    CHECK_OR_FAIL(
        write_file_fmt("/mnt/etc/hostname", "%s\n", hostname),
        "Failed to write hostname"
    );

    char hosts_content[512];
    snprintf(hosts_content, sizeof(hosts_content),
             "127.0.0.1   localhost\n"
             "::1         localhost\n"
             "127.0.1.1   %s.localdomain %s\n",
             hostname, hostname);

    CHECK_OR_FAIL(
        write_file("/mnt/etc/hosts", hosts_content),
        "Failed to write hosts file"
    );

    CHECK_OR_FAIL(
        chroot_exec_fmt("useradd -m -G wheel,video,audio,input,storage -s /bin/bash %s", username),
        "Failed to create user"
    );

    FILE *chpasswd_pipe = popen("arch-chroot /mnt chpasswd 2>> " LOG_PATH, "w");
    if (!chpasswd_pipe) {
        show_message("Failed to open chpasswd");
        return 0;
    }
    fprintf(chpasswd_pipe, "%s:%s\n", username, password);
    fprintf(chpasswd_pipe, "root:%s\n", password);
    int chpasswd_status = pclose(chpasswd_pipe);
    CHECK_OR_FAIL(
        chpasswd_status == 0,
        "Failed to set passwords"
    );

    create_directory("/mnt/etc/sudoers.d", 0750);
    CHECK_OR_FAIL(
        write_file("/mnt/etc/sudoers.d/wheel", "%wheel ALL=(ALL:ALL) ALL\n"),
        "Failed to configure sudo"
    );
    chmod("/mnt/etc/sudoers.d/wheel", 0440);

    CHECK_OR_FAIL(
        chroot_exec("systemctl enable NetworkManager"),
        "Failed to enable NetworkManager"
    );

    CHECK_OR_FAIL(
        chroot_exec("systemctl enable dbus"),
        "Failed to enable dbus"
    );

    /* PipeWire user units: best effort. */
    chroot_exec("systemctl --user enable pipewire pipewire-pulse wireplumber 2>/dev/null || true");

    LOG_INFO("System configuration completed successfully");
    show_message("System configured successfully!");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Bootloader                                                         */
/* ------------------------------------------------------------------ */

static int get_root_uuid(const char *root_part, char *uuid_out, size_t uuid_size) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "blkid -s UUID -o value %s", root_part);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_ERROR("Failed to get UUID for %s", root_part);
        return 0;
    }

    if (fgets(uuid_out, uuid_size, fp) == NULL) {
        pclose(fp);
        LOG_ERROR("Failed to read UUID for %s", root_part);
        return 0;
    }

    uuid_out[strcspn(uuid_out, "\n")] = '\0';
    pclose(fp);

    if (strlen(uuid_out) == 0) {
        LOG_ERROR("Empty UUID for %s", root_part);
        return 0;
    }

    LOG_INFO("Root partition UUID: %s", uuid_out);
    return 1;
}

static int install_bootloader(const Partition_Plan *plan) {
    char cmd[2048];
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    int uefi = is_uefi_system();

    printf("\033[%d;%dH\033[37mInstalling bootloader (%s)...\033[0m",
           10, logo_start, uefi ? "systemd-boot/GRUB" : "GRUB");
    fflush(stdout);

    char uuid[128];
    if (!get_root_uuid(plan->root_part, uuid, sizeof(uuid))) {
        show_message("Failed to get root partition UUID");
        return 0;
    }

    if (uefi) {
        if (plan->reuse_efi) {
            /* Existing ESP owned by another OS. Use GRUB with --efi-directory
             * pointing at the already-mounted ESP. Do NOT run bootctl install
             * (systemd-boot would compete with the existing bootloader). */
            LOG_INFO("UEFI alongside: installing GRUB into existing ESP %s", plan->efi_part);
            snprintf(cmd, sizeof(cmd),
                "arch-chroot /mnt grub-install --target=x86_64-efi "
                "--efi-directory=/boot --bootloader-id=luksarchy --recheck 2>> %s",
                LOG_PATH);
            if (system(cmd) != 0) {
                show_message("Failed to install GRUB (UEFI)");
                return 0;
            }
            snprintf(cmd, sizeof(cmd),
                "arch-chroot /mnt grub-mkconfig -o /boot/grub/grub.cfg 2>> %s", LOG_PATH);
            if (system(cmd) != 0) {
                show_message("Failed to generate GRUB config");
                return 0;
            }
        } else {
            /* Fresh ESP we own: systemd-boot is fine. */
            LOG_INFO("Fresh ESP: installing systemd-boot");
            if (!chroot_exec("bootctl install")) {
                LOG_ERROR("bootctl install failed");
                show_message("Failed to install bootloader");
                return 0;
            }

            if (!write_file("/mnt/boot/loader/loader.conf",
                "default luksarchy.conf\n"
                "timeout 3\n"
                "console-mode max\n"
                "editor no\n")) {
                show_message("Failed to create loader config");
                return 0;
            }

            if (!create_directory("/mnt/boot/loader/entries", 0755)) {
                return 0;
            }

            char boot_entry[512];
            snprintf(boot_entry, sizeof(boot_entry),
                "title   Luksarchy\n"
                "linux   /vmlinuz-linux\n"
                "initrd  /initramfs-linux.img\n"
                "options root=UUID=%s rw\n",
                uuid);

            if (!write_file("/mnt/boot/loader/entries/luksarchy.conf", boot_entry)) {
                show_message("Failed to create boot entry");
                return 0;
            }
        }
    } else {
        LOG_INFO("Installing GRUB to /dev/%s (offline)", plan->disk);
        snprintf(cmd, sizeof(cmd),
            "arch-chroot /mnt grub-install --target=i386-pc /dev/%s 2>> %s",
            plan->disk, LOG_PATH);
        if (system(cmd) != 0) {
            show_message("Failed to install GRUB");
            return 0;
        }

        snprintf(cmd, sizeof(cmd),
            "arch-chroot /mnt grub-mkconfig -o /boot/grub/grub.cfg 2>> %s", LOG_PATH);
        if (system(cmd) != 0) {
            show_message("Failed to generate GRUB config");
            return 0;
        }
    }

    show_message("Bootloader installed successfully!");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Hyprland + user setup                                              */
/* ------------------------------------------------------------------ */

static int setup_common_configs(const char *username) {
    char cmd[4096];

    create_directory("/mnt/usr/share/wallpapers", 0755);
    system("cp /usr/share/wallpapers/wall1.jpg /mnt/usr/share/wallpapers/wall1.jpg 2>/dev/null || true");

    create_directory("/mnt/usr/share/luksarchy", 0755);
    system("cp /usr/share/luksarchy/favicon.png /mnt/usr/share/luksarchy/favicon.png 2>/dev/null || true");

    create_directory("/mnt/usr/share/themes", 0755);
    system("cp -r /usr/share/luksarchy/Tokyonight-Dark /mnt/usr/share/themes/ 2>/dev/null || true");

    snprintf(cmd, sizeof(cmd), "/mnt/home/%s/.config/firefox", username);
    create_directory(cmd, 0755);
    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/firefox/default-release/* /mnt/home/%s/.config/firefox/ 2>/dev/null || true",
        username);
    system(cmd);
    snprintf(cmd, sizeof(cmd),
        "arch-chroot /mnt chown -R %s:%s /home/%s/.config/firefox",
        username, username, username);
    system(cmd);

    create_directory("/mnt/usr/lib/firefox/distribution", 0755);
    system("cp /usr/share/luksarchy/firefox-policies/policies.json "
           "/mnt/usr/lib/firefox/distribution/ 2>/dev/null || true");

    create_directory("/mnt/usr/share/applications", 0755);
    write_file("/mnt/usr/share/applications/firefox.desktop",
        "[Desktop Entry]\n"
        "Name=Firefox\n"
        "GenericName=Web Browser\n"
        "Exec=sh -c 'firefox --profile $HOME/.config/firefox'\n"
        "Type=Application\n"
        "Icon=firefox\n"
        "Categories=Network;WebBrowser;\n"
        "MimeType=text/html;text/xml;application/xhtml+xml;application/vnd.mozilla.xul+xml;\n"
    );

    snprintf(cmd, sizeof(cmd), "/mnt/home/%s/.config", username);
    create_directory(cmd, 0755);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/alacritty /mnt/home/%s/.config/alacritty 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/rofi /mnt/home/%s/.config/rofi 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/fastfetch /mnt/home/%s/.config/fastfetch 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/hypr /mnt/home/%s/.config/hypr 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/waybar /mnt/home/%s/.config/waybar 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "cp -r /usr/share/luksarchy/zathura /mnt/home/%s/.config/zathura 2>/dev/null || true",
        username);
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "arch-chroot /mnt chown -R %s:%s /home/%s/.config",
        username, username, username);
    system(cmd);

    return 1;
}

static int setup_autologin_getty(const char *username) {
    char autologin_exec[512];
    snprintf(autologin_exec, sizeof(autologin_exec),
             "ExecStart=-/sbin/agetty -o \"-p -f -- \\\\u\" --noclear --autologin %s %%I $TERM",
             username);

    Config_Entry autologin_entries[] = {
        {"[Service]", ""},
        {"ExecStart=", ""},
        {autologin_exec, ""}
    };

    Systemd_Override autologin = {
        "getty@tty1.service",
        "getty@tty1.service.d",
        "autologin.conf",
        autologin_entries,
        3
    };

    if (!setup_systemd_override(&autologin)) {
        LOG_ERROR("Failed to setup autologin");
        return 0;
    }
    return 1;
}

static int setup_ly_autologin(const char *username) {
    /* ly's config.ini lives at /etc/ly/config.ini after install. */
    create_directory("/mnt/etc/ly", 0755);

    /* Only write if ly config dir exists (i.e. ly was installed). */
    char test[512];
    snprintf(test, sizeof(test), "/mnt/etc/ly/config.ini");
    struct stat st;
    if (stat(test, &st) != 0) {
        LOG_WARN("ly config.ini not found — skipping ly autologin");
        return 0;
    }

    char cfg[512];
    snprintf(cfg, sizeof(cfg),
        "[Ly]\n"
        "animation = matrix\n"
        "clock = %s\n"
        "auto_login_user = %s\n"
        "default_session = hyprland\n",
        "%H:%M:%S", username);

    if (!write_file("/mnt/etc/ly/config.ini", cfg)) {
        LOG_WARN("Failed to write ly config.ini");
        return 0;
    }

    if (!chroot_exec("systemctl enable ly 2>/dev/null")) {
        LOG_WARN("Failed to enable ly.service");
        return 0;
    }
    LOG_INFO("ly autologin configured for %s", username);
    return 1;
}

static const char *BASHRC_CONTENT =
    "export PATH=\"$HOME/.local/bin:$PATH\"\n"
    "export EDITOR=\"nvim\"\n"
    "\n"
    "alias ls='ls --color=auto'\n"
    "alias la='ls -a'\n"
    "alias ll='ls -la'\n"
    "alias ..='cd ..'\n"
    "alias ...='cd ../..'\n"
    "alias grep='grep --color=auto'\n"
    "\n"
    "export PS1=\"\\[\\e[38;5;75m\\]\\u@\\h \\[\\e[38;5;113m\\]\\w \\[\\e[38;5;189m\\]\\$ \\[\\e[0m\\]\"\n"
    "\n"
    "fastfetch\n";

/* Only used if ly is NOT installed. */
static const char *BASH_PROFILE_CONTENT =
    "if [ -z \"$WAYLAND_DISPLAY\" ] && [ -z \"$DISPLAY\" ] && [ \"${XDG_VTNR:-0}\" -eq 1 ]; then\n"
    "  exec Hyprland\n"
    "fi\n";

static int configure_hyprland(const char *username, int ly_installed) {
    char cmd[4096];
    int rows, cols;
    get_terminal_size(&rows, &cols);

    clear_screen();
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    printf("\033[%d;%dH\033[37mConfiguring Hyprland...\033[0m", 10, logo_start);
    fflush(stdout);

    LOG_INFO("Starting Hyprland configuration for user: %s (ly=%d)", username, ly_installed);

    setup_common_configs(username);

    snprintf(cmd, sizeof(cmd),
        "arch-chroot /mnt chown -R %s:%s /home/%s/.config",
        username, username, username);
    system(cmd);

    Dotfile dotfiles[] = {
        { ".bashrc", BASHRC_CONTENT, 0644 }
    };

    for (size_t i = 0; i < sizeof(dotfiles) / sizeof(dotfiles[0]); i++) {
        if (!create_user_dotfile(username, &dotfiles[i])) {
            LOG_ERROR("Failed to create dotfile: %s", dotfiles[i].filename);
            return 0;
        }
    }

    if (ly_installed) {
        /* ly owns tty1; do not add getty autologin or .bash_profile exec,
         * otherwise they fight over tty1. */
        if (!setup_ly_autologin(username)) {
            LOG_WARN("ly autologin failed; falling back to getty autologin");
            ly_installed = 0;
        }
    }

    if (!ly_installed) {
        Dotfile bp = { ".bash_profile", BASH_PROFILE_CONTENT, 0644 };
        if (!create_user_dotfile(username, &bp)) {
            LOG_ERROR("Failed to create .bash_profile");
            return 0;
        }
        if (!setup_autologin_getty(username)) {
            return 0;
        }
    }

    LOG_INFO("Hyprland configuration completed successfully");
    show_message("Hyprland configured successfully!");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    logger_init(LOG_PATH);
    LOG_INFO("Luksarchy installer started");

    char username[256] = "";
    char password[256] = "";
    char confirmed_password[256] = "";
    char hostname[256] = "";
    char keyboard[256] = "";
    char timezone[256] = "";

    if (!get_form_input(username, password, confirmed_password, hostname, keyboard, timezone)) {
        logger_close();
        return 1;
    }

    /* --- Disk selection --- */
    Partition_Plan plan;
    memset(&plan, 0, sizeof(plan));

    if (!select_disk(plan.disk)) {
        LOG_INFO("Installation cancelled at disk selection");
        logger_close();
        return 1;
    }

    int mode = select_install_mode();
    if (mode < 0) {
        LOG_INFO("Installation cancelled at mode selection");
        logger_close();
        return 1;
    }
    plan.mode = (enum Install_Mode)mode;

    if (plan.mode == INSTALL_FREE_SPACE) {
        char regions[32][128];
        int n = list_free_space(plan.disk, regions, 32);
        if (n <= 0) {
            show_message("No free space found on this disk");
            logger_close();
            return 1;
        }

        /* Build human-readable labels: "Free space detected: X.X GB on /dev/<disk>" */
        char labels[32][128];
        const char *ptrs[32];
        for (int i = 0; i < n; i++) {
            unsigned long long s = 0, e = 0;
            sscanf(regions[i], "%llu:%llu", &s, &e);
            /* sectors are 512 bytes; convert to GiB */
            double gib = (double)(e - s) * 512.0 / (1024.0 * 1024.0 * 1024.0);
            snprintf(labels[i], sizeof(labels[i]),
                     "Free space detected: %.1f GB on /dev/%s",
                     gib, plan.disk);
            ptrs[i] = labels[i];
        }

        int sel = select_from_menu(ptrs, n);
        if (sel < 0) {
            logger_close();
            return 1;
        }

        /* Parse start/end back from the untouched regions[] entry. */
        unsigned long long sel_s = 0, sel_e = 0;
        if (sscanf(regions[sel], "%llu:%llu", &sel_s, &sel_e) != 2) {
            show_message("Malformed free region");
            logger_close();
            return 1;
        }
        snprintf(plan.free_start, sizeof(plan.free_start), "%llu", sel_s);
        snprintf(plan.free_end,   sizeof(plan.free_end),   "%llu", sel_e);

        if (is_uefi_system()) {
            char esp[64];
            if (detect_esp(plan.disk, esp, sizeof(esp))) {
                plan.reuse_efi = 1;
                snprintf(plan.efi_part, sizeof(plan.efi_part), "%s", esp);
            }
        }
        LOG_INFO("Alongside install: disk=%s free=%s..%s reuse_efi=%d esp=%s",
                 plan.disk, plan.free_start, plan.free_end,
                 plan.reuse_efi, plan.efi_part);
    } else {
        /* Whole-disk mode: still need explicit confirmation. */
        int rows, cols;
        get_terminal_size(&rows, &cols);
        clear_screen();
        draw_logo(cols);
        int logo_start = get_logo_start(cols);
        printf("\033[%d;%dH\033[37mWARNING: All data on \033[31m/dev/%s\033[37m will be destroyed!\033[0m",
               10, logo_start, plan.disk);
        printf("\033[%d;%dH\033[37mType 'yes' to confirm: \033[0m", 12, logo_start);
        fflush(stdout);

        char confirm[256];
        struct termios old_term;
        tcgetattr(STDIN_FILENO, &old_term);
        struct termios new_term = old_term;
        new_term.c_lflag |= (ECHO | ICANON);
        new_term.c_lflag &= ~ISIG;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);
        if (fgets(confirm, sizeof(confirm), stdin) == NULL) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
            logger_close();
            return 1;
        }
        confirm[strcspn(confirm, "\n")] = '\0';
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        if (strcmp(confirm, "yes") != 0) {
            show_message("Installation cancelled");
            logger_close();
            return 1;
        }
    }

    LOG_INFO("Selected disk: %s (mode=%d)", plan.disk, plan.mode);

    CHECK_OR_FAIL(partition_disk_plan(&plan), "Failed to partition disk");
    CHECK_OR_FAIL(install_packages_impl(HYPRLAND_PACKAGES), "Failed to install packages");

    int ly_installed = install_ly();

    CHECK_OR_FAIL(
        configure_system_impl(username, password, hostname, keyboard, timezone, plan.disk),
        "Failed to configure system"
    );
    CHECK_OR_FAIL(install_bootloader(&plan), "Failed to install bootloader");
    configure_hyprland(username, ly_installed);

    system("cp " LOG_PATH " /mnt/var/log/luksarchy-install.log");

    clear_screen();
    int rows, cols;
    get_terminal_size(&rows, &cols);
    draw_logo(cols);

    int logo_start = get_logo_start(cols);
    printf("\033[%d;%dH\033[1;32mInstallation complete!\033[0m\n", 10, logo_start);
    printf("\033[%d;%dH\033[37mPress Enter to reboot...\033[0m\n", 12, logo_start);
    fflush(stdout);

    char c;
    enable_raw_mode();
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\r' || c == '\n') break;
    }
    disable_raw_mode();

    LOG_INFO("Luksarchy installer finished - scheduling reboot");
    logger_close();

    sync();
    system("sleep 2 && reboot &");
    exit(0);
}
