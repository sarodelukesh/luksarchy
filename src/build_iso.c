#include "build_iso.h"
#include <stdarg.h>

static FILE *log_file = NULL;

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

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[INFO] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    if (log_file) {
        va_start(args, fmt);
        fprintf(log_file, "[INFO] ");
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
        va_end(args);
    }
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[ERROR] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    if (log_file) {
        va_start(args, fmt);
        fprintf(log_file, "[ERROR] ");
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
        va_end(args);
    }
}

void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[WARN] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    if (log_file) {
        va_start(args, fmt);
        fprintf(log_file, "[WARN] ");
        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
        va_end(args);
    }
}

int run_command(const char *cmd) {
    log_info("Running: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        log_error("Command failed with code %d: %s", ret, cmd);
        return 0;
    }
    return 1;
}

int run_command_in_container(const char *cmd, const Build_Config *config) {
    char container_cmd[CMD_MAX_LEN];

    if (config->container_type == CONTAINER_DISTROBOX) {
        snprintf(container_cmd, sizeof(container_cmd),
                 "distrobox enter %s -- sh -c '%s'",
                 config->distrobox_name, cmd);
    } else if (config->container_type == CONTAINER_PODMAN) {
        snprintf(container_cmd, sizeof(container_cmd),
                 "podman run --rm --privileged "
                 "-v '%s:/src' "
                 "-v '%s:/profile' "
                 "-v '%s:/out' "
                 "-v '%s:/work' "
                 "docker.io/archlinux:latest sh -c '%s'",
                 config->luksarchy_src,
                 config->iso_profile,
                 config->out_dir,
                 config->work_dir,
                 cmd);
    } else {
        return run_command(cmd);
    }

    return run_command(container_cmd);
}

int create_directory(const char *path, mode_t mode) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return 1;
    }

    if (mkdir(path, mode) != 0) {
        log_error("Failed to create directory: %s", path);
        return 0;
    }
    return 1;
}

int detect_container_runtime(void) {
    if (system("command -v podman >/dev/null 2>&1") == 0) {
        return 1;
    }
    if (system("command -v distrobox >/dev/null 2>&1") == 0) {
        return 1;
    }
    return 0;
}

int check_distrobox_exists(const char *name) {
    char cmd[CMD_MAX_LEN];
    snprintf(cmd, sizeof(cmd), "distrobox list | grep -q '%s'", name);
    return system(cmd) == 0;
}

int build_luksarchy_static(const Build_Config *config) {
    log_info("Building luksarchy static binary...");

    char cmd[CMD_MAX_LEN];

    if (config->use_container && config->container_type == CONTAINER_PODMAN) {
        log_info("Building static binary in podman container (offline)...");
        /*
         * Offline podman build - Omarchy-style:
         *
         *   1. The caller must have run `make cache` (or `build_iso --pull`)
         *      beforehand to save the image as archlinux-build.tar and populate
         *      the host pkg cache at config->pkg_cache_dir.
         *
         *   2. We mount the host's pacman package cache into the container so
         *      `pacman -S` never hits the network.
         *
         *   3. -S --needed (no -y): do NOT resync the remote database.
         *      All required packages (musl, gcc) must already be in the cache.
         *
         *   4. The container image tag is localhost/archlinux-build which was
         *      loaded offline via `podman load -i archlinux-build.tar`.
         *      Fall back to docker.io/archlinux:latest if the local tag is absent
         *      (requires network - acceptable for the initial `make cache` step).
         */
        snprintf(cmd, sizeof(cmd),
                 "sudo podman run --rm "
                 "-v '%s:/src' "
                 "-v '%s:/var/cache/pacman/pkg:ro' "
                 "localhost/archlinux-build:latest "
                 "sh -c 'pacman -S --noconfirm --needed musl gcc && "
                 "cd /src && rm -f luksarchy luksarchy-static && "
                 "musl-gcc -std=c23 -Wall -Wextra -O2 -static src/luksarchy.c -o luksarchy-static'",
                 config->luksarchy_src,
                 config->pkg_cache_dir);
        if (!run_command(cmd)) {
            log_error("Failed to build luksarchy-static in podman");
            return 0;
        }
    } else if (config->use_container && config->container_type == CONTAINER_DISTROBOX) {
        snprintf(cmd, sizeof(cmd),
                 "sudo pacman -S --noconfirm --needed musl && "
                 "cd '%s' && make clean && make static CC=musl-gcc",
                 config->luksarchy_src);
        if (!run_command_in_container(cmd, config)) {
            log_error("Failed to build luksarchy-static in distrobox");
            return 0;
        }
    } else {
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && make clean && make static CC=musl-gcc",
                 config->luksarchy_src);
        if (!run_command(cmd)) {
            log_error("Failed to build luksarchy-static");
            return 0;
        }
    }

    char binary_path[PATH_MAX_LEN];
    snprintf(binary_path, sizeof(binary_path), "%s/luksarchy-static", config->luksarchy_src);

    struct stat st;
    if (stat(binary_path, &st) != 0) {
        log_error("luksarchy-static binary not found at %s", binary_path);
        return 0;
    }

    log_info("Built luksarchy-static successfully");
    return 1;
}

