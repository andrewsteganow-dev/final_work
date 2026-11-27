#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <net/if.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <memory>

bool path_exists(const std::string& p) {
    struct stat st {};
    return stat(p.c_str(), &st) == 0;
}

void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
}

void rm_rf(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
                rm_rf(path + "/" + ent->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

std::string get_socket_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (false && xdg && path_exists(xdg)) {
        return std::string(xdg) + "/minidocker.sock";
    }
    return "/tmp/minidocker_" + std::to_string(getuid()) + ".sock";
}

std::string get_base_dir() {
    const char* home = std::getenv("HOME");
    std::string base = home ? (std::string(home) + "/.minidocker")
                            : ("/tmp/minidocker_data_" + std::to_string(getuid()));
    mkdir_p(base + "/containers");
    return base;
}

struct ContainerInfo {
    std::string id;
    pid_t pid = -1;
    std::string status;
    std::string command;
    std::string log_path;
    std::string state_dir;
    int pty_master = -1;
    std::mutex client_mtx;
    int attached_fd = -1;
};

std::mutex g_containers_mtx;
std::map<std::string, std::shared_ptr<ContainerInfo>> g_containers;
std::atomic<bool> g_daemon_running{true};

bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

std::string read_line(int fd) {
    std::string line;
    char c;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        line.push_back(c);
    }
    return line;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    if (!s.empty() && s.back() == delim) elems.push_back("");
    return elems;
}

struct SpawnConfig {
    std::string id;
    std::string rootfs;
    std::string state_dir;
    std::vector<std::string> cmd;
    pid_t join_net_pid = -1;
    std::string pty_slave_path;
    int sync_fd = -1;
};

bool setup_userns_mappings(pid_t child_pid, uid_t host_uid, gid_t host_gid) {
    std::string proc_dir = "/proc/" + std::to_string(child_pid);
    {
        std::ofstream f(proc_dir + "/setgroups");
        if (!f) return false;
        f << "deny";
    }
    {
        std::ofstream f(proc_dir + "/uid_map");
        if (!f) return false;
        f << "0 " << host_uid << " 1\n";
    }
    {
        std::ofstream f(proc_dir + "/gid_map");
        if (!f) return false;
        f << "0 " << host_gid << " 1\n";
    }
    return true;
}

bool bring_up_loopback() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        close(sock);
        return false;
    }
    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
    bool ok = (ioctl(sock, SIOCSIFFLAGS, &ifr) == 0);
    close(sock);
    return ok;
}

bool join_target_network(pid_t target_pid) {
    std::string user_ns = "/proc/" + std::to_string(target_pid) + "/ns/user";
    std::string net_ns  = "/proc/" + std::to_string(target_pid) + "/ns/net";
    int ufd = open(user_ns.c_str(), O_RDONLY | O_CLOEXEC);
    int nfd = open(net_ns.c_str(), O_RDONLY | O_CLOEXEC);
    if (ufd < 0 || nfd < 0) {
        if (ufd >= 0) close(ufd);
        if (nfd >= 0) close(nfd);
        return false;
    }
    if (setns(ufd, CLONE_NEWUSER) < 0 || setns(nfd, CLONE_NEWNET) < 0) {
        close(ufd); close(nfd);
        return false;
    }
    close(ufd);
    close(nfd);
    return true;
}

bool setup_overlay_and_rootfs(const SpawnConfig& cfg) {
    std::string upper  = cfg.state_dir + "/upper";
    std::string work   = cfg.state_dir + "/work";
    std::string merged = cfg.state_dir + "/merged";
    mkdir_p(upper);
    mkdir_p(work);
    mkdir_p(merged);

    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0) return false;

    std::string opts = "lowerdir=" + cfg.rootfs + ",upperdir=" + upper + ",workdir=" + work + ",userxattr";
    if (mount("overlay", merged.c_str(), "overlay", 0, opts.c_str()) < 0) {
        if (mount(cfg.rootfs.c_str(), merged.c_str(), nullptr, MS_BIND | MS_REC, nullptr) < 0) return false;
    }

    std::string dev_dir = merged + "/dev";
    mkdir_p(dev_dir);
    mount("tmpfs", dev_dir.c_str(), "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755");
    mkdir_p(dev_dir + "/pts");

    const char* dev_nodes[] = {"null", "zero", "random", "urandom", "tty"};
    for (const char* node : dev_nodes) {
        std::string host_dev = std::string("/dev/") + node;
        std::string cont_dev = dev_dir + "/" + node;
        int fd = open(cont_dev.c_str(), O_CREAT | O_WRONLY, 0666);
        if (fd >= 0) close(fd);
        mount(host_dev.c_str(), cont_dev.c_str(), nullptr, MS_BIND, nullptr);
    }

    if (true) {
        { std::string cs = merged + cfg.pty_slave_path; int sfd = open(cs.c_str(), O_CREAT | O_WRONLY, 0666); if (sfd >= 0) close(sfd); mount(cfg.pty_slave_path.c_str(), cs.c_str(), nullptr, MS_BIND, nullptr); }
    }

    if (chroot(merged.c_str()) < 0 || chdir("/") < 0) return false;

    mkdir_p("/proc");
    if (mount("proc", "/proc", "proc", 0, nullptr) < 0) return false;
    return true;
}

