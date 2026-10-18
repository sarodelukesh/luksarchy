#include "build_iso.h"
#include <stdarg.h>

static FILE *log_file = NULL;

/* ─── Logging ─────────────────────────────────────────────────────────────── */

void logger_init(const char *log_path) {
    log_file = fopen(log_path, "a");
    if (log_file) {
        time_t now = time(NULL);
        char *timestamp = ctime(&now);
        timestamp[strlen(timestamp) - 1] = '\0';
        fprintf(log_file, "\n=== Luksarchy ISO Build Log - %s ===\n", timestamp);
        fflush(log_file);
    }
}

void logger_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

/* Helper: emit a log line to both a FILE* and a va_list. */
static void log_to(FILE *out, const char *prefix, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);

    fprintf(out, "%s", prefix);
    vfprintf(out, fmt, ap2);
    fprintf(out, "\n");

    va_end(ap2);

    if (log_file) {
        va_list ap3;
        va_copy(ap3, ap);
        fprintf(log_file, "%s", prefix);
        vfprintf(log_file, fmt, ap3);
        fprintf(log_file, "\n");
        fflush(log_file);
        va_end(ap3);
    }
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_to(stdout, "[INFO]  ", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_to(stderr, "[WARN]  ", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_to(stderr, "[ERROR] ", fmt, ap);
    va_end(ap);
}

/* ─── Command execution ───────────────────────────────────────────────────── */

/*
 * run_command() - run a shell command via system(3).
 * Returns 1 on success, 0 on failure (non-zero exit or fork error).
 */
int run_command(const char *cmd) {
    log_info("Running: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        log_error("Command failed (exit %d): %s", ret, cmd);
        return 0;
    }
    return 1;
}

/*
 * run_command_in_container() - run cmd inside a distrobox container.
 * Falls back to run_command() when container_type is CONTAINER_NONE.
 */
int run_command_in_container(const char *cmd, const Build_Config *config) {
    if (config->container_type != CONTAINER_DISTROBOX) {
        return run_command(cmd);
    }

    char container_cmd[CMD_MAX_LEN];
    snprintf(container_cmd, sizeof(container_cmd),
             "distrobox enter %s -- sh -c '%s'",
             config->distrobox_name, cmd);
    return run_command(container_cmd);
}

/* ─── Filesystem helpers ──────────────────────────────────────────────────── */

int create_directory(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0)
        return 1;

    if (mkdir(path, mode) != 0) {
        log_error("Failed to create directory: %s", path);
        return 0;
    }
    return 1;
}

/* ─── Build steps ─────────────────────────────────────────────────────────── */

int build_luksarchy_static(const Build_Config *config) {
    log_info("Building luksarchy static binary...");

    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "cd '%s' && make clean && make static",
             config->luksarchy_src);

    int ok = (config->use_container && config->container_type == CONTAINER_DISTROBOX)
             ? run_command_in_container(cmd, config)
             : run_command(cmd);

    if (!ok) {
        log_error("Failed to build luksarchy-static");
        return 0;
    }

    char binary_path[PATH_MAX_LEN];
    snprintf(binary_path, sizeof(binary_path),
             "%s/luksarchy-static", config->luksarchy_src);

    struct stat st;
    if (stat(binary_path, &st) != 0) {
        log_error("luksarchy-static binary not found at %s after build",
                  binary_path);
        return 0;
    }

    log_info("Built luksarchy-static successfully");
    return 1;
}

int clean_airootfs(const Build_Config *config) {
    log_info("Cleaning airootfs...");

    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd),
             "sudo rm -rf '%s/airootfs/usr'", config->iso_profile);
    if (!run_command(cmd))
        log_warn("Failed to clean airootfs/usr (continuing)");

    snprintf(cmd, sizeof(cmd),
             "sudo rm -rf '%s/airootfs/root/luksarchy'", config->iso_profile);
    if (!run_command(cmd))
        log_warn("Failed to clean airootfs/root/luksarchy (continuing)");

    /* Also clean any previous copy of the local repo inside airootfs. */
    snprintf(cmd, sizeof(cmd),
             "sudo rm -rf '%s/airootfs/repo'", config->iso_profile);
    run_command(cmd);

    /* Remove any previously-enabled luksarchy-installer.service symlink so
     * it does not auto-start and fight getty@tty1 for ownership of tty1.
     * The live ISO now launches the installer via getty@tty1 autologin
     * (.bash_profile -> .automated_script.sh -> luksarchy-run) instead. */
    snprintf(cmd, sizeof(cmd),
             "sudo rm -f '%s/airootfs/etc/systemd/system/multi-user.target.wants/luksarchy-installer.service'",
             config->iso_profile);
    run_command(cmd);

    return 1;
}

