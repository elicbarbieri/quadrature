/**
 * quadrature-cli setup-rt - Realtime audio configuration checker and fixer
 *
 * Audits the system for realtime audio readiness and optionally applies fixes.
 *
 * Usage:
 *   quadrature-cli setup-rt              Check mode (default)
 *   quadrature-cli setup-rt --apply      Apply fixes interactively
 *   quadrature-cli setup-rt -v           Verbose output
 */

#include <errno.h>
#include <getopt.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <glib.h>

#include "internal.h"

// =============================================================================
// Types
// =============================================================================

typedef enum {
    RT_PASS,
    RT_WARN,
    RT_FAIL,
    RT_SKIP,
} rt_status_t;

typedef struct {
    rt_status_t  status;
    const char*  name;
    char*        detail;       /* heap-allocated, freed by caller */
    const char*  fix_cmd;      /* static string or NULL */
    bool         needs_root;
    bool         can_auto_apply;
} rt_check_result_t;

typedef struct {
    bool apply;
    bool verbose;
} rt_options_t;

// =============================================================================
// Helpers
// =============================================================================

static const char* status_label(rt_status_t s) {
    switch (s) {
        case RT_PASS: return "\033[32mPASS\033[0m";
        case RT_WARN: return "\033[33mWARN\033[0m";
        case RT_FAIL: return "\033[31mFAIL\033[0m";
        case RT_SKIP: return "\033[90mSKIP\033[0m";
    }
    return "????";
}

/** Run a command and capture stdout. Returns heap-allocated string or NULL. */
static char* run_cmd(const char* cmd) {
    char* out = NULL;
    char* err = NULL;
    int exit_status = 0;

    gboolean ok = g_spawn_command_line_sync(cmd, &out, &err, &exit_status, NULL);
    g_free(err);

    if (!ok || !WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
        g_free(out);
        return NULL;
    }

    /* Trim trailing newline */
    if (out) {
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = '\0';
    }
    return out;
}

/** Check if a command exits successfully. */
static bool cmd_ok(const char* cmd) {
    char* out = run_cmd(cmd);
    bool ok = (out != NULL);
    g_free(out);
    return ok;
}

static void print_check(const rt_check_result_t* r) {
    printf("  [%s] %s", status_label(r->status), r->name);
    if (r->detail)
        printf(" — %s", r->detail);
    printf("\n");
    if (r->fix_cmd && (r->status == RT_FAIL || r->status == RT_WARN))
        printf("         → %s\n", r->fix_cmd);
}

static void free_check(rt_check_result_t* r) {
    g_free(r->detail);
    r->detail = NULL;
}

// =============================================================================
// Checks
// =============================================================================

static rt_check_result_t check_pipewire_running(void) {
    rt_check_result_t r = { .name = "PipeWire running" };

    char* version = run_cmd("pw-cli --version");
    if (!version) {
        r.status = RT_FAIL;
        r.detail = g_strdup("pw-cli not found or PipeWire not running");
        r.fix_cmd = "Install and start PipeWire";
        return r;
    }

    /* pw-cli --version outputs multiple lines; first line has the version */
    char* nl = strchr(version, '\n');
    if (nl) *nl = '\0';

    r.status = RT_PASS;
    r.detail = g_strdup(version);
    g_free(version);
    return r;
}

static rt_check_result_t check_force_quantum(void) {
    rt_check_result_t r = { .name = "No global force-quantum override" };

    char* out = run_cmd("pw-metadata -n settings 0");
    if (!out) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not query PipeWire metadata");
        return r;
    }

    if (strstr(out, "clock.force-quantum")) {
        r.status = RT_WARN;
        r.detail = g_strdup("force-quantum is set globally — may override per-stream quantum requests");
        r.fix_cmd = "Remove clock.force-quantum from PipeWire config if unintended";
    } else {
        r.status = RT_PASS;
        r.detail = g_strdup("Not set (per-stream quantum requests will be respected)");
    }

    g_free(out);
    return r;
}