int clean_airootfs(const Build_Config *config) {
    log_info("Cleaning airootfs...");

    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "sudo rm -rf '%s/airootfs/usr'", config->iso_profile);
    if (!run_command(cmd)) {
        log_warn("Failed to clean airootfs/usr");
    }

    snprintf(cmd, sizeof(cmd), "sudo rm -rf '%s/airootfs/root/luksarchy'", config->iso_profile);
    if (!run_command(cmd)) {
        log_warn("Failed to clean airootfs/root/luksarchy");
    }

    /* Also clean any previous copy of the local repo inside airootfs */
    snprintf(cmd, sizeof(cmd), "sudo rm -rf '%s/airootfs/repo'", config->iso_profile);
    run_command(cmd);

    return 1;
}

int clean_work_dir(const Build_Config *config) {
    log_info("Cleaning work directory...");

    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "sudo umount -R '%s' 2>/dev/null || true", config->work_dir);
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

/*
 * write_build_pacman_conf() - generate the profile's pacman.conf used by mkarchiso.
 *
 * Critical: Server = file:///repo only works *inside the live system*.
 * During the build, pacman runs on the host and needs an absolute host path.
 * We therefore write Server = file://<absolute-path-to-iso/repo>.
 *
 * The live-system config (airootfs/etc/pacman.conf) keeps file:///repo and
 * receives a copy of the packages under airootfs/repo/.
 */