int clean_work_dir(const Build_Config *config) {
    log_info("Cleaning work directory...");

    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd),
             "sudo umount -R '%s' 2>/dev/null || true", config->work_dir);
    run_command(cmd);

    snprintf(cmd, sizeof(cmd), "sudo rm -rf '%s'", config->work_dir);
    if (!run_command(cmd)) {
        log_error("Failed to remove work directory: %s", config->work_dir);
        return 0;
    }

    sync();
    sleep(1);
    return 1;
}

/* ─── pacman.conf generation ──────────────────────────────────────────────── */

/*
 * write_build_pacman_conf() - generate the profile's pacman.conf for mkarchiso.
 *
 * Server = file://<abs_repo_dir> so that pacman on the host resolves the local
 * repo correctly.  The live-system config (airootfs/etc/pacman.conf) uses
 * Server = file:///repo instead (see write_live_pacman_conf).
 */
static int write_build_pacman_conf(const Build_Config *config,
                                   const char *abs_repo_dir) {
    char pacman_conf_path[PATH_MAX_LEN];
    snprintf(pacman_conf_path, sizeof(pacman_conf_path),
             "%s/pacman.conf", config->iso_profile);

    log_info("Writing build-time pacman.conf (repo: %s)", abs_repo_dir);

    FILE *fp = fopen(pacman_conf_path, "w");
    if (!fp) {
        log_error("Failed to open %s for writing", pacman_conf_path);
        return 0;
    }

    fprintf(fp,
        "#\n"
        "# Build-time pacman.conf for Luksarchy ISO (generated by build_iso)\n"
        "#\n"
        "[options]\n"
        "HoldPkg            = pacman glibc\n"
        "Architecture       = auto\n"
        "CheckSpace\n"
        "SigLevel           = Required DatabaseOptional\n"
        "LocalFileSigLevel  = Optional\n"
        "ParallelDownloads  = 5\n"
        "\n"
        "# Local repository (highest priority)\n"
        "[core-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server   = file://%s\n"
        "\n"
        "[extra-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server   = file://%s\n"
        "\n"
        "# Official mirrors - fallback when a package is missing from the local repo\n"
        "[core]\n"
        "Include = /etc/pacman.d/mirrorlist\n"
        "\n"
        "[extra]\n"
        "Include = /etc/pacman.d/mirrorlist\n",
        abs_repo_dir, abs_repo_dir);

    fclose(fp);
    log_info("Wrote %s", pacman_conf_path);
    return 1;
}

/*
 * write_live_pacman_conf() - write airootfs/etc/pacman.conf for the finished ISO.
 * The live system finds its packages at /repo (embedded in the squashfs).
 */