static rt_check_result_t check_suspend_on_idle(void) {
    rt_check_result_t r = {
        .name = "suspend-on-idle disabled",
        .can_auto_apply = true,
    };

    char* out = run_cmd("pw-cli ls Module");
    if (!out) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not list PipeWire modules");
        return r;
    }

    if (strstr(out, "suspend-on-idle")) {
        r.status = RT_WARN;
        r.detail = g_strdup("Module loaded — may cause latency spikes on stream resume");
        r.fix_cmd = "Disable in PipeWire drop-in config";
        r.can_auto_apply = true;
    } else {
        r.status = RT_PASS;
        r.detail = g_strdup("Not loaded");
    }

    g_free(out);
    return r;
}

static rt_check_result_t check_rtkit(void) {
    rt_check_result_t r = {
        .name = "rtkit-daemon",
        .needs_root = true,
    };

    if (cmd_ok("systemctl is-active --quiet rtkit-daemon")) {
        r.status = RT_PASS;
        r.detail = g_strdup("Active");
    } else {
        r.status = RT_FAIL;
        r.detail = g_strdup("Not running — PipeWire cannot acquire RT scheduling");
        r.fix_cmd = "sudo systemctl enable --now rtkit-daemon";
    }

    return r;
}

static rt_check_result_t check_audio_group(void) {
    rt_check_result_t r = {
        .name = "User in audio group",
        .needs_root = true,
    };

    char* groups = run_cmd("id -nG");
    if (!groups) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not determine group membership");
        return r;
    }

    /* Check for "audio" as a whole word */
    bool found = false;
    char* token = strtok(groups, " ");
    while (token) {
        if (g_strcmp0(token, "audio") == 0) { found = true; break; }
        token = strtok(NULL, " ");
    }

    if (found) {
        r.status = RT_PASS;
        r.detail = g_strdup("Member of audio group");
    } else {
        const char* user = g_get_user_name();
        r.status = RT_FAIL;
        r.detail = g_strdup("Not in audio group — RT priority limits may not apply");
        r.fix_cmd = "sudo usermod -aG audio <user> (then log out and back in)";
        (void)user;
    }

    g_free(groups);
    return r;
}

static rt_check_result_t check_rtprio_limits(void) {
    rt_check_result_t r = {
        .name = "RT priority limits (rtprio/memlock)",
        .needs_root = true,
    };

    /* Check current effective rtprio limit */
    struct rlimit {
        unsigned long rlim_cur;
        unsigned long rlim_max;
    };

    /* Use ulimit from a subshell to check effective limits */
    char* rtprio = run_cmd("sh -c 'ulimit -r'");
    char* memlock = run_cmd("sh -c 'ulimit -l'");

    if (!rtprio) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not query limits");
        g_free(memlock);
        return r;
    }

    long rt_val = strtol(rtprio, NULL, 10);
    bool memlock_ok = memlock && (g_strcmp0(memlock, "unlimited") == 0 || strtol(memlock, NULL, 10) > 1048576);

    if (rt_val >= 80 && memlock_ok) {
        r.status = RT_PASS;
        r.detail = g_strdup_printf("rtprio=%s memlock=%s", rtprio, memlock);
    } else {
        r.status = RT_FAIL;
        r.detail = g_strdup_printf("rtprio=%s memlock=%s (need rtprio>=80, memlock>=1GB or unlimited)",
                                   rtprio, memlock ? memlock : "?");
        r.fix_cmd = "Create /etc/security/limits.d/99-quadrature-audio.conf with: @audio - rtprio 95 / @audio - memlock unlimited";
    }

    g_free(rtprio);
    g_free(memlock);
    return r;
}

static rt_check_result_t check_cpu_governor(void) {
    rt_check_result_t r = {
        .name = "CPU frequency governor",
        .needs_root = true,
    };

    char* gov = run_cmd("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!gov) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not read CPU governor (cpufreq not available?)");
        return r;
    }

    if (g_strcmp0(gov, "performance") == 0) {
        r.status = RT_PASS;
        r.detail = g_strdup("performance");
    } else {
        r.status = RT_WARN;
        r.detail = g_strdup_printf("'%s' — frequency scaling may cause latency jitter", gov);
        r.fix_cmd = "sudo cpupower frequency-set -g performance";
    }

    g_free(gov);
    return r;
}