bool attach_slave_pty(const std::string& slave_path) {
    if (setsid() < 0) return false;
    int sfd = open(slave_path.c_str(), O_RDWR);
    if (sfd < 0) return false;
    if (ioctl(sfd, TIOCSCTTY, 0) < 0) {
        close(sfd);
        return false;
    }
    dup2(sfd, STDIN_FILENO);
    dup2(sfd, STDOUT_FILENO);
    dup2(sfd, STDERR_FILENO);
    if (sfd > STDERR_FILENO) close(sfd);
    return true;
}

int container_child_entry(void* arg) {
    auto* cfg = static_cast<SpawnConfig*>(arg);
    uint64_t ready = 0;
    if (read(cfg->sync_fd, &ready, sizeof(ready)) != sizeof(ready)) return 1;
    close(cfg->sync_fd);

    if (!attach_slave_pty(cfg->pty_slave_path)) return 1;

    if (cfg->join_net_pid > 0) {
        if (!join_target_network(cfg->join_net_pid)) return 1;
    } else {
        if (!bring_up_loopback()) return 1;
    }

    sethostname(cfg->id.c_str(), cfg->id.size());

    if (!setup_overlay_and_rootfs(*cfg)) {
        std::cerr << "[CONTAINER ERR] Rootfs/Overlay failed: " << strerror(errno) << "\n";
        return 1;
    }

    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "xterm", 1);

    std::vector<char*> argv;
    std::vector<std::string> fcmd = cfg->cmd; if (fcmd.size() == 1 && fcmd[0].find(' ') != std::string::npos) { std::istringstream iss(fcmd[0]); fcmd.clear(); std::string t; while (iss >> t) fcmd.push_back(t); } for (auto& s : fcmd) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    execvp(argv.at(0), argv.data());
    std::cerr << "[CONTAINER ERR] execvp failed: " << strerror(errno) << "\n";
    return 127;
}

void pty_logger_loop(std::shared_ptr<ContainerInfo> cinfo) {
    std::ofstream log_file(cinfo->log_path, std::ios::binary | std::ios::app);
    std::array<char, 4096> buf;

    while (true) {
        ssize_t n = read(cinfo->pty_master, buf.data(), buf.size());
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (log_file.is_open()) {
            log_file.write(buf.data(), n);
            log_file.flush();
        }
        std::lock_guard<std::mutex> lock(cinfo->client_mtx);
        if (cinfo->attached_fd >= 0) {
            if (!write_all(cinfo->attached_fd, buf.data(), n)) {
                cinfo->attached_fd = -1;
            }
        }
    }

    int status = 0;
    waitpid(cinfo->pid, &status, 0);
    cinfo->status = "EXITED";
    close(cinfo->pty_master);
    cinfo->pty_master = -1;

    std::lock_guard<std::mutex> lock(cinfo->client_mtx);
    if (cinfo->attached_fd >= 0) {
        shutdown(cinfo->attached_fd, SHUT_RDWR);
        cinfo->attached_fd = -1;
    }
}

pid_t resolve_net_target(const std::string& target) {
    if (target.empty() || target == "none") return -1;
    std::lock_guard<std::mutex> lock(g_containers_mtx);
    auto it = g_containers.find(target);
    if (it != g_containers.end() && it->second->status == "UP") {
        return it->second->pid;
    }
    try {
        pid_t p = std::stoi(target);
        if (p > 0 && kill(p, 0) == 0) return p;
    } catch (...) {}
    return -2;
}