static int write_live_pacman_conf(const Build_Config *config) {
    char conf_dir[PATH_MAX_LEN];
    char conf_path[PATH_MAX_LEN];
    char cmd[CMD_MAX_LEN];

    snprintf(conf_dir,  sizeof(conf_dir),  "%s/airootfs/etc", config->iso_profile);
    snprintf(conf_path, sizeof(conf_path), "%s/pacman.conf",  conf_dir);

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", conf_dir);
    if (!run_command(cmd))
        return 0;

    /* mkarchiso/sudo may leave airootfs/etc owned by root on rebuilds.
       Make this directory writable by the invoking build user before fopen. */
    snprintf(cmd, sizeof(cmd), "sudo chown -R %u:%u '%s'",
             (unsigned)getuid(), (unsigned)getgid(), conf_dir);
    if (!run_command(cmd)) {
        log_error("Could not take ownership of %s", conf_dir);
        return 0;
    }

    log_info("Writing live-system pacman.conf (Server = file:///repo)");

    FILE *fp = fopen(conf_path, "w");
    if (!fp) {
        log_error("Failed to open %s for writing", conf_path);
        return 0;
    }

    fprintf(fp,
        "#\n"
        "# Live-system pacman.conf for Luksarchy ISO\n"
        "# The local repo is embedded at /repo inside the squashfs.\n"
        "#\n"
        "[options]\n"
        "HoldPkg            = pacman glibc\n"
        "Architecture       = auto\n"
        "CheckSpace\n"
        "SigLevel           = Required DatabaseOptional\n"
        "LocalFileSigLevel  = Optional\n"
        "ParallelDownloads  = 5\n"
        "\n"
        "[core-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server   = file:///repo\n"
        "\n"
        "[extra-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server   = file:///repo\n"
        "\n"
        "# Official mirrors kept commented out for pure offline use\n"
        "#[core]\n"
        "#Include = /etc/pacman.d/mirrorlist\n"
        "#[extra]\n"
        "#Include = /etc/pacman.d/mirrorlist\n");

    fclose(fp);
    return 1;
}

/* ─── Local package repository ────────────────────────────────────────────── */

/*
 * If you add or remove a package in luksarchy.c's HYPRLAND_PACKAGES, update
 * this string too.  A mismatch causes pacstrap to fail at install time with
 * "target not found: <package>".
 */
static const char *BUILD_ISO_PACKAGES =
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
    "ttf-iosevka-nerd ttf-jetbrains-mono-nerd "
    "polkit-gnome qt5-wayland qt6-wayland "
    "grim slurp wl-clipboard brightnessctl playerctl "
    "noto-fonts noto-fonts-emoji ly";

/*
 * repo_has_packages() - return 1 if iso/repo/ already contains at least one
 * .pkg.tar.zst file, meaning a previous run (or --pull) already downloaded
 * everything.  We use this to skip the slow network step on rebuilds.
 */
static int repo_has_packages(const char *abs_repo_dir) {
    char cmd[CMD_MAX_LEN];
    /* ls glob exits 0 when at least one file matches */
    snprintf(cmd, sizeof(cmd),
             "ls '%s'/*.pkg.tar.zst >/dev/null 2>&1", abs_repo_dir);
    return system(cmd) == 0;
}

/*
 * build_local_repo() - ensure iso/repo/ is populated and databases are current.
 *
 * After this function:
 *   iso/repo/               - all .pkg.tar.zst + core-local.db + extra-local.db
 *   iso/pacman.conf         - host-path Server for mkarchiso
 *   airootfs/etc/pacman.conf- file:///repo Server for the live system
 *
 * Package download is SKIPPED when iso/repo/ already contains .pkg.tar.zst
 * files (e.g. from a previous run or from --pull).  repo-add always runs
 * because it is fast, local, and must reflect whatever is in the directory.
 *
 * Packages are copied into airootfs/repo by prepare_airootfs().
 */