static rt_check_result_t check_threadirqs(void) {
    rt_check_result_t r = { .name = "Kernel threadirqs" };

    char* cmdline = run_cmd("cat /proc/cmdline");
    if (!cmdline) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not read /proc/cmdline");
        return r;
    }

    if (strstr(cmdline, "threadirqs")) {
        r.status = RT_PASS;
        r.detail = g_strdup("Enabled");
    } else {
        r.status = RT_WARN;
        r.detail = g_strdup("Not set — IRQ handlers run in hardirq context");
        r.fix_cmd = "Add 'threadirqs' to kernel boot parameters";
    }

    g_free(cmdline);
    return r;
}

static rt_check_result_t check_cstate(void) {
    rt_check_result_t r = { .name = "CPU C-state depth" };

    char* cmdline = run_cmd("cat /proc/cmdline");
    if (!cmdline) {
        r.status = RT_SKIP;
        r.detail = g_strdup("Could not read /proc/cmdline");
        return r;
    }

    bool has_max_cstate = strstr(cmdline, "processor.max_cstate=") != NULL
                       || strstr(cmdline, "intel_idle.max_cstate=") != NULL;

    if (has_max_cstate) {
        r.status = RT_PASS;
        r.detail = g_strdup("C-state limited");
    } else {
        r.status = RT_WARN;
        r.detail = g_strdup("No C-state limit — deep sleep states can cause >100µs wakeup latency");
        r.fix_cmd = "Add 'processor.max_cstate=1 intel_idle.max_cstate=0' to kernel boot parameters";
    }

    g_free(cmdline);
    return r;
}

// =============================================================================
// Apply Logic
// =============================================================================

/** Write PipeWire drop-in config for RT priority and suspend-on-idle. */
static bool apply_pipewire_dropin(const rt_options_t* opts) {
    const char* config_dir = NULL;
    char* dropin_dir = NULL;
    char* dropin_path = NULL;
    bool ok = false;

    config_dir = g_get_user_config_dir(); /* ~/.config */
    dropin_dir = g_build_filename(config_dir, "pipewire", "pipewire.conf.d", NULL);
    dropin_path = g_build_filename(dropin_dir, "10-quadrature-rt.conf", NULL);

    /* Create directory if needed */
    if (g_mkdir_with_parents(dropin_dir, 0755) != 0) {
        fprintf(stderr, "Failed to create %s: %s\n", dropin_dir, strerror(errno));
        goto out;
    }

    const char* content =
        "# Generated by quadrature-cli setup-rt --apply\n"
        "# Optimizes PipeWire for low-latency audio playback.\n"
        "#\n"
        "# RT priority: ensure PipeWire graph runs at high RT priority.\n"
        "# suspend-on-idle: disable to prevent latency spikes on stream resume.\n"
        "#\n"
        "# Safe to delete — re-run 'quadrature-cli setup-rt --apply' to regenerate.\n"
        "\n"
        "context.properties = {\n"
        "    default.clock.rt-prio = 88\n"
        "    default.clock.rt-time-hard = 200000\n"
        "    default.clock.rt-time-soft = 200000\n"
        "}\n"
        "\n"
        "# Disable suspend-on-idle to avoid stream resume latency\n"
        "context.modules = [\n"
        "    { name = libpipewire-module-rt\n"
        "      args = {\n"
        "          nice.level   = -11\n"
        "          rt.prio      = 88\n"
        "          rt.time.hard = 200000\n"
        "          rt.time.soft = 200000\n"
        "      }\n"
        "    }\n"
        "]\n";

    if (!g_file_set_contents(dropin_path, content, -1, NULL)) {
        fprintf(stderr, "Failed to write %s\n", dropin_path);
        goto out;
    }

    printf("  Wrote %s\n", dropin_path);
    if (opts->verbose)
        printf("  Restart PipeWire to apply: systemctl --user restart pipewire\n");
    ok = true;

out:
    g_free(dropin_dir);
    g_free(dropin_path);
    return ok;
}

