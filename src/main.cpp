#include "helpers/Asserts.hpp"
#include "ui/UI.hpp"
#include "state/AppState.hpp"

#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <hyprutils/cli/ArgumentParser.hpp>
#include <hyprutils/os/Process.hpp>

#include <print>

using namespace Hyprutils::OS;

// Critical environment variables that must be preserved through fork
struct SEnvVars {
    std::string hyprlandInstanceSignature;
    std::string xdgRuntimeDir;
    std::string waylandDisplay;
};

static SEnvVars captureEnvVars() {
    SEnvVars env;
    const auto HIS = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (HIS)
        env.hyprlandInstanceSignature = HIS;
    const auto XDG = getenv("XDG_RUNTIME_DIR");
    if (XDG)
        env.xdgRuntimeDir = XDG;
    const auto WAYLAND = getenv("WAYLAND_DISPLAY");
    if (WAYLAND)
        env.waylandDisplay = WAYLAND;
    return env;
}

static void restoreEnvVars(const SEnvVars& env) {
    if (!env.hyprlandInstanceSignature.empty())
        setenv("HYPRLAND_INSTANCE_SIGNATURE", env.hyprlandInstanceSignature.c_str(), 1);
    if (!env.xdgRuntimeDir.empty())
        setenv("XDG_RUNTIME_DIR", env.xdgRuntimeDir.c_str(), 1);
    if (!env.waylandDisplay.empty())
        setenv("WAYLAND_DISPLAY", env.waylandDisplay.c_str(), 1);
}

// fork off of the parent process, so we don't get killed
static void forkoff(const SEnvVars& env) {
    pid_t pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    // Restore environment after first fork
    restoreEnvVars(env);

    if (setsid() < 0)
        exit(EXIT_FAILURE);

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    // Restore environment after second fork
    restoreEnvVars(env);

    umask(0);
}

int main(int argc, const char** argv, const char** envp) {
    Hyprutils::CLI::CArgumentParser parser({argv, sc<size_t>(argc)});

    ASSERT(parser.registerBoolOption("dry-run", "", "Do not exit apps, only show UI"));
    ASSERT(parser.registerBoolOption("no-exit", "", "Do not exit hyprland once apps close"));
    ASSERT(parser.registerStringOption("top-label", "t", "Set the text appearing on top (set to \"Shutting down...\" by default)"));
    ASSERT(parser.registerStringOption("post-cmd", "p", "Set a command ran after all apps and Hyprland shut down"));
    ASSERT(parser.registerBoolOption("verbose", "", "Enable more logging"));
    ASSERT(parser.registerBoolOption("no-fork", "", "Do not fork/daemonize (run in foreground)"));
    ASSERT(parser.registerIntOption("vt", "", "Switch to VT N after Hyprland exits (fixes NVIDIA+SDDM black screen)"));
    ASSERT(parser.registerBoolOption("help", "h", "Show the help menu"));

    if (const auto ret = parser.parse(); !ret) {
        g_logger->log(LOG_ERR, "Failed parsing arguments: {}", ret.error());
        return 1;
    }

    if (parser.getBool("help").value_or(false)) {
        std::println("{}", parser.getDescription(std::format("hyprshutdown v{}", HYPRSHUTDOWN_VERSION)));
        return 0;
    }

    if (parser.getBool("verbose").value_or(false))
        g_logger->setLogLevel(LOG_TRACE);

    if (parser.getBool("dry-run").value_or(false))
        State::state()->m_dryRun = true;

    // Capture critical environment variables BEFORE forking
    // These are needed for Wayland connection and Hyprland IPC
    const auto envVars = captureEnvVars();

    const auto HIS = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!HIS || HIS[0] == '\0') {
        g_logger->log(LOG_ERR, "Cannot run under a non-hyprland environment");
        return 1;
    }

    // By default, hyprshutdown forks to avoid being killed when the parent terminal closes.
    // The --no-fork option runs in the foreground, useful for debugging or scripting.
    if (!parser.getBool("no-fork").value_or(false))
        forkoff(envVars);
    else {
        g_logger->log(LOG_DEBUG, "Skipping fork due to --no-fork option");
        signal(SIGHUP, SIG_IGN); // Still ignore SIGHUP to survive terminal disconnect
    }

    if (!State::state()->init()) {
        g_logger->log(LOG_ERR, "Failed to init state");
        return 1;
    }

    g_ui                  = makeUnique<CUI>();
    g_ui->m_noExit        = parser.getBool("no-exit").value_or(false) || State::state()->m_dryRun;
    g_ui->m_shutdownLabel = parser.getString("top-label").value_or("Shutting down...");
    g_ui->m_postExitCmd   = parser.getString("post-cmd");

    // Capture VT switch option before running UI
    auto vtSwitch = parser.getInt("vt");

    g_ui->run();

    // VT switch for NVIDIA+SDDM: after Hyprland exits, the display may not
    // automatically switch back to the greeter's VT, causing a black screen.
    // This explicitly switches to the specified VT to fix it.
    if (vtSwitch && *vtSwitch > 0 && !State::state()->m_dryRun) {
        g_logger->log(LOG_DEBUG, "Switching to VT{}", *vtSwitch);
        std::string cmd = std::format("sudo -n chvt {}", *vtSwitch);
        CProcess proc("/bin/sh", {"-c", cmd});
        proc.runAsync();
    }

    return 0;
}