static int write_build_pacman_conf(const Build_Config *config, const char *abs_repo_dir) {
    char pacman_conf_path[PATH_MAX_LEN];
    snprintf(pacman_conf_path, sizeof(pacman_conf_path),
             "%s/pacman.conf", config->iso_profile);

    log_info("Writing build-time pacman.conf with host path: %s", abs_repo_dir);

    FILE *fp = fopen(pacman_conf_path, "w");
    if (!fp) {
        log_error("Failed to open %s for writing", pacman_conf_path);
        return 0;
    }

    fprintf(fp,
        "#\n"
        "# Build-time pacman.conf for Luksarchy ISO (generated by build_iso)\n"
        "# Server paths point at the host-side local repo so mkarchiso can\n"
        "# install packages offline.  The live system uses a different\n"
        "# config under airootfs/etc/pacman.conf with Server = file:///repo.\n"
        "#\n"
        "[options]\n"
        "HoldPkg     = pacman glibc\n"
        "Architecture = auto\n"
        "CheckSpace\n"
        "SigLevel    = Required DatabaseOptional\n"
        "LocalFileSigLevel = Optional\n"
        "ParallelDownloads = 5\n"
        "#DownloadUser = alpm\n"
        "\n"
        "# Local offline repositories (highest priority)\n"
        "[core-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server = file://%s\n"
        "\n"
        "[extra-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server = file://%s\n"
        "\n"
        "# Fallback to official mirrors (used only when a package is missing\n"
        "# from the local repo – normally never hit during an offline build)\n"
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
 * write_live_pacman_conf() - ensure airootfs/etc/pacman.conf points at /repo
 * so the finished ISO can install packages completely offline.
 */
static int write_live_pacman_conf(const Build_Config *config) {
    char conf_dir[PATH_MAX_LEN];
    char conf_path[PATH_MAX_LEN];
    char cmd[CMD_MAX_LEN];

    snprintf(conf_dir, sizeof(conf_dir), "%s/airootfs/etc", config->iso_profile);
    snprintf(conf_path, sizeof(conf_path), "%s/pacman.conf", conf_dir);

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", conf_dir);
    if (!run_command(cmd)) {
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
        "HoldPkg     = pacman glibc\n"
        "Architecture = auto\n"
        "CheckSpace\n"
        "SigLevel    = Required DatabaseOptional\n"
        "LocalFileSigLevel = Optional\n"
        "ParallelDownloads = 5\n"
        "\n"
        "[core-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server = file:///repo\n"
        "\n"
        "[extra-local]\n"
        "SigLevel = Optional TrustAll\n"
        "Server = file:///repo\n"
        "\n"
        "# Official mirrors kept as fallback (disabled for pure offline use)\n"
        "#[core]\n"
        "#Include = /etc/pacman.d/mirrorlist\n"
        "#[extra]\n"
        "#Include = /etc/pacman.d/mirrorlist\n");

    fclose(fp);
    return 1;
}

/*
 * build_local_repo() - create the package database inside the ISO profile.
 *
 * After this function:
 *   - iso/repo/ contains all .pkg.tar.zst + core-local.db + extra-local.db
 *   - iso/pacman.conf is rewritten with the absolute host path (for mkarchiso)
 *   - airootfs/etc/pacman.conf is written with file:///repo (for the live system)
 *
 * The actual copy of packages into airootfs/repo happens in prepare_airootfs().
 */
int build_local_repo(const Build_Config *config, const char *package_list) {
    log_info("Building local package repository in iso/repo/ ...");

    char repo_dir[PATH_MAX_LEN];
    snprintf(repo_dir, sizeof(repo_dir), "%s/repo", config->iso_profile);

    char cmd[CMD_MAX_LEN];
    char abs_repo_dir[PATH_MAX_LEN];

    /* mkdir first (as current user) so realpath can resolve it. */
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", repo_dir);
    if (!run_command(cmd)) {
        log_error("Failed to create repo directory: %s", repo_dir);
        return 0;
    }
    if (realpath(repo_dir, abs_repo_dir) == NULL) {
        snprintf(abs_repo_dir, sizeof(abs_repo_dir), "%s", repo_dir);
    }

    /* Remove stale database files from previous runs so repo-add starts clean. */
    snprintf(cmd, sizeof(cmd),
             "rm -f '%s'/core-local.db* '%s'/core-local.files* "
             "      '%s'/extra-local.db* '%s'/extra-local.files*",
             abs_repo_dir, abs_repo_dir, abs_repo_dir, abs_repo_dir);
    run_command(cmd);

    log_info("Downloading packages into %s ...", abs_repo_dir);
    log_info("(already-cached packages are skipped)");

    char pacman_conf_path[PATH_MAX_LEN];
    char pacman_db_path[PATH_MAX_LEN];
    snprintf(pacman_conf_path, sizeof(pacman_conf_path),
             "/tmp/luksarchy-pacman.conf");
    snprintf(pacman_db_path, sizeof(pacman_db_path),
             "/tmp/luksarchy-repodb");

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", pacman_db_path);
    run_command(cmd);

    {
        FILE *fp = fopen(pacman_conf_path, "w");
        if (!fp) {
            log_error("Failed to write temporary pacman.conf");
            return 0;
        }
        fprintf(fp,
            "[options]\n"
            "Architecture = auto\n"
            "SigLevel = Never\n"
            "ParallelDownloads = 5\n"
            "CacheDir = %s\n"
            "DBPath = %s\n"
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
        log_error("Failed to download packages into repo dir");
        log_error("This step requires internet access (run once, then offline).");
        return 0;
    }

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
        log_error("core-local.db missing after repo-add - something went wrong");
        return 0;
    }

    /* Generate the two pacman.conf files with the correct Server paths. */
    if (!write_build_pacman_conf(config, abs_repo_dir)) {
        return 0;
    }
    if (!write_live_pacman_conf(config)) {
        return 0;
    }

    log_info("Local repo built successfully at %s", repo_dir);
    log_info("  core-local.db  ok");
    log_info("  extra-local.db ok");
    return 1;
}

int prepare_airootfs(const Build_Config *config) {
    log_info("Preparing airootfs...");

    char cmd[CMD_MAX_LEN];
    char src_path[PATH_MAX_LEN];
    char dest_path[PATH_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/airootfs/usr/local/bin'", config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to create airootfs/usr/local/bin");
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/airootfs/usr/share'", config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to create airootfs/usr/share");
        return 0;
    }

    /* Install binary as /usr/local/bin/luksarchy */
    snprintf(src_path, sizeof(src_path), "%s/luksarchy-static", config->luksarchy_src);
    snprintf(dest_path, sizeof(dest_path), "%s/airootfs/usr/local/bin/luksarchy", config->iso_profile);
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src_path, dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to copy luksarchy binary");
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "chmod 755 '%s'", dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to set permissions on luksarchy binary");
        return 0;
    }

    /* Assets -> /usr/share/luksarchy (hypr, waybar, alacritty, etc.) */
    snprintf(src_path, sizeof(src_path), "%s/assets", config->luksarchy_src);
    snprintf(dest_path, sizeof(dest_path), "%s/airootfs/usr/share/luksarchy", config->iso_profile);
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

    snprintf(dest_path, sizeof(dest_path), "%s/airootfs/usr/share/wallpapers", config->iso_profile);
    snprintf(cmd, sizeof(cmd), "cp -r '%s/wallpapers' '%s'", src_path, dest_path);
    if (!run_command(cmd)) {
        log_warn("Failed to copy wallpapers");
    }

    /*
     * Copy the local package repository into airootfs/repo so that the
     * finished ISO contains /repo and the live pacman.conf (file:///repo)
     * can find the databases and packages offline.
     */
    log_info("Embedding local repo into airootfs/repo ...");
    snprintf(src_path, sizeof(src_path), "%s/repo", config->iso_profile);
    snprintf(dest_path, sizeof(dest_path), "%s/airootfs/repo", config->iso_profile);
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && cp -a '%s'/* '%s/'",
             dest_path, src_path, dest_path);
    if (!run_command(cmd)) {
        log_error("Failed to copy local repo into airootfs");
        return 0;
    }

    log_info("Setting proper ownership for airootfs...");
    snprintf(cmd, sizeof(cmd), "sudo chown -R root:root '%s/airootfs/usr' '%s/airootfs/repo'",
             config->iso_profile, config->iso_profile);
    if (!run_command(cmd)) {
        log_error("Failed to set ownership on airootfs");
        return 0;
    }

    return 1;
}

int run_mkarchiso(const Build_Config *config) {
    log_info("Building ISO with mkarchiso...");

    if (!create_directory(config->out_dir, 0755)) {
        return 0;
    }

    if (!create_directory(config->work_dir, 0755)) {
        return 0;
    }

    char cmd[CMD_MAX_LEN];
    snprintf(cmd, sizeof(cmd), "sudo mkarchiso -v -w '%s' -o '%s' '%s'",
             config->work_dir, config->out_dir, config->iso_profile);

    if (!run_command(cmd)) {
        log_error("mkarchiso failed");
        return 0;
    }

    return 1;
}

int run_mkarchiso_in_container(const Build_Config *config) {
    log_info("Building ISO with mkarchiso in container...");

    if (!create_directory(config->out_dir, 0755)) {
        return 0;
    }

    if (!create_directory(config->work_dir, 0755)) {
        return 0;
    }

    char cmd[CMD_MAX_LEN];

    log_info("Setting up container policy...");
    run_command("sudo mkdir -p /etc/containers");
    run_command("echo '{\"default\":[{\"type\":\"insecureAcceptAnything\"}]}' | sudo tee /etc/containers/policy.json > /dev/null");

    /*
     * Offline mkarchiso in podman - mirrors the Omarchy build approach:
     *
     *   - localhost/archlinux-build:latest is the pre-pulled image loaded via
     *     `podman load -i archlinux-build.tar` (see `make cache`).
     *
     *   - The host pkg cache is mounted read-only so `pacman -S archiso` inside
     *     the container uses cached packages, not the network.
     *
     *   - -S --needed (no -y) avoids a remote database resync.
     *
     *   - The /repo directory inside the profile is the local package repository
     *     built by the `make repo` step (repo-add over all .pkg.tar.zst files).
     *     mkarchiso copies it into the squashfs so the installer can run
     *     pacstrap -C /etc/pacman-offline.conf -G without touching the internet.
     */
    snprintf(cmd, sizeof(cmd),
             "sudo podman run --rm --privileged "
             "-v '%s:/profile' "
             "-v '%s:/out' "
             "-v '%s:/work' "
             "-v '%s:/var/cache/pacman/pkg:ro' "
             "localhost/archlinux-build:latest "
             "sh -c 'pacman -S --noconfirm --needed archiso && "
             "mkarchiso -v -w /work -o /out /profile'",
             config->iso_profile,
             config->out_dir,
             config->work_dir,
             config->pkg_cache_dir);

    if (!run_command(cmd)) {
        log_error("mkarchiso in container failed");
        return 0;
    }

    return 1;
}

const char *find_latest_iso(const char *out_dir) {
    static char iso_path[PATH_MAX_LEN];
    char cmd[CMD_MAX_LEN];

    snprintf(cmd, sizeof(cmd), "ls -t '%s'/*.iso 2>/dev/null | head -n1", out_dir);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return NULL;
    }

    if (fgets(iso_path, sizeof(iso_path), fp) != NULL) {
        iso_path[strcspn(iso_path, "\n")] = 0;
        pclose(fp);
        return iso_path;
    }

    pclose(fp);
    return NULL;
}