int build_local_repo(const Build_Config *config, const char *package_list) {
    log_info("Building local package repository in iso/repo/ ...");

    char repo_dir[PATH_MAX_LEN];
    snprintf(repo_dir, sizeof(repo_dir), "%s/repo", config->iso_profile);

    char cmd[CMD_MAX_LEN];
    char abs_repo_dir[PATH_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", repo_dir);
    if (!run_command(cmd)) {
        log_error("Failed to create repo directory: %s", repo_dir);
        return 0;
    }

    if (realpath(repo_dir, abs_repo_dir) == NULL)
        snprintf(abs_repo_dir, sizeof(abs_repo_dir), "%s", repo_dir);

    /* ── Download step (skipped when packages already present) ─────────── */
    if (repo_has_packages(abs_repo_dir)) {
        log_info("iso/repo/ already populated — skipping download.");
    } else {
        log_info("Downloading packages into %s ...", abs_repo_dir);

        const char *pacman_conf_path = "/tmp/luksarchy-pacman.conf";
        const char *pacman_db_path   = "/tmp/luksarchy-repodb";

        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", pacman_db_path);
        run_command(cmd);

        {
            FILE *fp = fopen(pacman_conf_path, "w");
            if (!fp) {
                log_error("Failed to write temporary pacman.conf at %s",
                          pacman_conf_path);
                return 0;
            }
            fprintf(fp,
                "[options]\n"
                "Architecture      = auto\n"
                "SigLevel          = Never\n"
                "ParallelDownloads = 5\n"
                "CacheDir          = %s\n"
                "DBPath            = %s\n"
                "\n"
                "[core]\n"
                "Include = /etc/pacman.d/mirrorlist\n"
                "\n"
                "[extra]\n"
                "Include = /etc/pacman.d/mirrorlist\n",
                abs_repo_dir, pacman_db_path);
            fclose(fp);
        }

        snprintf(cmd, sizeof(cmd),
                 "fakeroot -- pacman --config '%s' -Syw --noconfirm %s",
                 pacman_conf_path, package_list);
        if (!run_command(cmd)) {
            log_error("Failed to download packages (internet required for this step)");
            return 0;
        }
    }

    /* ── repo-add (always: fast, local, keeps db in sync with files) ───── */

    /* Remove stale database files so repo-add starts clean. */
    snprintf(cmd, sizeof(cmd),
             "rm -f '%s'/core-local.db* '%s'/core-local.files* "
             "      '%s'/extra-local.db* '%s'/extra-local.files*",
             abs_repo_dir, abs_repo_dir, abs_repo_dir, abs_repo_dir);
    run_command(cmd);

    log_info("Running repo-add for core-local ...");
    snprintf(cmd, sizeof(cmd),
             "repo-add '%s/core-local.db.tar.zst' '%s'/*.pkg.tar.zst 2>&1",
             abs_repo_dir, abs_repo_dir);
    if (!run_command(cmd)) {
        log_error("repo-add failed for core-local");
        return 0;
    }

    log_info("Running repo-add for extra-local ...");
    snprintf(cmd, sizeof(cmd),
             "repo-add '%s/extra-local.db.tar.zst' '%s'/*.pkg.tar.zst 2>&1",
             abs_repo_dir, abs_repo_dir);
    if (!run_command(cmd)) {
        log_error("repo-add failed for extra-local");
        return 0;
    }

    char db_path[PATH_MAX_LEN];
    snprintf(db_path, sizeof(db_path), "%s/core-local.db", abs_repo_dir);
    struct stat st;
    if (stat(db_path, &st) != 0) {
        log_error("core-local.db missing after repo-add");
        return 0;
    }

    if (!write_build_pacman_conf(config, abs_repo_dir))
        return 0;
    if (!write_live_pacman_conf(config))
        return 0;

    log_info("Local repo built successfully at %s", repo_dir);
    log_info("  core-local.db  ok");
    log_info("  extra-local.db ok");
    return 1;
}

/* ─── airootfs preparation ────────────────────────────────────────────────── */

int prepare_airootfs(const Build_Config *config) {
    log_info("Preparing airootfs...");

    char cmd[CMD_MAX_LEN];
    char src_path[PATH_MAX_LEN];
    char dest_path[PATH_MAX_LEN];

    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s/airootfs/usr/local/bin'", config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to create airootfs/usr/local/bin");
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s/airootfs/usr/share'", config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to create airootfs/usr/share");
        return 0;
    }

    /* Install binary as /usr/local/bin/luksarchy */
    snprintf(src_path,  sizeof(src_path),
             "%s/luksarchy-static", config->luksarchy_src);
    snprintf(dest_path, sizeof(dest_path),
             "%s/airootfs/usr/local/bin/luksarchy", config->iso_profile);
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src_path, dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to copy luksarchy binary");
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "sudo chmod 755 '%s'", dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to set permissions on luksarchy binary");
        return 0;
    }

    /* Assets -> /usr/share/luksarchy */
    snprintf(src_path,  sizeof(src_path),  "%s/assets", config->luksarchy_src);
    snprintf(dest_path, sizeof(dest_path),
             "%s/airootfs/usr/share/luksarchy", config->iso_profile);
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to create luksarchy share directory");
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "cp -r '%s'/* '%s'", src_path, dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to copy luksarchy config files");
        return 0;
    }

    snprintf(dest_path, sizeof(dest_path),
             "%s/airootfs/usr/share/wallpapers", config->iso_profile);
    snprintf(cmd, sizeof(cmd),
             "cp -r '%s/wallpapers' '%s'", src_path, dest_path);
    if (!run_command(cmd))
        log_warn("Failed to copy wallpapers (continuing)");

    /* Embed local repo into airootfs/repo so the live system can install offline. */
    log_info("Embedding local repo into airootfs/repo ...");
    snprintf(src_path,  sizeof(src_path),
             "%s/repo", config->iso_profile);
    snprintf(dest_path, sizeof(dest_path),
             "%s/airootfs/repo", config->iso_profile);
    snprintf(cmd, sizeof(cmd),
             "mkdir -p '%s' && cp -a '%s'/* '%s/'",
             dest_path, src_path, dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to copy local repo into airootfs");
        return 0;
    }

    /* ── luksarchy-run wrapper ───────────────────────────────────────────────
     *
     * A small shell script that re-launches luksarchy on failure so the user
     * can retry without rebooting.  Also the entry point for manual restarts:
     *
     *   luksarchy-run          (from any shell on the live system)
     *
     * The script is installed at /usr/local/bin/luksarchy-run and is what the
     * luksarchy-installer.service runs instead of calling luksarchy directly.
     */
    {
        const char *wrapper =
            "#!/bin/sh\n"
            "# luksarchy-run — restart wrapper for the Luksarchy installer.\n"
            "# Run this to (re)start the installer at any time:\n"
            "#   luksarchy-run\n"
            "\n"
            "LOG=/tmp/luksarchy-install.log\n"
            "\n"
            "# The installer owns tty1 as a full-screen console TUI. Start immediately;\n"
            "# do not wait for graphical.target — the live ISO has no display manager.\n"
            "while true; do\n"
            "    clear\n"
            "    /usr/local/bin/luksarchy\n"
            "    EXIT=$?\n"
            "\n"
            "    # Exit code 0 means installation finished successfully — don't loop.\n"
            "    if [ \"$EXIT\" -eq 0 ]; then\n"
            "        exit 0\n"
            "    fi\n"
            "\n"
            "    # Non-zero: installer crashed or was cancelled.\n"
            "    printf '\\n\\033[1;31mInstaller exited with code %d.\\033[0m\\n' \"$EXIT\"\n"
            "    [ -f \"$LOG\" ] && printf '\\033[90mLog: %s\\033[0m\\n' \"$LOG\"\n"
            "    printf '\\n\\033[1mOptions:\\033[0m\\n'\n"
            "    printf '  r  Restart installer\\n'\n"
            "    printf '  s  Drop to shell\\n'\n"
            "    printf '  q  Power off\\n'\n"
            "    printf '\\nChoice [r/s/q]: '\n"
            "\n"
            "    # Read one char without waiting for Enter.\n"
            "    old=$(stty -g)\n"
            "    stty raw -echo\n"
            "    choice=$(dd bs=1 count=1 2>/dev/null)\n"
            "    stty \"$old\"\n"
            "    printf '\\n'\n"
            "\n"
            "    case \"$choice\" in\n"
            "        r|R) continue ;;\n"
            "        s|S) exec /bin/bash --login ;;\n"
            "        q|Q) poweroff ;;\n"
            "        *)   continue ;;\n"   /* default: restart */
            "    esac\n"
            "done\n";

        snprintf(dest_path, sizeof(dest_path),
                 "%s/airootfs/usr/local/bin/luksarchy-run", config->iso_profile);

        FILE *fp = fopen(dest_path, "w");
        if (!fp) {
            log_error("Failed to create luksarchy-run wrapper at %s", dest_path);
            return 0;
        }
        fputs(wrapper, fp);
        fclose(fp);

        snprintf(cmd, sizeof(cmd), "sudo chmod 755 '%s'", dest_path);
        if (!run_command(cmd)) {
            log_error("Failed to set permissions on luksarchy-run");
            return 0;
        }
        log_info("Wrote luksarchy-run wrapper");
    }

    /* ── systemd service: auto-start installer on tty1 ──────────────────────
     *
     * Overrides getty@tty1 so the installer starts automatically on boot.
     * The user can still reach a shell via the 's' option in luksarchy-run,
     * or via tty2 (Alt+F2).
     */
    {
        const char *service =
            "[Unit]\n"
            "Description=Luksarchy Installer\n"
            "After=systemd-user-sessions.service\n"
            "Before=display-manager.service\n"
            "Conflicts=display-manager.service\n"
            "\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=/usr/local/bin/luksarchy-run\n"
            "StandardInput=tty-force\n"
            "StandardOutput=tty\n"
            "TTYPath=/dev/tty1\n"
            "TTYReset=yes\n"
            "TTYVHangup=no\n"
            "Restart=no\n"       /* luksarchy-run handles its own loop */
            "User=root\n"
            "\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n";

        snprintf(dest_path, sizeof(dest_path),
                 "%s/airootfs/etc/systemd/system/luksarchy-installer.service",
                 config->iso_profile);

        /* Ensure the systemd/system directory exists. */
        snprintf(cmd, sizeof(cmd),
                 "mkdir -p '%s/airootfs/etc/systemd/system'", config->iso_profile);
        run_command(cmd);

        FILE *fp = fopen(dest_path, "w");
        if (!fp) {
            log_error("Failed to create luksarchy-installer.service at %s", dest_path);
            return 0;
        }
        fputs(service, fp);
        fclose(fp);
        log_info("Wrote luksarchy-installer.service (not auto-enabled; getty@tty1 autologin owns tty1)");

        /* Boot the live ISO to multi-user.target, not graphical.target.
         * graphical.target pulls in display-manager.service, which conflicts
         * with the installer service — that combination deadlocks and leaves
         * tty1 stuck. The installer owns tty1 as a console TUI; a normal
         * getty remains available on tty2 (Alt+F2) for a fallback shell. */
        snprintf(cmd, sizeof(cmd),
                 "ln -sfn /usr/lib/systemd/system/multi-user.target "
                 "'%s/airootfs/etc/systemd/system/default.target' && "
                 "rm -f '%s/airootfs/etc/systemd/system/display-manager.service' "
                 "'%s/airootfs/etc/systemd/system/ly.service' "
                 "'%s/airootfs/etc/systemd/system/greetd.service' "
                 "'%s/airootfs/etc/systemd/system/sddm.service' "
                 "'%s/airootfs/etc/systemd/system/gdm.service' "
                 "'%s/airootfs/etc/systemd/system/lightdm.service'",
                 config->iso_profile, config->iso_profile, config->iso_profile,
                 config->iso_profile, config->iso_profile, config->iso_profile,
                 config->iso_profile);
        if (!run_command(cmd)) {
            log_error("Failed to set default boot target");
            return 0;
        }
        log_info("Configured multi-user boot; installer owns tty1, shell on tty2");
    }

    log_info("Setting ownership for airootfs...");
    snprintf(cmd, sizeof(cmd),
             "sudo chown -R root:root '%s/airootfs/usr' '%s/airootfs/repo' "
             "'%s/airootfs/etc'",
             config->iso_profile, config->iso_profile, config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to set ownership on airootfs");
        return 0;
    }

    return 1;
}

