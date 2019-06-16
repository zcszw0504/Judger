#include "limits.hpp"
#include <glog/logging.h>
#include <grp.h>
#include <libcgroup.h>
#include <signal.h>
#include <math.h>
#include <sys/resource.h>
#include <unistd.h>
#include <system_error>
#include "cgroup.hpp"
#include "utils.hpp"

using namespace std;

void cgroup_create(const struct runguard_options &opt) {
    cgroup_guard cg(opt.cgroupname);

    // Set up the memory restrictions
    cgroup_ctrl ctrl = cg.add_controller("memory");

    int64_t memory_limit = opt.memory_limit;
    if (memory_limit < 0) memory_limit = RLIM_INFINITY;

    // RAM limit and RAM+swap limit are the same,
    // so no swapping can occur.
    ctrl.add_value("memory.limit_in_bytes", opt.memory_limit);
    ctrl.add_value("memory.memsw.limit_in_bytes", opt.memory_limit);

    // Set up CPU restrictions; we pin the task to a specific set of
    // cpus. We also give it exclusive access to those cores.
    // And set no limits on memory nodes.
    if (!opt.cpuset.empty()) {
        cgroup_ctrl cpuset_ctrl = cg.add_controller("cpuset");

        // To make a cpuset exclusive, some additional setup
        // outside of judger required

        cpuset_ctrl.add_value("cpuset.mems", "0");
        cpuset_ctrl.add_value("cpuset.cpus", opt.cpuset);
    } else {
        LOG(INFO) << "cpuset undefined";
    }

    cg.add_controller("cpuacct");

    cg.create_cgroup(1);

    LOG(INFO) << "created cgroup '" << opt.cgroupname << "'";
}

void cgroup_attach(const struct runguard_options &opt) {
    cgroup_guard cg(opt.cgroupname);
    cg.get_cgroup();
    cg.attach_task();
}

void cgroup_kill(const struct runguard_options &opt) {
    void *ptr = nullptr;
    pid_t pid;

    while (true) {
        int ret = cgroup_get_task_begin(opt.cgroupname.c_str(), "memory", &ptr, &pid);
        cgroup_get_task_end(&ptr);
        if (ret == ECGEOF)
            break;
        kill(pid, SIGKILL);
    }
}

void cgroup_delete(const struct runguard_options &opt) {
    cgroup_guard cg(opt.cgroupname);
    cg.add_controller("cpuacct");
    cg.add_controller("memory");

    if (!opt.cpuset.empty()) {
        cg.add_controller("cpuset");
    }

    cg.delete_cgroup();

    LOG(INFO) << "deleted cgroup '" << opt.cgroupname << "'";
}

void set_rlimit(int resource, rlim_t cur, rlim_t max) {
    struct rlimit lim;
    lim.rlim_cur = cur;
    lim.rlim_max = max;
    if (setrlimit(resource, &lim) != 0)
        throw system_error(errno, generic_category(), "setrlimit");
}

void set_restrictions(const struct runguard_options &opt) {
    if (!opt.preserve_sys_env) {
        char *path = getenv("PATH");
        environ[0] = nullptr;
        if (path) setenv("PATH", path, true);
    }

    for (auto &entry : opt.env) {
        std::string env = entry;
        putenv(env.data());
    }

    if (opt.use_cpu_limit) {
        /* Setting the real hard limit one second
		   higher: at the soft limit the kernel will send SIGXCPU at
		   the hard limit a SIGKILL. The SIGXCPU can be caught, but is
		   not by default and gives us a reliable way to detect if the
		   CPU-time limit was reached. */
        rlim_t cputime_limit = (rlim_t)ceil(opt.cpu_limit.hard);
        set_rlimit(RLIMIT_CPU, cputime_limit, cputime_limit + 1);
    }

    // memory limits(RLIMIT_AS, RLIMIT_DATA) are handled by cgroups
    set_rlimit(RLIMIT_AS, RLIM_INFINITY, RLIM_INFINITY);
    set_rlimit(RLIMIT_DATA, RLIM_INFINITY, RLIM_INFINITY);
    // time limits are handled by cgroups

    set_rlimit(RLIMIT_STACK, RLIM_INFINITY, RLIM_INFINITY);

    if (opt.file_limit > 0) set_rlimit(RLIMIT_FSIZE, opt.file_limit, opt.file_limit);
    if (opt.nproc > 0) set_rlimit(RLIMIT_NPROC, opt.nproc, opt.nproc);
    if (opt.no_core_dumps) set_rlimit(RLIMIT_CORE, 0, 0);

    // put child process in the control group
    cgroup_attach(opt);

    // run the command in a separate process group,
    // so the command and all its child processes can be killed
    // off with one signal
    if (setsid() == -1)
        throw system_error(errno, generic_category(), "unable to setsid");

    // set root directory and change working directory
    if (!opt.chroot_dir.empty()) {
        if (chroot(opt.chroot_dir.c_str()) != 0)
            throw system_error(errno, generic_category(), format("unable to chroot to {}", opt.chroot_dir));
        if (chdir("/") != 0)
            throw system_error(errno, generic_category(), "unable to chdir to / in chroot");

        LOG(INFO) << "chrooted to directory " << opt.chroot_dir;
    }

    if (opt.group_id >= 0) {
        if (setgid(opt.group_id))
            throw system_error(errno, generic_category(), "unable to set group id");
        gid_t aux_groups[10];
        aux_groups[0] = opt.group_id;
        if (setgroups(1, aux_groups))
            throw system_error(errno, generic_category(), "unable to clear auxiliary groups");
    }

    if (opt.user_id >= 0) {
        if (setuid(opt.user_id))
            throw system_error(errno, generic_category(), "unable to set user id");
    } else {
        if (setuid(getuid()))
            throw system_error(errno, generic_category(), "unable to reset user id");
    }

    if (geteuid() == 0 || getuid() == 0)
        throw runtime_error("you cannot run user command as root");
}