/*
 * HYPRLAND_PACKAGES - kept in sync with luksarchy.c.
 *
 * build_iso uses this list to populate iso/repo/ via build_local_repo() so
 * that every package the installer needs is present in the squashfs-embedded
 * local repository before mkarchiso runs.
 *
 * If you add or remove a package in luksarchy.c's HYPRLAND_PACKAGES, update
 * this string too.  A mismatch means pacstrap will fail at install time with
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
    "vlc evince eog "
    "fastfetch rofi-wayland ripgrep fd "
    "ttf-iosevka-nerd ttf-jetbrains-mono-nerd "
    "polkit-gnome qt5-wayland qt6-wayland "
    "grim slurp wl-clipboard brightnessctl playerctl "
    "noto-fonts noto-fonts-emoji ly";

/*
 * pull_container_image() - online pre-flight step (run once, then offline forever).
 *
 * Pulls docker.io/archlinux:latest, saves it to <luksarchy_src>/archlinux-build.tar,
 * and re-tags it as localhost/archlinux-build:latest so subsequent offline builds
 * never touch the network.
 *
 * Invoke with:  build_iso --pull
 */
static int pull_container_image(const Build_Config *config) {
    char cmd[CMD_MAX_LEN];
    char tar_path[PATH_MAX_LEN];
    snprintf(tar_path, sizeof(tar_path), "%s/archlinux-build.tar", config->luksarchy_src);

    log_info("Pulling docker.io/archlinux:latest (online - run once)...");
    if (!run_command("sudo podman pull docker.io/archlinux:latest")) {
        log_error("Failed to pull archlinux image");
        return 0;
    }

    log_info("Saving image to %s ...", tar_path);
    snprintf(cmd, sizeof(cmd),
             "sudo podman save docker.io/archlinux:latest -o '%s'", tar_path);
    if (!run_command(cmd)) {
        log_error("Failed to save archlinux image");
        return 0;
    }

    log_info("Tagging as localhost/archlinux-build:latest ...");
    if (!run_command("sudo podman tag docker.io/archlinux:latest localhost/archlinux-build:latest")) {
        log_error("Failed to tag archlinux image");
        return 0;
    }

    log_info("Container image ready for offline use.");

    /* Also download all ISO packages into iso/repo/ and build the databases.
     * This means --pull does everything online in a single step:
     *   build_iso --pull        (internet required, run once)
     *   build_iso --container   (fully offline from here) */
    log_info("Populating iso/repo/ with all packages ...");
    if (!build_local_repo(config, BUILD_ISO_PACKAGES)) {
        log_warn("Failed to build local repo - run `build_iso --pull` again with internet");
        /* Non-fatal here: the user can re-run --pull */
    }

    log_info("===================================");
    log_info("--pull complete.  You are now ready for offline builds:");
    log_info("  build_iso --container podman");
    log_info("===================================");
    return 1;
}