/* ─── mkarchiso ───────────────────────────────────────────────────────────── */

int run_mkarchiso(const Build_Config *config) {
    log_info("Building ISO with mkarchiso...");

    if (!create_directory(config->out_dir,  0755)) return 0;
    if (!create_directory(config->work_dir, 0755)) return 0;

    char cmd[CMD_MAX_LEN];
    snprintf(cmd, sizeof(cmd),
             "sudo mkarchiso -v -w '%s' -o '%s' '%s'",
             config->work_dir, config->out_dir, config->iso_profile);

    if (!run_command(cmd)) {
        log_error("mkarchiso failed");
        return 0;
    }
    return 1;
}

/* ─── ISO discovery ───────────────────────────────────────────────────────── */

const char *find_latest_iso(const char *out_dir) {
    static char iso_path[PATH_MAX_LEN];
    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd),
             "ls -t '%s'/*.iso 2>/dev/null | head -n1", out_dir);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return NULL;

    if (fgets(iso_path, sizeof(iso_path), fp) != NULL) {
        iso_path[strcspn(iso_path, "\n")] = '\0';
        pclose(fp);
        return iso_path;
    }

    pclose(fp);
    return NULL;
}

/* ─── CLI ─────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nOptions:\n");
    printf("  --iso-profile PATH    Path to ISO profile directory (default: ./iso)\n");
    printf("  --out-dir PATH        Output directory for ISO (default: ./out)\n");
    printf("  --distrobox NAME      Build inside a distrobox container\n");
    printf("  --pkg-cache PATH      Host pacman pkg cache dir\n");
    printf("                        (default: /var/cache/pacman/pkg)\n");
    printf("  -h, --help            Show this help message\n");
}

static int parse_args(int argc, char *argv[], Build_Config *config) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iso-profile") == 0 && i + 1 < argc) {
            snprintf(config->iso_profile, sizeof(config->iso_profile),
                     "%s", argv[++i]);
        } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            snprintf(config->out_dir, sizeof(config->out_dir),
                     "%s", argv[++i]);
        } else if (strcmp(argv[i], "--distrobox") == 0 && i + 1 < argc) {
            snprintf(config->distrobox_name, sizeof(config->distrobox_name),
                     "%s", argv[++i]);
            config->use_container  = true;
            config->container_type = CONTAINER_DISTROBOX;
        } else if (strcmp(argv[i], "--pkg-cache") == 0 && i + 1 < argc) {
            snprintf(config->pkg_cache_dir, sizeof(config->pkg_cache_dir),
                     "%s", argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            log_error("Unknown option: %s", argv[i]);
            print_usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

/* ─── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    logger_init("/tmp/build_iso.log");
    log_info("Luksarchy ISO Builder starting...");

    Build_Config config = {
        .work_dir       = "/tmp/luksarchy_iso_work",
        .distrobox_name = "arch",
        .container_type = CONTAINER_NONE,
        .use_container  = false,
        .pkg_cache_dir  = "/var/cache/pacman/pkg",
    };

    if (getcwd(config.luksarchy_src, sizeof(config.luksarchy_src)) == NULL) {
        log_error("Failed to get current directory");
        return 1;
    }
    snprintf(config.iso_profile, sizeof(config.iso_profile),
             "%s/iso", config.luksarchy_src);
    snprintf(config.out_dir, sizeof(config.out_dir),
             "%s/out", config.luksarchy_src);

    if (!parse_args(argc, argv, &config)) {
        logger_close();
        return 1;
    }

    log_info("Luksarchy source : %s", config.luksarchy_src);
    log_info("ISO profile      : %s", config.iso_profile);
    log_info("Work directory   : %s", config.work_dir);
    log_info("Output directory : %s", config.out_dir);
    log_info("Package cache    : %s", config.pkg_cache_dir);

    if (config.use_container)
        log_info("Container mode   : distrobox (%s)", config.distrobox_name);

    if (!build_luksarchy_static(&config)) {
        log_error("Build failed at: luksarchy-static");
        logger_close();
        return 1;
    }

    if (!build_local_repo(&config, BUILD_ISO_PACKAGES)) {
        log_error("Build failed at: local package repository");
        logger_close();
        return 1;
    }

    if (!clean_airootfs(&config)) {
        log_error("Build failed at: clean airootfs");
        logger_close();
        return 1;
    }

    if (!clean_work_dir(&config)) {
        log_error("Build failed at: clean work dir");
        logger_close();
        return 1;
    }

    if (!prepare_airootfs(&config)) {
        log_error("Build failed at: prepare airootfs");
        logger_close();
        return 1;
    }

    if (!run_mkarchiso(&config)) {
        log_error("Build failed at: mkarchiso");
        logger_close();
        return 1;
    }

    if (!clean_work_dir(&config))
        log_warn("Failed to clean work directory after build");

    log_info("Syncing filesystem...");
    sync();
    sleep(2);

    const char *iso_path = find_latest_iso(config.out_dir);
    if (!iso_path) {
        log_error("ISO was built but could not be found in %s", config.out_dir);
        logger_close();
        return 1;
    }

    log_info("===================================");
    log_info("ISO created successfully!");
    log_info("Location: %s", iso_path);
    log_info("Test with: make test");
    log_info("===================================");

    logger_close();
    return 0;
}