void handle_client(int client_fd) {
    std::string req = read_line(client_fd);
    if (req.empty()) { close(client_fd); return; }

    auto parts = split(req, '|');
    const std::string& action = parts.at(0);

    if (action == "RUN" && parts.size() >= 5) {
        std::string id = parts.at(1);
        std::string rootfs = parts.at(2);
        std::string net_target = parts.at(3);
        auto cmd = split(parts.at(4), '\t');

        if (!path_exists(rootfs)) {
            { std::string _e = "ERR|Rootfs не найден\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            if (g_containers.count(id) && g_containers.at(id)->status == "UP") {
                { std::string _e = "ERR|ID уже занят\n"; write_all(client_fd, _e.c_str(), _e.size()); }
                close(client_fd);
                return;
            }
        }

        pid_t join_pid = resolve_net_target(net_target);
        if (join_pid == -2) {
            { std::string _e = "ERR|Сеть не найдена\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }

        int master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master_fd < 0 || grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
            if (master_fd >= 0) close(master_fd);
            { std::string _e = "ERR|Ошибка PTY\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }
        char slave_name[128] = {0}; if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) { close(master_fd); close(client_fd); return; }

        std::string state_dir = get_base_dir() + "/containers/" + id;
        rm_rf(state_dir);
        mkdir_p(state_dir);

        char resolved_rootfs[PATH_MAX];
        if (!realpath(rootfs.c_str(), resolved_rootfs)) {
            std::strncpy(resolved_rootfs, rootfs.c_str(), PATH_MAX - 1);
        }

        SpawnConfig cfg;
        cfg.id = id;
        cfg.rootfs = resolved_rootfs;
        cfg.state_dir = state_dir;
        cfg.cmd = cmd;
        cfg.join_net_pid = join_pid;
        cfg.pty_slave_path = slave_name;
        cfg.sync_fd = eventfd(0, EFD_CLOEXEC); int exec_pipe[2]; if (pipe2(exec_pipe, O_CLOEXEC) < 0) { close(master_fd); close(cfg.sync_fd); close(client_fd); return; }

        int clone_flags = SIGCHLD | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC;
        if (join_pid <= 0) clone_flags |= (CLONE_NEWUSER | CLONE_NEWNET);

        constexpr size_t STACK_SIZE = 1024 * 1024;
        auto* stack = new char[STACK_SIZE];

        pid_t pid = clone(container_child_entry, stack + STACK_SIZE, clone_flags, &cfg);
        if (pid < 0) {
            close(master_fd); close(cfg.sync_fd); delete[] stack;
            { std::string _e = "ERR|Ошибка clone\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }

        if (join_pid <= 0 && !setup_userns_mappings(pid, getuid(), getgid())) {
            kill(pid, SIGKILL); close(master_fd); close(cfg.sync_fd); delete[] stack;
            { std::string _e = "ERR|Ошибка uid_map\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }

        uint64_t go = 1;
        write(cfg.sync_fd, &go, sizeof(go));
        close(cfg.sync_fd);
        close(exec_pipe[1]); char _ep_dummy; while (read(exec_pipe[0], &_ep_dummy, 1) < 0 && errno == EINTR) {} close(exec_pipe[0]);
        delete[] stack;

        auto cinfo = std::make_shared<ContainerInfo>();
        cinfo->id = id;
        cinfo->pid = pid;
        cinfo->status = "UP";
        cinfo->command = parts.at(4);
        std::replace(cinfo->command.begin(), cinfo->command.end(), '\t', ' ');
        cinfo->log_path = state_dir + "/container.log";
        cinfo->state_dir = state_dir;
        cinfo->pty_master = master_fd;

        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            g_containers[id] = cinfo;
        }
        std::thread(pty_logger_loop, cinfo).detach();

        std::string resp = "OK|" + std::to_string(pid) + "\n";
        write_all(client_fd, resp.c_str(), resp.size());
        close(client_fd);
    }
    else if (action == "LIST") {
        std::ostringstream oss;
        oss << "ID\tPID\tSTATUS\tCOMMAND\n";
        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            for (const auto& kv : g_containers) {
                oss << kv.second->id << "\t" << kv.second->pid << "\t"
                    << kv.second->status << "\t" << kv.second->command << "\n";
            }
        }
        std::string out = oss.str();
        write_all(client_fd, out.c_str(), out.size());
        close(client_fd);
    }
    else if (action == "LOGS" && parts.size() >= 2) {
        std::string id = parts.at(1);
        std::string log_path;
        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            if (g_containers.count(id)) log_path = g_containers.at(id)->log_path;
        }
        if (log_path.empty() || !path_exists(log_path)) {
            { std::string _e = "ERR|Логи не найдены\n"; write_all(client_fd, _e.c_str(), _e.size()); }
        } else {
            std::ifstream ifs(log_path, std::ios::binary);
            std::stringstream ss;
            ss << ifs.rdbuf();
            std::string data = ss.str();
            write_all(client_fd, data.c_str(), data.size());
        }
        close(client_fd);
    }
    else if (action == "KILL" && parts.size() >= 2) {
        std::string target = parts.at(1);
        pid_t target_pid = -1;
        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            if (g_containers.count(target)) {
                target_pid = g_containers.at(target)->pid;
            } else {
                try {
                    pid_t p = std::stoi(target);
                    for (const auto& kv : g_containers) {
                        if (kv.second->pid == p) { target_pid = p; break; }
                    }
                } catch (...) {}
            }
        }
        if (target_pid > 0 && kill(target_pid, SIGKILL) == 0) {
            std::string msg = "OK|Контейнер (PID " + std::to_string(target_pid) + ") остановлен\n";
            write_all(client_fd, msg.c_str(), msg.size());
        } else {
            { std::string _e = "ERR|Контейнер не найден\n"; write_all(client_fd, _e.c_str(), _e.size()); }
        }
        close(client_fd);
    }
    else if (action == "ATTACH" && parts.size() >= 4) {
        std::string id = parts.at(1);
        int rows = std::stoi(parts.at(2));
        int cols = std::stoi(parts.at(3));

        std::shared_ptr<ContainerInfo> cinfo;
        {
            std::lock_guard<std::mutex> lock(g_containers_mtx);
            if (g_containers.count(id)) cinfo = g_containers.at(id);
        }
        if (!cinfo || cinfo->status != "UP" || cinfo->pty_master < 0) {
            { std::string _e = "ERR|Контейнер не запущен\n"; write_all(client_fd, _e.c_str(), _e.size()); }
            close(client_fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cinfo->client_mtx);
            if (cinfo->attached_fd >= 0) {
                { std::string _e = "ERR|Терминал уже занят\n"; write_all(client_fd, _e.c_str(), _e.size()); }
                close(client_fd);
                return;
            }
            cinfo->attached_fd = client_fd;
        }

        struct winsize ws {};
        ws.ws_row = rows;
        ws.ws_col = cols;
        ioctl(cinfo->pty_master, TIOCSWINSZ, &ws);
        write_all(client_fd, "OK\n", 3);

        std::array<char, 1024> buf;
        while (true) {
            ssize_t n = read(client_fd, buf.data(), buf.size());
            if (n <= 0) break;
            if (cinfo->pty_master >= 0) write_all(cinfo->pty_master, buf.data(), n);
        }
        {
            std::lock_guard<std::mutex> lock(cinfo->client_mtx);
            if (cinfo->attached_fd == client_fd) cinfo->attached_fd = -1;
        }
        close(client_fd);
    }
    else {
        { std::string _e = "ERR|Неизвестная команда\n"; write_all(client_fd, _e.c_str(), _e.size()); }
        close(client_fd);
    }
}

void daemon_sig_handler(int) { g_daemon_running = false; }

int run_daemon() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, daemon_sig_handler);
    signal(SIGTERM, daemon_sig_handler);

    std::string sock_path = get_socket_path();
    unlink(sock_path.c_str());

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(server_fd, 32) < 0) {
        perror("bind/listen");
        close(server_fd);
        return 1;
    }

    std::cout << "[DAEMON] Запущен (rootless, UID=" << getuid() << ") на " << sock_path << "\n";

    struct pollfd pfd {server_fd, POLLIN, 0};
    while (g_daemon_running) {
        if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd >= 0) std::thread(handle_client, client_fd).detach();
        }
    }
    std::cout << "\n[DAEMON] Остановка...\n";
    close(server_fd);
    unlink(sock_path.c_str());
    return 0;
}

