#include "server/program.hpp"
#include <fmt/core.h>
#include <glog/logging.h>
#include <stdlib.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include "common/interprocess.hpp"
#include "common/io_utils.hpp"
#include "common/utils.hpp"
#include "common/exceptions.hpp"
#include "config.hpp"
using namespace std;

namespace judge::server {

compilation_error::compilation_error(const string &what)
    : runtime_error(what) {}

compilation_error::compilation_error(const char *what)
    : runtime_error(what) {}

executable::executable(const string &id, const fs::path &workdir, asset_uptr &&asset, const string &md5sum)
    : dir(workdir / "executable" / id), runpath(dir / "compile" / "run"), id(assert_safe_path(id)), md5sum(md5sum), md5path(dir / "md5sum"), deploypath(dir / ".deployed"), buildpath(dir / "compile" / "build"), asset(move(asset)) {
}

void executable::fetch(const string &cpuset, const fs::path &, const fs::path &chrootdir) {
    ip::file_lock file_lock = lock_directory(dir);
    ip::scoped_lock guard(file_lock);
    if (!fs::is_directory(dir) ||
        !fs::is_regular_file(deploypath) ||
        (!md5sum.empty() && !fs::is_regular_file(md5path)) ||
        (!md5sum.empty() && read_file_content(md5path) != md5sum)) {
        fs::remove_all(dir);
        fs::create_directories(dir);

        asset->fetch(dir / "compile");

        if (fs::exists(buildpath)) {
            if (auto ret = call_process(EXEC_DIR / "compile_executable.sh", "-n", cpuset, /* workdir */ dir, chrootdir); ret != 0) {
                switch (ret) {
                    case E_COMPILER_ERROR:
                        throw compilation_error("executable compilation error");
                    default:
                        throw internal_error("unknown exitcode " + to_string(ret));
                }
            }
        }

        if (!fs::exists(runpath)) {
            throw compilation_error("executable malformed");
        }
    }
    ofstream to_be_created(deploypath);
}

void executable::fetch(const string &cpuset, const fs::path &chrootdir) {
    fetch(cpuset, {}, chrootdir);
}

string executable::get_compilation_log(const fs::path &) {
    fs::path compilation_log_file(dir / "compile.out");
    if (!fs::exists(compilation_log_file)) {
        return "No compilation informations";
    } else {
        return read_file_content(compilation_log_file);
    }
}

filesystem::path executable::get_run_path(const filesystem::path &) {
    return runpath;
}

local_executable_asset::local_executable_asset(const string &type, const string &id, const fs::path &execdir)
    : asset(""), execdir(execdir / assert_safe_path(type) / assert_safe_path(id)) {}

void local_executable_asset::fetch(const fs::path &dir) {
    fs::copy(/* from */ execdir, /* to */ dir, fs::copy_options::recursive);
}

remote_executable_asset::remote_executable_asset(asset_uptr &&remote_asset, const string &md5sum)
    : asset(""), md5sum(md5sum), remote_asset(move(remote_asset)) {}

void remote_executable_asset::fetch(const fs::path &dir) {
    fs::path md5path(dir / "md5sum");
    fs::path deploypath(dir / ".deployed");
    fs::path buildpath(dir / "build");
    fs::path runpath(dir / "run");
    fs::path zippath(dir / "executable.zip");

    remote_asset->fetch(zippath);

    if (!md5sum.empty()) {
        // TODO: check for md5 of executable.zip
    }

    LOG(INFO) << "Unzipping executable " << zippath;

    if (system(fmt::format("unzip -Z '{}' | grep -q ^l", zippath.string()).c_str()) == 0)
        throw compilation_error("Executable contains symlinks");

    if (system(fmt::format("unzip -j -q -d {}, {}", dir, zippath).c_str()) != 0)
        throw compilation_error("Unable to unzip executable");
}

local_executable_manager::local_executable_manager(const fs::path &workdir, const fs::path &execdir)
    : workdir(workdir), execdir(execdir) {}

unique_ptr<executable> local_executable_manager::get_compile_script(const string &language) const {
    asset_uptr asset = make_unique<local_executable_asset>("compile", language, execdir);
    return make_unique<executable>("compile-" + language, workdir, move(asset));
}

unique_ptr<executable> local_executable_manager::get_run_script(const string &language) const {
    asset_uptr asset = make_unique<local_executable_asset>("run", language, execdir);
    return make_unique<executable>("run-" + language, workdir, move(asset));
}

unique_ptr<executable> local_executable_manager::get_check_script(const string &language) const {
    asset_uptr asset = make_unique<local_executable_asset>("check", language, execdir);
    return make_unique<executable>("check-" + language, workdir, move(asset));
}

unique_ptr<executable> local_executable_manager::get_compare_script(const string &language) const {
    asset_uptr asset = make_unique<local_executable_asset>("compare", language, execdir);
    return make_unique<executable>("compare-" + language, workdir, move(asset));
}

source_code::source_code(executable_manager &exec_mgr)
    : exec_mgr(exec_mgr) {}

void source_code::fetch(const string &cpuset, const fs::path &workdir, const fs::path &chrootdir) {
    vector<fs::path> paths;
    for (auto &file : source_files) {
        assert_safe_path(file->name);
        auto filepath = workdir / "compile" / file->name;
        fs::create_directories(filepath.parent_path());
        paths.push_back(filepath);
        file->fetch(filepath);
    }

    for (auto &file : assist_files) {
        assert_safe_path(file->name);
        auto filepath = workdir / "compile" / file->name;
        file->fetch(filepath);
    }

    auto exec = exec_mgr.get_compile_script(language);
    exec->fetch(cpuset, chrootdir);
    // compile.sh <compile script> <cpuset> <chrootdir> <workdir> <memlimit> <files...>
    if (auto ret = call_process(EXEC_DIR / "compile.sh", "-n", cpuset, /* compile script */ exec->get_run_path(), cpuset, chrootdir, workdir, memory_limit, /* source files */ paths); ret != 0) {
        switch (ret) {
            case E_COMPILER_ERROR:
                throw compilation_error("Compilation failed");
            case E_INTERNAL_ERROR:
                throw internal_error("Compilation failed because of internal errors");
            default:
                throw runtime_error(fmt::format("Unrecognized compile.sh return code: {}", ret));
        }
    }
}

string source_code::get_compilation_log(const fs::path &workdir) {
    fs::path compilation_log_file(workdir / "compile" / "compile.out");
    if (!fs::exists(compilation_log_file)) {
        return "No compilation informations";
    } else {
        return read_file_content(compilation_log_file);
    }
}

fs::path source_code::get_run_path(const fs::path &path) {
    // run 参见 compile.sh，path 为 $WORKDIR
    return path / "compile" / "run";
}

}  // namespace judge::server