/*
 * load_container_image() - restore the saved image into podman's local store.
 * Called automatically at the start of an offline build if the local tag is missing.
 */
static int ensure_container_image(const Build_Config *config) {
    /* Check if localhost/archlinux-build:latest is already loaded */
    if (system("sudo podman image exists localhost/archlinux-build:latest") == 0) {
        return 1;
    }

    char tar_path[PATH_MAX_LEN];
    snprintf(tar_path, sizeof(tar_path), "%s/archlinux-build.tar", config->luksarchy_src);

    struct stat st;
    if (stat(tar_path, &st) != 0) {
        log_error("Container image not found. Run: build_iso --pull");
        log_error("(requires internet - do this once, then builds work offline)");
        return 0;
    }

    log_info("Loading container image from %s ...", tar_path);
    char cmd[CMD_MAX_LEN];
    snprintf(cmd, sizeof(cmd), "sudo podman load -i '%s'", tar_path);
    if (!run_command(cmd)) {
        log_error("Failed to load container image from %s", tar_path);
        return 0;
    }

    if (!run_command("sudo podman tag docker.io/archlinux:latest localhost/archlinux-build:latest")) {
        log_error("Failed to re-tag loaded image");
        return 0;
    }

    return 1;
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nOptions:\n");
    printf("  --iso-profile PATH    Path to ISO profile directory (default: ./iso)\n");
    printf("  --out-dir PATH        Output directory for ISO (default: ./out)\n");
    printf("  --container [TYPE]    Build using container (podman or distrobox)\n");
    printf("  --distrobox NAME      Distrobox container name (default: arch)\n");
    printf("  --pkg-cache PATH      Host pacman pkg cache for offline builds\n");
    printf("                        (default: /var/cache/pacman/pkg)\n");
    printf("  --pull                Pull + save container image for offline use\n");
    printf("                        (requires internet - run once before going offline)\n");
    printf("  -h, --help            Show this help message\n");
}