int connect_to_daemon() {
    std::string sock_path = get_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int cli_attach(const std::string& id) {
    int fd = connect_to_daemon();
    if (fd < 0) { std::cerr << "ERROR: Демон не запущен\n"; return 1; }

    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) { ws.ws_row = 24; ws.ws_col = 80; }

    std::string req = "ATTACH|" + id + "|" + std::to_string(ws.ws_row) + "|" + std::to_string(ws.ws_col) + "\n";
    write_all(fd, req.c_str(), req.size());

    std::string status = read_line(fd);
    if (status != "OK") { std::cerr << "ERROR: " << status << "\n"; close(fd); return 1; }

    std::cout << "[ATTACH] Подключено к " << id << ". Выход: Ctrl + ]\r\n";

    struct termios orig_termios {}, raw {};
    bool is_tty = isatty(STDIN_FILENO);
    if (is_tty) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        raw = orig_termios;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    std::array<struct pollfd, 2> pfds {{ {STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0} }};
    std::array<char, 1024> buf;

    while (true) {
        if (poll(pfds.data(), 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (pfds.at(0).revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf.data(), buf.size());
            if (n <= 0) break;
            bool detach = false;
            for (ssize_t i = 0; i < n; ++i) if (buf.at(i) == 0x1d) detach = true;
            if (detach || !write_all(fd, buf.data(), n)) break;
        }
        if (pfds.at(1).revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fd, buf.data(), buf.size());
            if (n <= 0) break;
            write_all(STDOUT_FILENO, buf.data(), n);
        }
    }

    if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    std::cout << "\n[DETACHED] Отключено от " << id << "\n";
    close(fd);
    return 0;
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv, argv + argc);
    if (args.size() < 2) {
        std::cout << "Использование:\n  ./minidocker daemon\n  ./minidocker run [-d] [--net=<id|pid>] <id> <rootfs> <cmd> [args...]\n  ./minidocker ps | list\n  ./minidocker logs <id>\n  ./minidocker attach <id>\n  ./minidocker kill <pid|id>\n";
        return 1;
    }

    const std::string& subcmd = args.at(1);
    if (subcmd == "daemon") return run_daemon();

    if (subcmd == "run") {
        bool detached = false;
        std::string net_target = "none";
        std::vector<std::string> pos;
        for (size_t i = 2; i < args.size(); ++i) {
            if (pos.empty()) {
                if (args.at(i) == "-d" || args.at(i) == "--detach") { detached = true; continue; }
                if (args.at(i).rfind("--net=", 0) == 0) { net_target = args.at(i).substr(6); continue; }
            }
            pos.push_back(args.at(i));
        }
        if (pos.size() < 3) { std::cerr << "ERROR: Проверьте id, rootfs и command\n"; return 1; }

        std::ostringstream cmd_oss;
        for (size_t i = 2; i < pos.size(); ++i) { if (i > 2) cmd_oss << "\t"; cmd_oss << pos.at(i); }

        int fd = connect_to_daemon();
        if (fd < 0) { std::cerr << "ERROR: Демон не запущен\n"; return 1; }

        std::string req = "RUN|" + pos.at(0) + "|" + pos.at(1) + "|" + net_target + "|" + cmd_oss.str() + "\n";
        write_all(fd, req.c_str(), req.size());
        std::string resp = read_line(fd);
        close(fd);

        auto parts = split(resp, '|');
        if (parts.at(0) != "OK") { std::cerr << "ERROR: " << (parts.size() > 1 ? parts.at(1) : resp) << "\n"; return 1; }
        if (detached) { std::cout << pos.at(0) << " (PID: " << parts.at(1) << ")\n"; return 0; }
        return cli_attach(pos.at(0));
    }

    if (subcmd == "ps" || subcmd == "list") {
        int fd = connect_to_daemon();
        if (fd < 0) { std::cerr << "ERROR: Демон не запущен\n"; return 1; }
        write_all(fd, "LIST\n", 5);
        std::array<char, 1024> buf;
        ssize_t n;
        while ((n = read(fd, buf.data(), buf.size())) > 0) write_all(STDOUT_FILENO, buf.data(), n);
        close(fd);
        return 0;
    }

    if (subcmd == "logs" && args.size() >= 3) {
        int fd = connect_to_daemon();
        if (fd < 0) { std::cerr << "ERROR: Демон не запущен\n"; return 1; }
        std::string req = "LOGS|" + args.at(2) + "\n";
        write_all(fd, req.c_str(), req.size());
        std::array<char, 1024> buf;
        ssize_t n;
        while ((n = read(fd, buf.data(), buf.size())) > 0) write_all(STDOUT_FILENO, buf.data(), n);
        close(fd);
        return 0;
    }

    if (subcmd == "attach" && args.size() >= 3) return cli_attach(args.at(2));

    if (subcmd == "kill" && args.size() >= 3) {
        int fd = connect_to_daemon();
        if (fd < 0) { std::cerr << "ERROR: Демон не запущен\n"; return 1; }
        std::string req = "KILL|" + args.at(2) + "\n";
        write_all(fd, req.c_str(), req.size());
        std::cout << read_line(fd) << "\n";
        close(fd);
        return 0;
    }

    std::cerr << "ERROR: Неверные аргументы\n";
    return 1;
}
