#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>

#ifndef AGY_TERMUX_VERSION
#define AGY_TERMUX_VERSION "1.0.2"
#endif

// Helper to query your fork's latest release version via GitHub API and update in-place
void check_and_perform_update(const char* dir) {
    printf("[agy-termux] Querying latest release from wallentx/antigravity-cli-termux...\n");

    // Formulate a secure curl command to query the GitHub Releases API
    char cmd[512];
    snprintf(cmd, sizeof(cmd), 
        "curl -fsSL -H \"User-Agent: Termux-Agy\" https://api.github.com/repos/wallentx/antigravity-cli-termux/releases/latest | rg -o '\"tag_name\"\\s*:\\s*\"[^\"]*' | cut -d'\"' -f4", 
        NULL);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        printf("[agy-termux] Error: Could not check for updates.\n");
        return;
    }

    char latest_tag[64] = {0};
    if (fgets(latest_tag, sizeof(latest_tag) - 1, fp) != NULL) {
        // Strip trailing newline
        latest_tag[strcspn(latest_tag, "\r\n")] = '\0';
    }
    pclose(fp);

    if (strlen(latest_tag) == 0) {
        printf("[agy-termux] Error: Failed to parse latest release tag from GitHub.\n");
        return;
    }

    // Clean version representations (e.g. "v1.0.2" -> "1.0.2")
    const char* clean_latest = (latest_tag[0] == 'v') ? latest_tag + 1 : latest_tag;
    const char* clean_current = (AGY_TERMUX_VERSION[0] == 'v') ? &AGY_TERMUX_VERSION[1] : AGY_TERMUX_VERSION;

    printf("[agy-termux] Current standalone version: v%s\n", clean_current);
    printf("[agy-termux] Latest available version : v%s\n", clean_latest);

    if (strcmp(clean_latest, clean_current) != 0) {
        printf("\n[agy-termux] A new update (v%s) is available!\n", clean_latest);
        printf("[agy-termux] Would you like to update now? [y/N]: ");
        
        char response = 'n';
        if (scanf(" %c", &response) == 1 && (response == 'y' || response == 'Y')) {
            printf("\n[agy-termux] Downloading and applying standalone update...\n");
            
            // Runs a subshell command to download the new tar.gz, extract it, and overwrite files
            // Uses dir/.. to target the parent directory containing bin/ and lib/
            char update_cmd[1024];
            snprintf(update_cmd, sizeof(update_cmd),
                "cd \"%s/..\" && "
                "curl -fsSLO \"https://github.com/wallentx/antigravity-cli-termux/releases/download/%s/antigravity-termux-standalone.tar.gz\" && "
                "tar -xzf antigravity-termux-standalone.tar.gz && "
                "rm antigravity-termux-standalone.tar.gz",
                dir, latest_tag);

            int status = system(update_cmd);
            if (status == 0) {
                printf("[agy-termux] Update completed successfully! Please restart the CLI.\n");
            } else {
                printf("[agy-termux] Error: Update failed during download or extraction.\n");
            }
        } else {
            printf("[agy-termux] Update cancelled.\n");
        }
    } else {
        printf("[agy-termux] You are already up to date with the latest standalone release.\n");
    }
}

int main(int argc, char** argv) {
    // 1. Detect environment and setup paths
    int is_termux = (access("/data/data/com.termux/files/usr/bin", F_OK) == 0);
    
    // Clear conflicting Android Bionic preloads if in Termux
    if (is_termux) {
        unsetenv("LD_PRELOAD");
    } else {
        // In non-Termux environments (e.g. Ubuntu chroot), we may need the mmap fixer
        // to handle the 39-bit VA limit of the underlying Android kernel.
        char fixer_path[PATH_MAX];
        char exec_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
        if (len != -1) {
            exec_path[len] = '\0';
            char* dir = dirname(exec_path);
            snprintf(fixer_path, sizeof(fixer_path), "%s/../lib/libmmap_va39_fix.so", dir);
            if (access(fixer_path, F_OK) == 0) {
                setenv("LD_PRELOAD", fixer_path, 1);
            }
        }
    }
    unsetenv("LD_LIBRARY_PATH");

    // 2. Set dynamic Go resolver and SSL configurations
    setenv("GODEBUG", "netdns=cgo", 1);
    if (is_termux) {
        setenv("SSL_CERT_FILE", "/data/data/com.termux/files/usr/etc/tls/cert.pem", 1);
    } else if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1);
    }

    // 3. Resolve executable directory
    char exec_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (len == -1) {
        return 1;
    }
    exec_path[len] = '\0';
    char* dir = dirname(exec_path);

    // 4. Intercept 'update' subcommand
    if (argc >= 2 && strcmp(argv[1], "update") == 0) {
        check_and_perform_update(dir);
        return 0;
    }

    // 5. Construct relocatable paths relative to our executable's location
    char lib_path[PATH_MAX * 3];
    char patched_bin[PATH_MAX];
    char* loader = "/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1";
    
    if (access(loader, F_OK) != 0) {
        loader = "/lib/ld-linux-aarch64.so.1";
    }

    if (is_termux) {
        snprintf(lib_path, sizeof(lib_path), "%s/../lib:/data/data/com.termux/files/usr/glibc/lib", dir);
    } else {
        snprintf(lib_path, sizeof(lib_path), "%s/../lib:/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu", dir);
    }
    
    // patched_bin: <exec_dir>/agy.va39
    snprintf(patched_bin, sizeof(patched_bin), "%s/agy.va39", dir);

    // 6. Construct argument array
    char** new_argv = malloc((argc + 6) * sizeof(char*));
    if (!new_argv) {
        return 1;
    }

    int arg_idx = 0;
    new_argv[arg_idx++] = loader;
    
    // Add preload if fixer exists (important for Ubuntu/chroot on Android)
    char fixer_path[PATH_MAX];
    snprintf(fixer_path, sizeof(fixer_path), "%s/../lib/libmmap_va39_fix.so", dir);
    if (access(fixer_path, F_OK) == 0) {
        new_argv[arg_idx++] = "--preload";
        new_argv[arg_idx++] = strdup(fixer_path);
    }

    new_argv[arg_idx++] = "--library-path";
    new_argv[arg_idx++] = lib_path;
    new_argv[arg_idx++] = patched_bin;

    for (int i = 1; i < argc; i++) {
        new_argv[arg_idx++] = argv[i];
    }
    new_argv[arg_idx] = NULL;

    // 7. Execute glibc loader
    execv(loader, new_argv);

    free(new_argv);
    return 1;
}