static void apply_system_fixes(const rt_check_result_t* checks, int count) {
    bool has_system_fixes = false;

    for (int i = 0; i < count; i++) {
        if (checks[i].needs_root && checks[i].fix_cmd &&
            (checks[i].status == RT_FAIL || checks[i].status == RT_WARN)) {
            has_system_fixes = true;
            break;
        }
    }

    if (!has_system_fixes) return;

    printf("\n  The following fixes require root privileges:\n\n");
    for (int i = 0; i < count; i++) {
        if (checks[i].needs_root && checks[i].fix_cmd &&
            (checks[i].status == RT_FAIL || checks[i].status == RT_WARN)) {
            printf("    %s\n", checks[i].fix_cmd);
        }
    }
    printf("\n  Run these commands manually, or use pkexec/sudo.\n");
}

// =============================================================================
// Entry Point
// =============================================================================

static void print_help(void) {
    printf("Usage: quadrature-cli setup-rt [options]\n\n"
           "Check and configure the system for realtime audio.\n\n"
           "Options:\n"
           "  --apply     Apply fixes (user-level configs written automatically,\n"
           "              system-level fixes printed for manual execution)\n"
           "  -v, --verbose   Verbose output\n"
           "  -h, --help      Show help\n");
}

int cli_setup_rt(int argc, char** argv) {
    rt_options_t opts = { .apply = false, .verbose = false };

    static struct option long_options[] = {
        {"apply",   no_argument, 0, 'a'},
        {"verbose", no_argument, 0, 'v'},
        {"help",    no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "avh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a': opts.apply = true;    break;
            case 'v': opts.verbose = true;  break;
            case 'h': print_help();         return 0;
            default:  print_help();         return 1;
        }
    }

    printf("Quadrature RT Audio Configuration Check\n");
    printf("========================================\n\n");

    /* Run all checks */
    rt_check_result_t checks[] = {
        check_pipewire_running(),
        check_force_quantum(),
        check_suspend_on_idle(),
        check_rtkit(),
        check_audio_group(),
        check_rtprio_limits(),
        check_cpu_governor(),
        check_threadirqs(),
        check_cstate(),
    };
    int check_count = (int)(sizeof(checks) / sizeof(checks[0]));

    /* Print results */
    printf("PipeWire:\n");
    for (int i = 0; i < 3; i++) print_check(&checks[i]);

    printf("\nSystem:\n");
    for (int i = 3; i < 7; i++) print_check(&checks[i]);

    printf("\nKernel:\n");
    for (int i = 7; i < check_count; i++) print_check(&checks[i]);

    /* Count issues */
    int fails = 0, warns = 0;
    for (int i = 0; i < check_count; i++) {
        if (checks[i].status == RT_FAIL) fails++;
        if (checks[i].status == RT_WARN) warns++;
    }

    printf("\n");
    if (fails == 0 && warns == 0) {
        printf("All checks passed.\n");
    } else {
        printf("%d issue(s), %d warning(s).\n", fails, warns);
    }

    /* Apply if requested */
    if (opts.apply) {
        printf("\nApplying fixes:\n");

        /* User-level: PipeWire drop-in config */
        bool need_pw_config = false;
        for (int i = 1; i < 4; i++) {
            if (checks[i].can_auto_apply &&
                (checks[i].status == RT_FAIL || checks[i].status == RT_WARN)) {
                need_pw_config = true;
                break;
            }
        }

        if (need_pw_config) {
            apply_pipewire_dropin(&opts);
        } else {
            printf("  PipeWire config: no changes needed.\n");
        }

        /* System-level: print commands for manual execution */
        apply_system_fixes(checks, check_count);
    } else if (fails > 0 || warns > 0) {
        printf("Run 'quadrature-cli setup-rt --apply' to fix.\n");
    }

    /* Cleanup */
    for (int i = 0; i < check_count; i++) free_check(&checks[i]);

    return (fails > 0) ? 1 : 0;
}
