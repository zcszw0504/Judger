#pragma once

#include <filesystem>
#include "server/problem.hpp"
#include "server/status.hpp"

namespace judge::server {
using namespace std;
struct judge_server {
    virtual string category() const = 0;

    /**
     * @brief 初始化评测服务器连接
     * @param 配置文件路径
     * 你可以在这里通过解析配置文件来建立消息队列、建立数据库连接
     * 配置文件的格式自定义，没有任何约定，但建议使用 JSON
     */
    virtual void init(const filesystem::path &config_path) = 0;

    /**
     * @brief 获取一个提交
     * @param submit 存储该提交的信息
     * @return 返回是否获取到一个提交，如果没有能够获取到一个提交，则应该 sleep(100ms)
     * 该函数可能立即返回不阻塞，此时获取到提交才返回 true；
     * 也可能阻塞到有提交为止，此时返回总是 true。
     */
    virtual bool fetch_submission(submission &submit) = 0;

    /**
     * @brief 将提交返回给服务器
     * 该函数是阻塞的。评测系统不需要通过多线程来并发写服务器，因为 server 并不会因为
     * 评测过程而阻塞，获取提交和返回评测结果都能很快完成，因此 server 是单线程的。
     */
    virtual void summarize() = 0;
};
}  // namespace judge::server