static int parse_args(int argc, char *argv[], Build_Config *config) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iso-profile") == 0 && i + 1 < argc) {
            snprintf(config->iso_profile, sizeof(config->iso_profile), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            snprintf(config->out_dir, sizeof(config->out_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--container") == 0) {
            config->use_container = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i++;
                if (strcmp(argv[i], "podman") == 0) {
                    config->container_type = CONTAINER_PODMAN;
                } else if (strcmp(argv[i], "distrobox") == 0) {
                    config->container_type = CONTAINER_DISTROBOX;
                } else {
                    log_error("Unknown container type: %s", argv[i]);
                    return 0;
                }
            } else {
                config->container_type = CONTAINER_PODMAN;
            }
        } else if (strcmp(argv[i], "--distrobox") == 0 && i + 1 < argc) {
            snprintf(config->distrobox_name, sizeof(config->distrobox_name), "%s", argv[++i]);
            config->use_container = true;
            config->container_type = CONTAINER_DISTROBOX;
        } else if (strcmp(argv[i], "--pkg-cache") == 0 && i + 1 < argc) {
            /* Path to a pre-populated pacman package cache on the host.
             * Mounted into the container read-only so pacman never hits
             * the network during offline builds. */
            snprintf(config->pkg_cache_dir, sizeof(config->pkg_cache_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--pull") == 0) {
            /* Online pre-flight: pull + save container image, then exit.
             * Run this once while connected; afterwards all builds are offline. */
            config->do_pull = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
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

int main(int argc, char *argv[]) {
    logger_init("/tmp/build_iso.log");

    log_info("Luksarchy ISO Builder starting...");

    Build_Config config = {
        .work_dir      = "/tmp/luksarchy_iso_work",
        .distrobox_name = "arch",
        .container_type = CONTAINER_NONE,
        .use_container  = false,
        .do_pull        = false,
        /* Default pkg cache: the host's own pacman cache.
         * Override with --pkg-cache if your cache lives elsewhere. */
        .pkg_cache_dir  = "/var/cache/pacman/pkg",
    };

    if (getcwd(config.luksarchy_src, sizeof(config.luksarchy_src)) == NULL) {
        log_error("Failed to get current directory");
        return 1;
    }

    snprintf(config.iso_profile, sizeof(config.iso_profile), "%s/iso", config.luksarchy_src);
    snprintf(config.out_dir, sizeof(config.out_dir), "%s/out", config.luksarchy_src);

    if (!parse_args(argc, argv, &config)) {
        logger_close();
        return 1;
    }

    /* --pull: online pre-flight - fetch and save the container image, then exit.
     * Run once while connected to the internet; afterwards all builds are offline. */
    if (config.do_pull) {
        log_info("--pull: fetching container image for offline use...");
        int ok = pull_container_image(&config);
        logger_close();
        return ok ? 0 : 1;
    }

    log_info("Luksarchy source: %s", config.luksarchy_src);
    log_info("ISO profile: %s", config.iso_profile);
    log_info("Work directory: %s", config.work_dir);
    log_info("Output directory: %s", config.out_dir);
    log_info("Package cache: %s", config.pkg_cache_dir);

    if (config.use_container) {
        log_info("Container mode: %s",
                 config.container_type == CONTAINER_PODMAN ? "podman" : "distrobox");
        if (config.container_type == CONTAINER_DISTROBOX) {
            log_info("Distrobox name: %s", config.distrobox_name);
        }

        /* Ensure the offline container image is loaded before we try to use it. */
        if (config.container_type == CONTAINER_PODMAN) {
            if (!ensure_container_image(&config)) {
                log_error("Cannot proceed without container image.");
                log_error("Run:  build_iso --pull   (requires internet, one time only)");
                logger_close();
                return 1;
            }
        }
    }

    if (!build_luksarchy_static(&config)) {
        log_error("Build failed");
        logger_close();
        return 1;
    }

    /* Build (or refresh) the local package repo at iso/repo/ so mkarchiso
     * packs it into the squashfs and pacstrap can install offline. */
    if (!build_local_repo(&config, BUILD_ISO_PACKAGES)) {
        log_error("Failed to build local package repository");
        log_error("Ensure all packages were downloaded (run build_iso --pull first)");
        logger_close();
        return 1;
    }

    if (!clean_airootfs(&config)) {
        log_error("Failed to clean airootfs");
        logger_close();
        return 1;
    }

    if (!clean_work_dir(&config)) {
        log_error("Failed to clean work directory");
        logger_close();
        return 1;
    }

    if (!prepare_airootfs(&config)) {
        log_error("Failed to prepare airootfs");
        logger_close();
        return 1;
    }

    int build_result;
    if (config.use_container && config.container_type == CONTAINER_PODMAN) {
        build_result = run_mkarchiso_in_container(&config);
    } else {
        build_result = run_mkarchiso(&config);
    }

    if (!build_result) {
        log_error("Failed to build ISO");
        logger_close();
        return 1;
    }

    if (!clean_work_dir(&config)) {
        log_warn("Failed to clean work directory after build");
    }

    log_info("Syncing filesystem...");
    sync();
    sleep(2);

    const char *iso_path = find_latest_iso(config.out_dir);
    if (iso_path) {
        log_info("===================================");
        log_info("ISO created successfully!");
        log_info("Location: %s", iso_path);
        log_info("Test with: make test");
        log_info("===================================");
    } else {
        log_error("ISO was built but could not be found in output directory");
        logger_close();
        return 1;
    }

    logger_close();
    return 0;
}
